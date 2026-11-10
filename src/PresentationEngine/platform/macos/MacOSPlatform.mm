#include "platform/macos/MacOSPlatform.hpp"
#include "platform/OsTag.hpp"
#include "platform/qt/QtRecording.hpp"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/ps/IOPowerSources.h>
#import <IOKit/ps/IOPSKeys.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDUsageTables.h>
#import <Foundation/Foundation.h>

#include <QBuffer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTimeZone>
#include <QThread>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/vm_statistics.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <dispatch/dispatch.h>
#include <format>
#include <map>
#include <optional>
#include <pwd.h>
#include <set>
#include <sstream>
#include <utility>

namespace bps::platform {
namespace {

std::string Utf8(NSString* value)
{
    if (!value) return {};
    const char* bytes = value.UTF8String;
    return bytes ? std::string(bytes) : std::string{};
}

NSString* Ns(std::string_view value)
{
    return [[[NSString alloc] initWithBytes:value.data() length:value.size()
                                   encoding:NSUTF8StringEncoding] autorelease];
}

template<class F>
auto OnMainThread(F&& work) -> decltype(work())
{
    using R = decltype(work());
    if (pthread_main_np()) return work();
    __block std::optional<R> result;
    dispatch_sync(dispatch_get_main_queue(), ^{ result.emplace(work()); });
    return std::move(*result);
}

std::string DisplayUuid(CGDirectDisplayID display)
{
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(display);
    if (!uuid) return std::format("display:{}", display);
    CFStringRef text = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    NSString* bridged = (NSString*)text;
    std::string result = Utf8(bridged);
    CFRelease(text);
    CFRelease(uuid);
    return result;
}

std::string Address(const sockaddr* address)
{
    std::array<char, NI_MAXHOST> host{};
    const socklen_t size = address->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    if (getnameinfo(address, size, host.data(), static_cast<socklen_t>(host.size()),
                    nullptr, 0, NI_NUMERICHOST) != 0) return {};
    return host.data();
}

Result<std::optional<std::string>> OpenPanel(std::string_view title,
                                              const std::vector<std::string>& filters,
                                              bool directory)
{
    return OnMainThread([&]() -> Result<std::optional<std::string>> {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = Ns(title);
        panel.canChooseFiles = !directory;
        panel.canChooseDirectories = directory;
        panel.allowsMultipleSelection = NO;
        NSMutableSet<NSString*>* extensions = [NSMutableSet set];
        for (const auto& filter : filters) {
            size_t pos = 0;
            while ((pos = filter.find("*.", pos)) != std::string::npos) {
                pos += 2;
                size_t end = pos;
                while (end < filter.size() && std::isalnum(static_cast<unsigned char>(filter[end]))) ++end;
                if (end > pos) [extensions addObject:Ns(std::string_view(filter).substr(pos, end - pos))];
                pos = end;
            }
        }
        if (extensions.count) panel.allowedFileTypes = extensions.allObjects;
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil) return std::optional<std::string>{};
        return std::optional<std::string>{Utf8(panel.URL.path)};
    });
}

} // namespace

unsigned MacOSThreading::HardwareConcurrency() const
{
    const int count = QThread::idealThreadCount();
    return count > 0 ? static_cast<unsigned>(count) : 1u;
}

Result<void> MacOSThreading::SetCurrentThreadName(std::string_view name)
{
    const std::string value(name);
    const int rc = pthread_setname_np(value.c_str());
    return rc == 0 ? Ok() : Result<void>(Error::Make(Err::IoError, "Threading",
        "pthread_setname_np failed: " + std::string(std::strerror(rc))));
}

std::string MacOSThreading::CurrentThreadName() const
{
    std::array<char, 64> name{};
    return pthread_getname_np(pthread_self(), name.data(), name.size()) == 0
        ? std::string(name.data()) : std::string{};
}

uint64_t MacOSThreading::CurrentThreadId() const
{
    uint64_t id = 0;
    (void)pthread_threadid_np(nullptr, &id);
    return id;
}

