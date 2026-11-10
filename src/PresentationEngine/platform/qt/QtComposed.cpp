// QtComposed — Qt-first network, environment, input and notifications (see QtComposed.hpp).

#include "platform/qt/QtComposed.hpp"

#include "platform/qt/QtGuiThread.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QIcon>
#include <QInputDevice>
#include <QLocale>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QSysInfo>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <private/qguiapplication_p.h>
#include <qpa/qplatformsystemtrayicon.h>
#include <qpa/qplatformtheme.h>

#include <algorithm>
#include <memory>

namespace bps::platform {
namespace {

std::string Lower(QString s) { return s.toLower().toStdString(); }

} // namespace

// ===========================================================================================
// Network
// ===========================================================================================
std::string QtNetwork::Hostname() const {
    const QString name = QHostInfo::localHostName();
    return name.isEmpty() ? native_.Hostname() : name.toStdString();
}

std::vector<NetworkAdapterInfo> QtNetwork::Adapters() const {
    std::vector<NetworkAdapterInfo> out;
    for (const QNetworkInterface& nic : QNetworkInterface::allInterfaces()) {
        NetworkAdapterInfo info;
        info.name = nic.humanReadableName().isEmpty() ? nic.name().toStdString() : nic.humanReadableName().toStdString();
        info.mac = Lower(nic.hardwareAddress());   // the contract spells it "aa:bb:cc:dd:ee:ff"
        info.isUp = nic.flags().testFlag(QNetworkInterface::IsUp);
        info.isLoopback = nic.flags().testFlag(QNetworkInterface::IsLoopBack);
        for (const QNetworkAddressEntry& entry : nic.addressEntries()) {
            QHostAddress ip = entry.ip();
            ip.setScopeId(QString());   // "fe80::1%eth0" -> "fe80::1"
            if (ip.protocol() == QAbstractSocket::IPv4Protocol) info.ipv4.push_back(ip.toString().toStdString());
            else if (ip.protocol() == QAbstractSocket::IPv6Protocol) info.ipv6.push_back(ip.toString().toStdString());
        }
        out.push_back(std::move(info));
    }
    return out.empty() ? native_.Adapters() : out;
}

Result<std::string> QtNetwork::IpAddress() const {
    for (const NetworkAdapterInfo& a : Adapters())
        if (a.isUp && !a.isLoopback && !a.ipv4.empty()) return a.ipv4.front();
    return native_.IpAddress();
}

Result<bool> QtNetwork::InternetAvailable() const {
    // Qt's reachability backend where the platform ships one (NetworkListManager, SCNetworkReachability,
    // NetworkManager); "unknown" or no backend falls through to the native default-route check.
    static const bool loaded = QNetworkInformation::loadDefaultBackend();
    if (loaded) {
        if (QNetworkInformation* info = QNetworkInformation::instance()) {
            switch (info->reachability()) {
            case QNetworkInformation::Reachability::Online: return true;
            case QNetworkInformation::Reachability::Disconnected: return false;
            default: break;   // Local / Site / Unknown: ask the native check
            }
        }
    }
    return native_.InternetAvailable();
}

// ===========================================================================================
// Environment
// ===========================================================================================
EnvironmentInfo QtEnvironment::Current() const {
    EnvironmentInfo info = native_.Current();   // CPU model, GPU name, RAM, build id live here
    auto take = [](std::string& field, const QString& qt) { if (!qt.isEmpty()) field = qt.toStdString(); };
    take(info.osName, QSysInfo::prettyProductName());
    take(info.osVersion, QSysInfo::kernelVersion());
    take(info.arch, QSysInfo::currentCpuArchitecture());
    take(info.hostname, QSysInfo::machineHostName());
    take(info.locale, QLocale::system().name().replace(QLatin1Char('_'), QLatin1Char('-')));
    take(info.timezone, QString::fromLatin1(QTimeZone::systemTimeZoneId()));
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty()) user = qEnvironmentVariable("USER");
    if (user.isEmpty()) user = qEnvironmentVariable("LOGNAME");
    take(info.username, user);
    if (info.buildNumber.empty()) info.buildNumber = QSysInfo::productVersion().toStdString();
    if (const int cores = QThread::idealThreadCount(); cores > 0) info.coreCount = static_cast<unsigned>(cores);
    return info;
}

