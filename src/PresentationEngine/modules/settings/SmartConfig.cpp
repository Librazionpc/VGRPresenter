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

} // namespace

std::string RecommendProfile(const adaptive::HardwareInfo& hw) {
    if (hw.hasBattery && hw.onBattery) return "powerSaver";
    if (!HasGpu(hw) || hw.totalRamBytes < 8 * kGb || hw.coreCount < 4) return "balanced";
    return "performance";
}

HardwareReport BuildHardwareReport(const adaptive::HardwareInfo& hw, bool hardwareEncode, AudioCounts audio) {
    HardwareReport report;

    HardwareRow gpu{ "gpu", "GPU", {}, HasGpu(hw) };
    if (HasGpu(hw)) {
        gpu.value = hw.gpuName.empty() ? std::string("Graphics adapter") : hw.gpuName;
        if (hw.gpuVramTotalBytes > 0)
            gpu.value += std::format(" · {} GB", std::max<uint64_t>(1, (hw.gpuVramTotalBytes + kGb / 2) / kGb));
    } else {
        gpu.value = "Not detected - software rendering";
    }
    report.rows.push_back(std::move(gpu));

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

    report.recommendedProfile = RecommendProfile(hw);
    report.headline = "Recommended setup detected for this hardware";
    if (report.recommendedProfile == "powerSaver")
        report.detail = "This machine is running on battery, so Power Saver keeps the show going longest.";
    else if (report.recommendedProfile == "balanced")
        report.detail = "GPU, memory and cores are modest, so Balanced keeps rendering and encoding in step.";
    else
        report.detail = "GPU, encoder, audio and display capabilities analyzed - this machine can run the Performance profile.";
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