Result<void> MacOSThreading::SetThreadAffinity(std::thread&, unsigned)
{
    return Error::Make(Err::Unsupported, "Threading", "macOS does not expose portable CPU affinity");
}

Result<void> MacOSThreading::SetCurrentThreadPriority(int priority)
{
    qos_class_t qos = QOS_CLASS_DEFAULT;
    if (priority >= 4) qos = QOS_CLASS_USER_INTERACTIVE;
    else if (priority == 3) qos = QOS_CLASS_USER_INITIATED;
    else if (priority == 2) qos = QOS_CLASS_UTILITY;
    else if (priority == 1) qos = QOS_CLASS_BACKGROUND;
    const int rc = pthread_set_qos_class_self_np(qos, 0);
    return rc == 0 ? Ok() : Result<void>(Error::Make(Err::IoError, "Threading",
        "pthread_set_qos_class_self_np failed: " + std::string(std::strerror(rc))));
}

std::vector<MonitorInfo> MacOSMonitor::Enumerate() const
{
    uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess || count == 0) return {};
    std::vector<CGDirectDisplayID> ids(count);
    if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess) return {};
    ids.resize(count);
    std::vector<MonitorInfo> out;
    const CGDirectDisplayID primary = CGMainDisplayID();
    int ordinal = 0;
    for (CGDirectDisplayID id : ids) {
        const CGRect bounds = CGDisplayBounds(id);
        MonitorInfo info;
        info.id = DisplayUuid(id);   // the stable key (a CoreGraphics UUID) — what saved settings and capture resolve by
        // What a person reads in a picker: not the UUID. NSScreen::localizedName would be the OS's own
        // wording, but AppKit screens may only be read on the main thread and this is called from engine
        // threads (a main-thread hop from here can deadlock against a busy interface), so the label is
        // built from CoreGraphics data that is safe anywhere: "Built-in · 3024×1964" / "Display 2 · 1920×1080".
        ++ordinal;
        info.name = std::format("{} · {}×{}",
                                CGDisplayIsBuiltin(id) ? std::string("Built-in")
                                                       : std::format("Display {}", ordinal),
                                static_cast<int>(CGDisplayPixelsWide(id)),
                                static_cast<int>(CGDisplayPixelsHigh(id)));
        info.x = static_cast<int>(bounds.origin.x);
        info.y = static_cast<int>(bounds.origin.y);
        info.widthPx = static_cast<int>(CGDisplayPixelsWide(id));
        info.heightPx = static_cast<int>(CGDisplayPixelsHigh(id));
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
        if (mode) {
            info.refreshRateHz = static_cast<int>(std::lround(CGDisplayModeGetRefreshRate(mode)));
            CGDisplayModeRelease(mode);
        }
        info.orientation = static_cast<int>(std::lround(CGDisplayRotation(id))) % 360;
        info.primary = id == primary;
        info.connected = true;
        info.hdrSupported = false;
        const CGSize physical = CGDisplayScreenSize(id);
        if (physical.width > 0)
            info.dpi = static_cast<int>(std::lround(25.4 * info.widthPx / physical.width));
        out.push_back(std::move(info));
    }
    return out;
}

Result<MonitorInfo> MacOSMonitor::Primary() const
{
    auto displays = Enumerate();
    for (const auto& display : displays) if (display.primary) return display;
    if (!displays.empty()) return displays.front();
    return Error::Make(Err::NotFound, "Monitor", "no active displays");
}

std::string MacOSMonitor::DefaultMonitorId() const
{
    const auto primary = Primary();
    return primary.ok() ? primary.value().id : std::string{};
}

std::string MacOSNetwork::Hostname() const
{
    std::array<char, 256> value{};
    return gethostname(value.data(), value.size() - 1) == 0 ? value.data() : std::string{};
}

