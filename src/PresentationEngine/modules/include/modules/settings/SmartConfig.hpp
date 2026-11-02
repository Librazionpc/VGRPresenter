#pragma once

#include "core/common/Common.hpp"
#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/adaptive/Hardware.hpp"
#include "modules/settings/AppSettings.hpp"

#include <string>
#include <vector>

namespace bps::settings {
struct HardwareRow {
    std::string key;     // gpu | encoder | audio | displays
    std::string label;
    std::string value;
    bool ok = false;     // detected / usable (the green check)
};

struct AudioCounts { unsigned outputs = 0; unsigned inputs = 0; };

// One line of CONFIG ADVICE: what the engine found and what to do about it. Severity is the UI's cue —
// "ok" (nothing to do), "info" (worth knowing) or "warn" (act before going live).
struct AdviceRow {
    std::string key;        // cpu | memory | gpu | storage | encoder | audio | displays | battery | thermal
    std::string severity;   // "ok" | "info" | "warn"
    std::string text;
};

struct HardwareReport {
    std::vector<HardwareRow> rows;    // what the machine HAS (the "Hardware detected" card)
    std::vector<AdviceRow> advice;    // what that means for running the app (the advice card)
    std::string recommendedProfile;   // "performance" | "balanced" | "powerSaver"
    std::string headline;             // "Recommended setup detected for this hardware"
    std::string detail;               // one line on why
    std::string adviceSummary;        // one line over the advice list
};

// The profile that suits this machine: a laptop running on battery -> Power Saver; a machine without a GPU, with under
// 8 GB of RAM or under 4 cores -> Balanced; anything stronger -> Performance.
std::string RecommendProfile(const adaptive::HardwareInfo& hw);

HardwareReport BuildHardwareReport(const adaptive::HardwareInfo& hw, bool hardwareEncode, AudioCounts audio);

// The report for THIS machine: the adaptive runtime's hardware view, its encode capability, and the PAL's audio devices.
AudioCounts CountAudioDevices();
HardwareReport CurrentHardwareReport();
HardwareReport CurrentHardwareReport(AudioCounts audio);

Result<void> ApplyToEngine(const AppSettings& settings, adaptive::AdaptiveRuntime& runtime);
} // namespace bps::settings