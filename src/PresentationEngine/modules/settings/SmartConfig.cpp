#include "modules/settings/SmartConfig.hpp"

#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace bps::settings {

namespace {

constexpr uint64_t kGb = 1024ull * 1024ull * 1024ull;

bool Contains(std::string text, std::string word) {
    auto lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    lower(text);
    lower(word);
    return text.find(word) != std::string::npos;
}

// What the machine's hardware encoder is called - shown to the user only, never used to decide anything.
std::string EncoderName(const std::string& gpuName) {
    if (Contains(gpuName, "nvidia")) return "NVENC";
    if (Contains(gpuName, "radeon") || Contains(gpuName, "amd")) return "AMF";
    if (Contains(gpuName, "intel")) return "Quick Sync";
    return "Hardware encoder";
}

// A graphics adapter is there when the platform reported its video memory or, failing that, at least its name.
bool HasGpu(const adaptive::HardwareInfo& hw) { return hw.gpuDetected || !hw.gpuName.empty(); }

std::string Plural(unsigned n, std::string_view one, std::string_view many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}

// Bytes as whole GB, rounded to nearest (the number the user reads).
uint64_t BytesToGb(uint64_t bytes) { return (bytes + kGb / 2) / kGb; }

// "SSD · 120 GB free of 512 GB" - empty when the PAL reported no volume.
std::string StorageSummary(const adaptive::HardwareInfo& hw) {
    if (hw.diskTotalBytes == 0)
        return {};
    return std::format("{}{} GB free of {} GB", hw.storageIsSsd ? "SSD · " : "",
                       BytesToGb(hw.diskFreeBytes), BytesToGb(hw.diskTotalBytes));
}

// Whether the free space is low enough that the storage advice fires.
bool StorageIsLow(const adaptive::HardwareInfo& hw) {
    return hw.diskTotalBytes > 0 && hw.diskFreeBytes < 5 * kGb;
}

} // namespace

std::string RecommendProfile(const adaptive::HardwareInfo& hw) {
    if (hw.hasBattery && hw.onBattery) return "powerSaver";
    if (!HasGpu(hw) || hw.totalRamBytes < 8 * kGb || hw.coreCount < 4) return "balanced";
    return "performance";
}