std::vector<NetworkAdapterInfo> MacOSNetwork::Adapters() const
{
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    std::map<std::string, NetworkAdapterInfo> entries;
    for (ifaddrs* item = list; item; item = item->ifa_next) {
        if (!item->ifa_name) continue;
        auto& info = entries[item->ifa_name];
        info.name = item->ifa_name;
        info.isUp = (item->ifa_flags & IFF_UP) != 0;
        info.isLoopback = (item->ifa_flags & IFF_LOOPBACK) != 0;
        if (!item->ifa_addr) continue;
        if (item->ifa_addr->sa_family == AF_INET || item->ifa_addr->sa_family == AF_INET6) {
            const std::string ip = Address(item->ifa_addr);
            if (!ip.empty()) (item->ifa_addr->sa_family == AF_INET ? info.ipv4 : info.ipv6).push_back(ip);
        } else if (item->ifa_addr->sa_family == AF_LINK) {
            const auto* link = reinterpret_cast<const sockaddr_dl*>(item->ifa_addr);
            const auto* bytes = reinterpret_cast<const unsigned char*>(LLADDR(link));
            if (link->sdl_alen == 6)
                info.mac = std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
        }
    }
    freeifaddrs(list);
    std::vector<NetworkAdapterInfo> out;
    for (auto& [name, adapter] : entries) { (void)name; out.push_back(std::move(adapter)); }
    return out;
}

Result<std::string> MacOSNetwork::IpAddress() const
{
    for (const auto& adapter : Adapters())
        if (adapter.isUp && !adapter.isLoopback && !adapter.ipv4.empty()) return adapter.ipv4.front();
    return Error::Make(Err::NotFound, "Network", "no active non-loopback IPv4 address");
}

Result<std::string> MacOSNetwork::Gateway() const
{
    QProcess route;
    route.start("/sbin/route", {"-n", "get", "default"});
    if (!route.waitForStarted(500) || !route.waitForFinished(1500)) {
        route.kill();
        return Error::Make(Err::Timeout, "Network", "default route query timed out");
    }
    const QString output = QString::fromLocal8Bit(route.readAllStandardOutput());
    const QRegularExpression pattern(QStringLiteral("(?m)^\\s*gateway:\\s*(\\S+)"));
    const auto match = pattern.match(output);
    if (route.exitCode() == 0 && match.hasMatch()) return match.captured(1).toStdString();
    return Error::Make(Err::NotFound, "Network", "no default gateway");
}

Result<bool> MacOSNetwork::InternetAvailable() const { return Gateway().ok(); }

std::vector<std::string> MacOSNetwork::DnsServers() const
{
    QFile file("/etc/resolv.conf");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    std::vector<std::string> out;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.startsWith("nameserver ")) out.push_back(line.mid(11).trimmed().toStdString());
    }
    return out;
}

std::string MacOSNetwork::Proxy() const
{
    const auto env = QProcessEnvironment::systemEnvironment();
    for (const char* key : {"https_proxy", "HTTPS_PROXY", "http_proxy", "HTTP_PROXY", "all_proxy", "ALL_PROXY"}) {
        const QString value = env.value(QString::fromLatin1(key));
        if (!value.isEmpty()) return value.toStdString();
    }
    return {};
}

INetwork::FirewallAccess MacOSNetwork::ProbeInboundAccess(const std::string&, const std::string&) const
{
    return FirewallAccess::Unavailable;
}

INetwork::FirewallRequestOutcome MacOSNetwork::RequestInboundAccess(
    const std::string&, const std::string&, const std::string&, std::string* diag)
{
    if (diag) *diag = "inbound firewall rules are managed by macOS or device management";
    return FirewallRequestOutcome::Unavailable;
}

