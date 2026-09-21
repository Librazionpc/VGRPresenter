#pragma once

// Smart Config: what the engine found on this machine, which resource profile suits it, and putting the user's settings
// (configuration mode, resource profile, budgets, Lock In Mode, log level) to work in the engine. The screens only show
// what these return - the wording of a row, the recommendation and the mapping onto the adaptive runtime live here.

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

struct HardwareReport {
    std::vector<HardwareRow> rows;
    std::string recommendedProfile;   // "performance" | "balanced" | "powerSaver"
    std::string headline;             // "Recommended setup detected for this hardware"
    std::string detail;               // one line on why
};

// The profile that suits this machine: a laptop running on battery -> Power Saver; a machine without a GPU, with under
// 8 GB of RAM or under 4 cores -> Balanced; anything stronger -> Performance.
std::string RecommendProfile(const adaptive::HardwareInfo& hw);

HardwareReport BuildHardwareReport(const adaptive::HardwareInfo& hw, bool hardwareEncode, AudioCounts audio);

// The report for THIS machine: the adaptive runtime's hardware view, its encode capability, and the PAL's audio devices.
AudioCounts CountAudioDevices();
HardwareReport CurrentHardwareReport();

// Applies the settings to the running engine:
//   configuration mode  Smart -> Automatic layer, Strict -> Assisted layer (prefers a low memory footprint), Manual -> Expert
//   resource profile    Performance / Balanced / Power Saver -> the runtime's Performance / Balanced / Battery mode
//   Lock In Mode        the runtime's Presentation mode (background work paused) while it is on
//   budgets             the profile's own GPU / CPU caps (Smart, Strict) or the user's numbers (Manual)
//   log level           the logger's global level
Result<void> ApplyToEngine(const AppSettings& settings, adaptive::AdaptiveRuntime& runtime);

} // namespace bps::settings
