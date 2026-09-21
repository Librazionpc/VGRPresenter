#pragma once

// The app's settings (Settings > General and Settings > Smart Config): ONE place that knows what can be set, what the
// defaults are, what values are allowed, where they are kept, and what they mean for the rest of the engine. A screen
// draws whatever Definitions() lists and calls Set(); it never carries a default, a list of options or a number of its
// own. Every write is checked here (a wrong type, an unknown choice or a number out of range is refused and nothing
// changes), saved, and announced to the listeners, so the app reacts to a change the moment the engine accepts it.
//
// The numbers that shape the resource profile (how much of the GPU and CPU each profile may use, and how it splits
// across rendering / encoding / output) are the engine's defaults too - see CapsFor() and AllocationFor().

#include "core/common/Common.hpp"
#include "core/config/Json.hpp"
#include "modules/presentation/ScriptureSlides.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bps::settings {

enum class SettingKind { Bool, Int, Choice, Text };

// One allowed value of a Choice (or one suggested value of an Int, whose `value` is then the number as text).
struct SettingChoice {
    std::string value;          // what is stored
    std::string label;          // what the screen shows
    std::string description;    // a line under the label (may be empty)
    std::string color;          // a colour to draw with it (the accent choices), else empty
    std::string colorLight;     // its lighter variant
    bool recommended = false;   // filled in at query time, never stored
};

struct SettingDef {
    std::string key;            // "group.name"
    std::string group;          // appearance | startup | preferences | backups | notifications | resources | smart | session
    std::string label;
    std::string description;
    SettingKind kind = SettingKind::Bool;
    json::Value dflt;
    long long min = 0, max = 0; // Int without choices: the allowed range
    std::vector<SettingChoice> choices;   // Choice: the allowed values; Int: when non-empty, the allowed values
};

// The share of the machine a resource profile may use, and how that share is split up.
struct ResourceCaps { int gpuPct = 100; int cpuPct = 100; };
struct ProfileAllocation { int renderingPct = 0; int encodingPct = 0; int outputPct = 0; };

class AppSettings {
public:
    using Listener = std::function<void(const std::string& key)>;

    explicit AppSettings(std::string storageFile);

    // Reads the file (a missing file is just the defaults; a damaged one is reported and the defaults are used).
    Result<void> Load();

    // ---- the schema ----
    static const std::vector<SettingDef>& Definitions();
    static const SettingDef* Find(std::string_view key);

    // ---- values ----
    // Unknown keys read as null / false / 0 / "" - never as a crash.
    json::Value Get(std::string_view key) const;
    bool GetBool(std::string_view key) const;
    long long GetInt(std::string_view key) const;
    std::string GetString(std::string_view key) const;

    // Checks the value against the definition, stores it, saves the file and tells the listeners. A value equal to the
    // current one changes nothing (and tells nobody).
    Result<void> Set(std::string_view key, const json::Value& value);
    Result<void> Reset(std::string_view key);
    // Every setting back to its default (the session memory - last show and view - is kept). Returns how many changed.
    size_t ResetAll();

    // Listeners run on the thread that made the change, after the lock is released.
    size_t Subscribe(Listener listener);
    void Unsubscribe(size_t id);

    // ---- what the settings mean ----
    // The resource profile ("performance" | "balanced" | "powerSaver") and the configuration mode ("strict" | "smart" | "manual").
    std::string Profile() const { return GetString("resources.profile"); }
    std::string Mode() const { return GetString("smart.mode"); }
    // The caps in force: the profile's own in Smart and Strict, the user's numbers in Manual.
    ResourceCaps EffectiveCaps() const;

    static ResourceCaps CapsFor(std::string_view profile);
    static ProfileAllocation AllocationFor(std::string_view profile);

    // The scripture options (Scripture tab > options) as the slide builder takes them.
    presentation::ScriptureSettings Scripture() const;

    const std::string& StorageFile() const noexcept { return storageFile_; }

private:
    Result<void> SaveLocked() const;
    Result<void> Validate(const SettingDef& def, const json::Value& value) const;

    std::string storageFile_;
    mutable std::mutex mutex_;
    std::map<std::string, json::Value, std::less<>> values_;   // only what differs from the default
    std::map<size_t, Listener> listeners_;
    size_t nextListener_ = 1;
};

} // namespace bps::settings