PowerInfo MacOSPower::Current() const
{
    PowerInfo result;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return result;
    CFArrayRef sources = IOPSCopyPowerSourcesList(info);
    if (!sources) { CFRelease(info); return result; }
    for (CFIndex i = 0; i < CFArrayGetCount(sources); ++i) {
        CFTypeRef source = CFArrayGetValueAtIndex(sources, i);
        CFDictionaryRef description = IOPSGetPowerSourceDescription(info, source);
        if (!description) continue;
        auto type = static_cast<CFStringRef>(CFDictionaryGetValue(description, CFSTR(kIOPSTypeKey)));
        if (!type || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo) continue;
        result.hasBattery = true;
        auto current = static_cast<CFNumberRef>(CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey)));
        auto maximum = static_cast<CFNumberRef>(CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey)));
        int cur = 0, max = 0;
        if (current && maximum && CFNumberGetValue(current, kCFNumberIntType, &cur) &&
            CFNumberGetValue(maximum, kCFNumberIntType, &max) && max > 0)
            result.batteryPercent = std::clamp(cur * 100 / max, 0, 100);
        auto state = static_cast<CFStringRef>(CFDictionaryGetValue(description, CFSTR(kIOPSPowerSourceStateKey)));
        result.onBattery = state && CFStringCompare(state, CFSTR(kIOPSBatteryPowerValue), 0) == kCFCompareEqualTo;
        auto charging = static_cast<CFBooleanRef>(CFDictionaryGetValue(description, CFSTR(kIOPSIsChargingKey)));
        result.charging = charging && CFBooleanGetValue(charging);
        break;
    }
    CFRelease(sources);
    CFRelease(info);
    return result;
}

EnvironmentInfo MacOSEnvironment::Current() const
{
    EnvironmentInfo info;
    info.osName = "macOS " + QSysInfo::productVersion().toStdString();
    info.osVersion = QSysInfo::kernelVersion().toStdString();
    info.arch = CompileArch();
    info.username = qEnvironmentVariable("USER").toStdString();
    if (info.username.empty()) {
        if (const passwd* user = getpwuid(getuid()); user && user->pw_name)
            info.username = user->pw_name;
    }
    info.hostname = MacOSNetwork{}.Hostname();
    const QLocale locale = QLocale::system();
    info.locale = locale.name().toStdString();
    info.timezone = QTimeZone::systemTimeZoneId().toStdString();
    const int cores = QThread::idealThreadCount();
    info.coreCount = cores > 0 ? static_cast<unsigned>(cores) : 1u;
    size_t size = sizeof(info.totalRamBytes);
    (void)sysctlbyname("hw.memsize", &info.totalRamBytes, &size, nullptr, 0);
    std::array<char, 128> cpu{};
    size = cpu.size();
    if (sysctlbyname("machdep.cpu.brand_string", cpu.data(), &size, nullptr, 0) == 0)
        info.cpuModel = cpu.data();
    utsname system{};
    if (uname(&system) == 0) info.buildNumber = system.version;
    return info;
}

std::vector<InputDeviceInfo> MacOSInput::Enumerate() const
{
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) return {};
    IOHIDManagerSetDeviceMatching(manager, nullptr);
    if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        CFRelease(manager);
        return {};
    }
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    std::vector<InputDeviceInfo> out;
    if (devices) {
        const CFIndex count = CFSetGetCount(devices);
        std::vector<const void*> values(static_cast<size_t>(count));
        CFSetGetValues(devices, values.data());
        for (const void* raw : values) {
            auto device = static_cast<IOHIDDeviceRef>(const_cast<void*>(raw));
            auto number = [device](CFStringRef key) {
                int value = 0;
                CFTypeRef rawValue = IOHIDDeviceGetProperty(device, key);
                if (rawValue && CFGetTypeID(rawValue) == CFNumberGetTypeID())
                    CFNumberGetValue(static_cast<CFNumberRef>(rawValue), kCFNumberIntType, &value);
                return value;
            };
            const int page = number(CFSTR(kIOHIDPrimaryUsagePageKey));
            const int usage = number(CFSTR(kIOHIDPrimaryUsageKey));
            InputDeviceKind kind = InputDeviceKind::Other;
            if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Keyboard) kind = InputDeviceKind::Keyboard;
            else if (page == kHIDPage_GenericDesktop && (usage == kHIDUsage_GD_Mouse || usage == kHIDUsage_GD_Pointer)) kind = InputDeviceKind::Mouse;
            else if (page == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_Joystick) kind = InputDeviceKind::Gamepad;
            CFStringRef product = static_cast<CFStringRef>(IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey)));
            uint64_t id = 0;
            const io_service_t service = IOHIDDeviceGetService(device);
            if (service) (void)IORegistryEntryGetRegistryEntryID(service, &id);
            out.push_back({std::format("hid:{:x}", id), Utf8((NSString*)product), kind, {}});
        }
        CFRelease(devices);
    }
    (void)IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    return out;
}