// ===========================================================================================
// Input
// ===========================================================================================
namespace {
InputDeviceKind KindOf(const QInputDevice* d) {
    switch (d->type()) {
    case QInputDevice::DeviceType::Keyboard: return InputDeviceKind::Keyboard;
    case QInputDevice::DeviceType::Mouse: return InputDeviceKind::Mouse;
    case QInputDevice::DeviceType::TouchScreen:
    case QInputDevice::DeviceType::TouchPad: return InputDeviceKind::Touch;
    case QInputDevice::DeviceType::Stylus:
    case QInputDevice::DeviceType::Airbrush:
    case QInputDevice::DeviceType::Puck: return InputDeviceKind::Pen;
    default: return InputDeviceKind::Other;
    }
}
} // namespace

std::vector<InputDeviceInfo> QtInput::Enumerate() const {
    std::vector<InputDeviceInfo> native = native_.Enumerate();
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return native;

    // Qt's list (read on the GUI thread, where its device registry lives).
    struct QtDev { std::string name; InputDeviceKind kind; qint64 systemId; };
    const std::vector<QtDev> qt = OnGuiThreadOr([]() {
        std::vector<QtDev> list;
        for (const QInputDevice* d : QInputDevice::devices())
            if (d->type() != QInputDevice::DeviceType::Unknown && d->type() != QInputDevice::DeviceType::AllDevices)
                list.push_back({d->name().toStdString(), KindOf(d), d->systemId()});
        return list;
    }, std::vector<QtDev>{});

    std::vector<InputDeviceInfo> out;
    std::vector<bool> used(native.size(), false);
    for (const QtDev& d : qt) {
        InputDeviceInfo info;
        info.name = d.name;
        info.kind = d.kind;
        info.id = d.systemId != 0 ? "qt" + std::to_string(d.systemId) : d.name;
        // The native entry of the same name carries the stable id and the device node.
        for (size_t i = 0; i < native.size(); ++i)
            if (!used[i] && native[i].name == d.name) {
                used[i] = true;
                info.id = native[i].id;
                info.path = native[i].path;
                break;
            }
        out.push_back(std::move(info));
    }
    // What Qt does not list at all (gamepads above all) comes from the native enumerator.
    for (size_t i = 0; i < native.size(); ++i)
        if (!used[i] && native[i].kind == InputDeviceKind::Gamepad) out.push_back(native[i]);
    return out.empty() ? native : out;
}

bool QtInput::HasKeyboard() const {
    for (const auto& d : Enumerate()) if (d.kind == InputDeviceKind::Keyboard) return true;
    return false;
}

bool QtInput::HasPointer() const {
    for (const auto& d : Enumerate())
        if (d.kind == InputDeviceKind::Mouse || d.kind == InputDeviceKind::Touch || d.kind == InputDeviceKind::Pen) return true;
    return false;
}

// ===========================================================================================
// Notifications: the platform theme's tray icon is the one cross-OS notification Qt has without Widgets
// (Windows toast balloon, macOS Notification Center, freedesktop.org Notifications over D-Bus).
// ===========================================================================================
namespace {
std::unique_ptr<QPlatformSystemTrayIcon> NewTray() {
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return nullptr;
    QPlatformTheme* theme = QGuiApplicationPrivate::platformTheme();
    if (!theme) return nullptr;
    std::unique_ptr<QPlatformSystemTrayIcon> tray(theme->createPlatformSystemTrayIcon());
    if (!tray || !tray->isSystemTrayAvailable() || !tray->supportsMessages()) return nullptr;
    return tray;
}
} // namespace

bool QtNotifications::Supported() const {
    const bool qt = OnGuiThreadOr([] { return NewTray() != nullptr; }, false);
    return qt || native_.Supported();
}

Result<void> QtNotifications::Show(std::string_view title, std::string_view body) {
    const QString t = QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size()));
    const QString b = QString::fromUtf8(body.data(), static_cast<qsizetype>(body.size()));
    const bool shown = OnGuiThreadOr([&] {
        std::unique_ptr<QPlatformSystemTrayIcon> tray = NewTray();
        if (!tray) return false;
        QIcon icon = QGuiApplication::windowIcon();
        if (icon.isNull()) icon = QIcon(QStringLiteral(":/app/app-icon.ico"));
        tray->init();
        tray->updateIcon(icon);
        tray->showMessage(t, b, icon, QPlatformSystemTrayIcon::Information, 5000);
        // The tray entry has to outlive the balloon; it is removed once the message has had its time.
        QPlatformSystemTrayIcon* raw = tray.release();
        QTimer::singleShot(8000, QCoreApplication::instance(), [raw] { raw->cleanup(); delete raw; });
        return true;
    }, false);
    return shown ? Result<void>(Ok()) : native_.Show(title, body);
}

} // namespace bps::platform