HardwareReport BuildHardwareReport(const adaptive::HardwareInfo& hw, bool hardwareEncode, AudioCounts audio) {
    HardwareReport report;

    // ---- What the machine HAS (the "Hardware detected" rows) ----
    report.rows.push_back({ "cpu", "CPU",
                            hw.coreCount > 0 ? Plural(hw.coreCount, "core", "cores") : std::string("Not detected"),
                            hw.coreCount >= 4 });

    report.rows.push_back({ "memory", "Memory",
                            hw.totalRamBytes > 0 ? std::format("{} GB", BytesToGb(hw.totalRamBytes)) : std::string("Not detected"),
                            hw.totalRamBytes >= 8 * kGb });

    HardwareRow gpu{ "gpu", "GPU", {}, HasGpu(hw) };
    if (HasGpu(hw)) {
        gpu.value = hw.gpuName.empty() ? std::string("Graphics adapter") : hw.gpuName;
        if (hw.gpuVramTotalBytes > 0)
            gpu.value += std::format(" · {} GB", std::max<uint64_t>(1, BytesToGb(hw.gpuVramTotalBytes)));
    } else {
        gpu.value = "Not detected - software rendering";
    }
    report.rows.push_back(std::move(gpu));

    if (const std::string storage = StorageSummary(hw); !storage.empty())
        report.rows.push_back({ "storage", hw.storageIsSsd ? "Storage · SSD" : "Storage", storage, !StorageIsLow(hw) });

    report.rows.push_back({ "encoder", "Encoder",
                            hardwareEncode ? std::format("{} available", EncoderName(hw.gpuName)) : std::string("Software encoding only"),
                            hardwareEncode });

    report.rows.push_back({ "audio", "Audio devices",
                            audio.outputs + audio.inputs == 0
                                ? std::string("None found")
                                : std::format("{}, {}", Plural(audio.outputs, "output", "outputs"), Plural(audio.inputs, "input", "inputs")),
                            audio.outputs + audio.inputs > 0 });

    report.rows.push_back({ "displays", "Displays",
                            hw.displayCount > 0 ? std::format("{} connected", hw.displayCount) : std::string("None found"),
                            hw.displayCount > 0 });

    // A desktop has no battery row (nothing to say); a laptop always does.
    if (hw.hasBattery)
        report.rows.push_back({ "battery", "Battery",
                                hw.batteryPercent >= 0
                                    ? (hw.onBattery ? std::format("On battery · {}%", hw.batteryPercent)
                                                    : std::format("Plugged in · {}%", hw.batteryPercent))
                                    : (hw.onBattery ? std::string("On battery") : std::string("Plugged in")),
                                !hw.onBattery });

    if (hw.thermalCelsius > 0)
        report.rows.push_back({ "thermal", "Thermals",
                                std::format("{} °C", static_cast<int>(hw.thermalCelsius + 0.5)),
                                hw.thermalCelsius < 85.0 });

    // ---- The recommendation ----
    report.recommendedProfile = RecommendProfile(hw);
    report.headline = "Recommended setup detected for this hardware";
    if (report.recommendedProfile == "powerSaver")
        report.detail = "This machine is running on battery, so Power Saver keeps the show going longest.";
    else if (report.recommendedProfile == "balanced")
        report.detail = "GPU, memory and cores are modest, so Balanced keeps rendering and encoding in step.";
    else
        report.detail = "GPU, encoder, audio and display capabilities analyzed - this machine can run the Performance profile.";

    // ---- What that means for running the app (the config advice) ----
    // Written the way a helpful engineer would: name the finding, then say what
    // to do about it. `warnings` is what the summary counts - the "info" lines
    // are things worth knowing, never things to fix.
    int warnings = 0;
    const auto advise = [&report](std::string key, std::string severity, std::string text) {
        report.advice.push_back({ std::move(key), std::move(severity), std::move(text) });
    };
    const auto warn = [&advise, &warnings](std::string key, std::string text) {
        ++warnings;
        advise(std::move(key), "warn", std::move(text));
    };

    if (hw.hasBattery && hw.onBattery)
        warn("battery", "Running on battery. Power Saver keeps the show going longest, and plugging in restores the full profile.");
    if (hw.hasBattery && !hw.onBattery && hw.batteryPercent >= 0 && hw.batteryPercent <= 20)
        warn("battery", "Battery under 20%. Plug in - a service can outlast this charge.");
    if (hw.totalRamBytes > 0 && hw.totalRamBytes < 8 * kGb)
        warn("memory", "Under 8 GB of memory. The engine keeps its caches small here, so close other apps before going live.");
    else if (hw.totalRamBytes >= 32 * kGb)
        advise("memory", "info", "Plenty of memory - the engine can preload more of your library up front.");
    if (!HasGpu(hw))
        warn("gpu", "No graphics adapter detected. Everything renders on the CPU, so keep the preview off while live.");
    if (!hardwareEncode)
        advise("encoder", "info", "No hardware encoder - recording and streaming use the CPU, which competes with rendering.");
    if (hw.coreCount > 0 && hw.coreCount < 4)
        warn("cpu", "Fewer than 4 cores. The engine limits background work to protect the live output.");
    else if (hw.coreCount >= 8)
        advise("cpu", "info", "8 or more cores - encoding and the preview run alongside the live output comfortably.");
    if (StorageIsLow(hw))
        warn("storage", "Under 5 GB of disk space free. Thumbnails and caches may fail to write.");
    if (hw.displayCount <= 0)
        warn("displays", "No display detected. Check the output connection before going live.");
    if (hw.thermalCelsius >= 85.0)
        warn("thermal", "The CPU is already warm. The engine will ease off background work to keep it cool.");
    if (report.advice.empty())
        advise("overall", "ok", "Everything the engine needs is present. Start with the recommended profile and only adjust if the meters run hot.");

    report.adviceSummary = warnings == 0
        ? std::string("No problems detected for this machine.")
        : std::format("{} thing{} to look at before going live.", warnings, warnings == 1 ? "" : "s");
    return report;
}

AudioCounts CountAudioDevices() {
    AudioCounts counts;
    for (const auto& d : platform::PlatformAccessor::Get().Audio().Enumerate())
        (d.isInput ? counts.inputs : counts.outputs)++;
    return counts;
}

HardwareReport CurrentHardwareReport() {
    auto& runtime = adaptive::AdaptiveRuntime::Instance();
    return BuildHardwareReport(runtime.Hardware(), runtime.Supports(adaptive::Capability::HardwareVideoEncode), CountAudioDevices());
}

Result<void> ApplyToEngine(const AppSettings& settings, adaptive::AdaptiveRuntime& runtime) {
    using adaptive::ConfigLayer;
    using adaptive::Preference;
    using adaptive::UserMode;

    const std::string mode = settings.Mode();
    const std::string profile = settings.Profile();

    ConfigLayer layer = ConfigLayer::Automatic;
    Preference preference = Preference::None;
    if (mode == "strict") { layer = ConfigLayer::Assisted; preference = Preference::PreferLowMemory; }
    else if (mode == "manual") layer = ConfigLayer::Expert;

    UserMode userMode = profile == "balanced" ? UserMode::Balanced
                      : profile == "powerSaver" ? UserMode::Battery : UserMode::Performance;
    if (settings.GetBool("appearance.lockInMode")) userMode = UserMode::Presentation;

    if (auto r = runtime.SetConfigLayer(layer); !r.ok()) return r;
    if (auto r = runtime.SetPreference(preference); !r.ok()) return r;
    if (auto r = runtime.SetUserMode(userMode); !r.ok()) return r;

    const ResourceCaps caps = settings.EffectiveCaps();
    if (auto r = runtime.SetResourceCaps(static_cast<unsigned>(caps.gpuPct), static_cast<unsigned>(caps.cpuPct)); !r.ok()) return r;

    const std::string level = settings.GetString("notifications.logLevel");
    Logger::Instance().SetGlobalLevel(level == "error" ? LogLevel::Error
                                    : level == "warning" ? LogLevel::Warning
                                    : level == "debug" ? LogLevel::Debug : LogLevel::Info);
    return {};
}

} // namespace bps::settings