bool MacOSInput::HasKeyboard() const
{
    const auto devices = Enumerate();
    return std::any_of(devices.begin(), devices.end(), [](const auto& d) { return d.kind == InputDeviceKind::Keyboard; });
}

bool MacOSInput::HasPointer() const
{
    const auto devices = Enumerate();
    return std::any_of(devices.begin(), devices.end(), [](const auto& d) {
        return d.kind == InputDeviceKind::Mouse || d.kind == InputDeviceKind::Touch || d.kind == InputDeviceKind::Pen;
    });
}

Result<std::optional<std::string>> MacOSDialogs::OpenFileDialog(
    std::string_view title, const std::vector<std::string>& filters)
{
    return OpenPanel(title, filters, false);
}

Result<std::optional<std::string>> MacOSDialogs::SelectFolderDialog(std::string_view title)
{
    return OpenPanel(title, {}, true);
}

Result<std::optional<std::string>> MacOSDialogs::SaveFileDialog(
    std::string_view title, std::string_view defaultName)
{
    return OnMainThread([&]() -> Result<std::optional<std::string>> {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.title = Ns(title);
        panel.nameFieldStringValue = Ns(defaultName);
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil) return std::optional<std::string>{};
        return std::optional<std::string>{Utf8(panel.URL.path)};
    });
}

Result<void> MacOSDialogs::MessageDialog(std::string_view title, std::string_view message)
{
    return OnMainThread([&]() -> Result<void> {
        NSAlert* alert = [NSAlert new];
        alert.messageText = Ns(title);
        alert.informativeText = Ns(message);
        [alert addButtonWithTitle:@"OK"];
        (void)[alert runModal];
        [alert release];
        return Ok();
    });
}

Result<bool> MacOSDialogs::QuestionDialog(std::string_view title, std::string_view message)
{
    return OnMainThread([&]() -> Result<bool> {
        NSAlert* alert = [NSAlert new];
        alert.messageText = Ns(title);
        alert.informativeText = Ns(message);
        [alert addButtonWithTitle:@"Yes"];
        [alert addButtonWithTitle:@"No"];
        const bool answer = [alert runModal] == NSAlertFirstButtonReturn;
        [alert release];
        return answer;
    });
}

Result<void> MacOSNotifications::Show(std::string_view title, std::string_view body)
{
    return OnMainThread([&]() -> Result<void> {
        NSUserNotification* notification = [NSUserNotification new];
        notification.title = Ns(title);
        notification.informativeText = Ns(body);
        [[NSUserNotificationCenter defaultUserNotificationCenter] deliverNotification:notification];
        [notification release];
        return Ok();
    });
}

std::string MacOSPlatform::OsVersion() const { return QSysInfo::productVersion().toStdString(); }
std::string MacOSPlatform::Arch() const { return CompileArch(); }

PlatformCapabilities MacOSPlatform::Capabilities() const
{
    PlatformCapabilities c;
    c.audioDeviceDiscovery = true;
    c.audioInputCapture = true;  // Qt Multimedia capture tap feeds meters and the mixer
    c.audioOutputPlayback = true; // Qt Multimedia render callbacks feed the program and outputs
    c.audioStreamTaps = true;    // decoded media buffers + pushed NDI audio share the mixer tap ring
    c.audioLoopbackCapture = true; // output metering: measured in the app's own render stream (QtAudioDevices::OutputLevels)
    c.videoDeviceDiscovery = true;
    c.videoCapture = true; // asynchronous Qt Multimedia camera preview
    c.audioRecording = true; // WAV (portable writer) plus M4A/MP3/FLAC where the Qt backend encodes them
    c.videoRecording = QtVideoRecordingSupported(); // QMediaRecorder (Qt Multimedia, FFmpeg backend): MP4/MKV/MOV/WebM
    const QString mediaBackend = qEnvironmentVariable("QT_MEDIA_BACKEND").toLower();
    c.screenCapture = QGuiApplication::instance()
        && QGuiApplication::platformName() == QStringLiteral("cocoa")
        && (mediaBackend.isEmpty() || mediaBackend == QStringLiteral("ffmpeg"));
    c.displayEnumeration = true;
    c.batteryTelemetry = true;
    c.desktopNotifications = true;
    c.nativeDialogs = true;
    c.clipboardImages = true;
    // Qt Multimedia supplies audio I/O and camera preview. Application stream
    // taps and screen capture remain separate platform capabilities.
    return c;
}

SystemInfo MacOSPlatform::Info()
{
    const EnvironmentInfo env = environment_.Current();
    SystemInfo info;
    info.osName = "macOS";
    info.osVersion = env.osVersion;
    info.buildNumber = env.buildNumber;
    info.arch = env.arch;
    info.hostname = env.hostname;
    info.username = env.username;
    info.language = locale_.Language();
    info.timezone = env.timezone;
    info.coreCount = env.coreCount;
    info.totalRamBytes = env.totalRamBytes;
    return info;
}

Snapshot MacOSPlatform::Sample()
{
    Snapshot sample;
    const EnvironmentInfo env = environment_.Current();
    const PowerInfo power = power_.Current();
    sample.totalRamBytes = env.totalRamBytes;
    sample.coreCount = env.coreCount;
    const QStorageInfo root = QStorageInfo::root();
    if (root.isValid() && root.isReady()) {
        sample.diskTotalBytes = static_cast<uint64_t>(std::max<qint64>(0, root.bytesTotal()));
        sample.diskFreeBytes = static_cast<uint64_t>(std::max<qint64>(0, root.bytesAvailable()));
    }
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t vmCount = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm), &vmCount) == KERN_SUCCESS) {
        const uint64_t availablePages = static_cast<uint64_t>(vm.free_count)
            + static_cast<uint64_t>(vm.inactive_count)
            + static_cast<uint64_t>(vm.speculative_count);
        sample.availableRamBytes = availablePages * static_cast<uint64_t>(vm_page_size);
    }
    natural_t processorCount = 0;
    processor_info_array_t cpuInfo = nullptr;
    mach_msg_type_number_t cpuCount = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                            &processorCount, &cpuInfo, &cpuCount) == KERN_SUCCESS) {
        uint64_t total = 0;
        uint64_t idle = 0;
        for (natural_t processor = 0; processor < processorCount; ++processor) {
            const auto* ticks = reinterpret_cast<const processor_cpu_load_info_t>(
                cpuInfo + processor * CPU_STATE_MAX);
            for (int state = 0; state < CPU_STATE_MAX; ++state)
                total += ticks->cpu_ticks[state];
            idle += ticks->cpu_ticks[CPU_STATE_IDLE];
        }
        sample.cpuTotalJiffies = total;
        sample.cpuIdleJiffies = idle;
        (void)vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(cpuInfo),
                            static_cast<vm_size_t>(cpuCount) * sizeof(integer_t));
    }
    proc_taskinfo task{};
    if (proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &task, sizeof(task)) == sizeof(task))
        sample.threadCount = task.pti_threadnum;
    sample.batteryPercent = power.hasBattery ? power.batteryPercent : -1;
    sample.onBattery = power.onBattery;
    sample.batteryCharging = power.charging;
    return sample;
}

std::vector<OsEvent> MacOSPlatform::PollChanges()
{
    std::vector<OsEvent> events;
    const auto monitors = monitor_.Enumerate();
    std::ostringstream monitorIds;
    std::ostringstream resolutions;
    for (const auto& monitor : monitors) {
        monitorIds << monitor.id << ';';
        resolutions << monitor.id << '=' << monitor.widthPx << 'x' << monitor.heightPx << ';';
    }

    std::ostringstream network;
    for (const auto& adapter : network_.Adapters()) {
        network << adapter.name << ':' << adapter.isUp << ':' << adapter.mac << ':';
        for (const auto& address : adapter.ipv4) network << address << ',';
        network << ':';
        for (const auto& address : adapter.ipv6) network << address << ',';
        network << ';';
    }

    const std::string monitorIdsNow = monitorIds.str();
    const std::string resolutionsNow = resolutions.str();
    const std::string networkNow = network.str();
    const std::string audioNow = audio_.Fingerprint();
    const std::string videoNow = video_.Fingerprint();
    const PowerInfo powerNow = power_.Current();

    if (havePollBaseline_) {
        if (monitorIdsNow != lastMonitorIds_) {
            for (const auto& monitor : monitors) {
                if (lastMonitorIds_.find(monitor.id + ';') == std::string::npos)
                    events.push_back({OsEventType::MonitorConnected, monitor.id});
            }
            std::istringstream previous(lastMonitorIds_);
            std::string id;
            while (std::getline(previous, id, ';')) {
                if (!id.empty() && monitorIdsNow.find(id + ';') == std::string::npos)
                    events.push_back({OsEventType::MonitorDisconnected, id});
            }
        }

        if (resolutionsNow != lastResolutions_) {
            for (const auto& monitor : monitors) {
                if (lastMonitorIds_.find(monitor.id + ';') == std::string::npos) continue;
                const std::string current = monitor.id + '=' + std::to_string(monitor.widthPx)
                    + 'x' + std::to_string(monitor.heightPx) + ';';
                if (lastResolutions_.find(current) == std::string::npos)
                    events.push_back({OsEventType::ResolutionChanged,
                        monitor.id + ' ' + std::to_string(monitor.widthPx) + 'x'
                            + std::to_string(monitor.heightPx)});
            }
        }

        if (networkNow != lastNetworkFingerprint_)
            events.push_back({OsEventType::NetworkChanged, "network adapter configuration changed"});
        auto appendDeviceChanges = [&](const std::string& previous, const std::string& current,
                                       std::string_view type) {
            if (previous == current) return;
            auto tokens = [](const std::string& value) {
                std::set<std::string> result;
                std::istringstream input(value);
                std::string token;
                while (std::getline(input, token, '|'))
                    if (!token.empty()) result.insert(std::move(token));
                return result;
            };
            const auto oldDevices = tokens(previous);
            const auto newDevices = tokens(current);
            for (const auto& id : newDevices)
                if (!oldDevices.contains(id))
                    events.push_back({OsEventType::DeviceConnected,
                        std::string(type) + ':' + id});
            for (const auto& id : oldDevices)
                if (!newDevices.contains(id))
                    events.push_back({OsEventType::DeviceRemoved,
                        std::string(type) + ':' + id});
        };
        appendDeviceChanges(lastAudioFingerprint_, audioNow, "audio");
        appendDeviceChanges(lastVideoFingerprint_, videoNow, "video");

        if (lastPower_ && (lastPower_->onBattery != powerNow.onBattery
            || lastPower_->charging != powerNow.charging
            || lastPower_->batteryPercent != powerNow.batteryPercent)) {
            events.push_back({OsEventType::PowerChanged,
                std::string(powerNow.onBattery ? "on battery" : "on AC") + " ("
                    + std::to_string(powerNow.batteryPercent) + "%)"});
        }
        if (lastPower_ && powerNow.onBattery && powerNow.batteryPercent >= 0
            && powerNow.batteryPercent <= 20
            && !(lastPower_->onBattery && lastPower_->batteryPercent <= 20)) {
            events.push_back({OsEventType::BatteryLow, std::to_string(powerNow.batteryPercent) + "%"});
        }
    }

    havePollBaseline_ = true;
    lastMonitorIds_ = monitorIdsNow;
    lastResolutions_ = resolutionsNow;
    lastNetworkFingerprint_ = networkNow;
    lastAudioFingerprint_ = audioNow;
    lastVideoFingerprint_ = videoNow;
    lastPower_ = powerNow;
    return events;
}

} // namespace bps::platform
