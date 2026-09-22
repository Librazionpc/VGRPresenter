#include "modules/settings/AppSettings.hpp"

#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace bps::settings {

namespace {

constexpr const char* kModule = "AppSettings";
using J = json::Value;

SettingDef Bool(std::string key, std::string group, std::string label, bool dflt, std::string description = {}) {
    SettingDef d;
    d.key = std::move(key); d.group = std::move(group); d.label = std::move(label); d.description = std::move(description);
    d.kind = SettingKind::Bool; d.dflt = J::Bool(dflt);
    return d;
}

SettingDef Choice(std::string key, std::string group, std::string label, std::string dflt, std::vector<SettingChoice> choices,
                  std::string description = {}) {
    SettingDef d;
    d.key = std::move(key); d.group = std::move(group); d.label = std::move(label); d.description = std::move(description);
    d.kind = SettingKind::Choice; d.dflt = J::String(std::move(dflt)); d.choices = std::move(choices);
    return d;
}

// An Int that may only be one of a few listed values (each shown with its own label).
SettingDef IntOf(std::string key, std::string group, std::string label, long long dflt, std::vector<SettingChoice> choices,
                 std::string description = {}) {
    SettingDef d;
    d.key = std::move(key); d.group = std::move(group); d.label = std::move(label); d.description = std::move(description);
    d.kind = SettingKind::Int; d.dflt = J::Number(static_cast<double>(dflt)); d.choices = std::move(choices);
    return d;
}

SettingDef IntRange(std::string key, std::string group, std::string label, long long dflt, long long lo, long long hi,
                    std::string description = {}) {
    SettingDef d;
    d.key = std::move(key); d.group = std::move(group); d.label = std::move(label); d.description = std::move(description);
    d.kind = SettingKind::Int; d.dflt = J::Number(static_cast<double>(dflt)); d.min = lo; d.max = hi;
    return d;
}

SettingDef Text(std::string key, std::string group, std::string label) {
    SettingDef d;
    d.key = std::move(key); d.group = std::move(group); d.label = std::move(label);
    d.kind = SettingKind::Text; d.dflt = J::String("");
    return d;
}

SettingChoice C(std::string value, std::string label, std::string description = {}) {
    SettingChoice c;
    c.value = std::move(value); c.label = std::move(label); c.description = std::move(description);
    return c;
}

SettingChoice Accent(std::string value, std::string label, std::string color, std::string light) {
    SettingChoice c = C(std::move(value), std::move(label));
    c.color = std::move(color); c.colorLight = std::move(light);
    return c;
}

// The slide builder's options, under one tab's prefix. Scripture and The Table have the same set; only the word for a piece of text differs
// (verse / paragraph).
void AddSlideBuilderOptions(std::vector<SettingDef>& d, const std::string& p, const std::string& One, const std::string& Many,
                            const std::string& one, const std::string& many) {
    d.push_back(Bool(p + ".verseNumbers", p, One + " numbers", true));
    d.push_back(Bool(p + ".versesOnIndividualLines", p, Many + " on individual lines", false));
    d.push_back(Bool(p + ".splitLongVerses", p, "Divide long " + many, false));
    d.push_back(Bool(p + ".splitLongVersesSuffix", p, "Number the parts (1a, 1b)", false));
    d.push_back(IntRange(p + ".longVersesChars", p, "Size", 100, 3, 1000, "Characters a " + one + " may have before it is divided"));
    d.push_back(IntRange(p + ".longVersesTolerance", p, "Tolerance", 0, 0, 100, "Percent past the size a cut may wait for a word end"));
    d.push_back(Bool(p + ".smartSplit", p, "Smart split", true, "As many " + many + " to a slide as the template's text box holds"));
    d.push_back(IntRange(p + ".versesPerSlide", p, "Max " + many, 3, 1, 100));
    d.push_back(Text(p + ".template", p, "Template"));
}

std::vector<SettingDef> BuildDefinitions() {
    std::vector<SettingDef> d;

    // ---- Appearance ----
    d.push_back(Choice("appearance.theme", "appearance", "Theme", "dark", { C("dark", "Dark") }));
    d.push_back(Choice("appearance.language", "appearance", "Language", "en-US", { C("en-US", "English (US)") }));
    d.push_back(Bool("appearance.lockInMode", "appearance", "Lock In Mode", false,
                     "Keeps the engine focused on the live show: background work is paused and only errors interrupt you."));
    d.push_back(Choice("appearance.accent", "appearance", "Accent color", "purple", {
        Accent("red", "Red", "#ff4d3d", "#ff6b61"),
        Accent("blue", "Blue", "#4da6ff", "#8ecbff"),
        Accent("green", "Green", "#4ade80", "#7ee2a8"),
        Accent("purple", "Purple", "#6c5ce7", "#9b8ff5"),
    }));

    // ---- Startup ----
    d.push_back(Bool("startup.openLastProject", "startup", "Open last project", false));
    d.push_back(Bool("startup.launchAtLogin", "startup", "Launch at login", false));
    d.push_back(Bool("startup.autosave", "startup", "Autosave", false,
                     "Saves the open show by itself once it has a file."));
    d.push_back(IntOf("startup.autosaveSeconds", "startup", "Autosave every", 60, {
        C("30", "30 seconds"), C("60", "1 minute"), C("120", "2 minutes"), C("300", "5 minutes"),
    }));

    // ---- Preferences ----
    d.push_back(Bool("preferences.startMinimized", "preferences", "Start minimized", false));
    d.push_back(Bool("preferences.restoreLastSession", "preferences", "Restore last session", true,
                     "Comes back to the screen and show you left."));
    d.push_back(Bool("preferences.closeToTray", "preferences", "Close to tray", false,
                     "Closing the window keeps the app running in the system tray; its icon brings the window back or quits."));

    // ---- Backups & recovery ----
    d.push_back(Bool("backups.automatic", "backups", "Automatic backups", true));
    d.push_back(IntOf("backups.intervalMinutes", "backups", "Back up", 30, {
        C("15", "Every 15 minutes"), C("30", "Every 30 minutes"), C("60", "Every hour"), C("120", "Every 2 hours"),
    }));
    d.push_back(IntOf("backups.keepLast", "backups", "Keep last", 10, {
        C("3", "3 backups"), C("5", "5 backups"), C("10", "10 backups"), C("20", "20 backups"), C("50", "50 backups"),
    }));
    d.push_back(Bool("backups.crashRecovery", "backups", "Crash recovery", true,
                     "Restores unsaved work after an unexpected exit"));

    // ---- Notifications & logs ----
    d.push_back(Bool("notifications.show", "notifications", "Show notifications", true,
                     "Toasts for saves, backups and warnings. Errors always show."));
    d.push_back(Choice("notifications.logLevel", "notifications", "Log level", "info", {
        C("error", "Error"), C("warning", "Warning"), C("info", "Info"), C("debug", "Debug"),
    }));

    // ---- Resource profile ----
    d.push_back(Choice("resources.profile", "resources", "Resource profile", "performance", {
        C("performance", "Performance"), C("balanced", "Balanced"), C("powerSaver", "Power Saver"),
    }, "Runtime priority for rendering, encoding and outputs"));

    // ---- Smart Config ----
    d.push_back(Choice("smart.mode", "smart", "Configuration mode", "smart", {
        C("strict", "Strict", "Only initialize what you enable"),
        C("smart", "Smart", "Auto-tune for this hardware"),
        C("manual", "Manual", "You configure every option"),
    }));
    d.push_back(IntRange("smart.gpuBudgetPct", "smart", "GPU budget", 80, 10, 100,
                         "The most of the GPU the engine may use (used in Manual mode)."));
    d.push_back(IntRange("smart.cpuBudgetPct", "smart", "CPU budget", 60, 10, 100,
                         "The most of the CPU the engine may use (used in Manual mode)."));

    // ---- Scripture and The Table: the slide builder's options (FreeShow's scripture settings), each tab with its own keys and its own template ----
    AddSlideBuilderOptions(d, "scripture", "Verse", "Verses", "verse", "verses");
    AddSlideBuilderOptions(d, "table", "Paragraph", "Paragraphs", "paragraph", "paragraphs");

    // ---- What the app remembers between runs (not shown as settings) ----
    d.push_back(Text("session.scriptureBible", "session", "Last Bible"));
    d.push_back(Text("session.lastProject", "session", "Last project"));
    d.push_back(Text("session.lastShowPath", "session", "Last show"));
    d.push_back(Text("session.lastView", "session", "Last screen"));
    // The slide grid on the Show screen's centre page (FreeShow's slidesOptions): slides across, and how they are laid out.
    d.push_back(IntRange("session.slideColumns", "session", "Slides across", 4, 2, 10));
    d.push_back(Choice("session.slideView", "session", "Slide view", "grid", {
        C("grid", "Grid"), C("list", "List"), C("lyrics", "Lyrics"),
    }));
    // The reference pane's (Scripture / The Table) column widths - dragged by their SplitHandle, remembered so a rebuild or a
    // restart doesn't put them back to their defaults. Shared between the two tabs, the same as the rest of "session".
    d.push_back(IntRange("session.referencePaneBooksWidth", "session", "Reference books column width", 150, 100, 340));
    d.push_back(IntRange("session.referencePaneChaptersWidth", "session", "Reference chapters column width", 52, 40, 420));
    d.push_back(IntRange("session.referencePanePreviewWidth", "session", "Reference preview column width", 360, 260, 560));
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

const std::vector<SettingDef>& AppSettings::Definitions() {
    static const std::vector<SettingDef> defs = BuildDefinitions();
    return defs;
}

const SettingDef* AppSettings::Find(std::string_view key) {
    for (const SettingDef& d : Definitions())
        if (d.key == key) return &d;
    return nullptr;
}

ResourceCaps AppSettings::CapsFor(std::string_view profile) {
    if (profile == "balanced") return { 65, 50 };
    if (profile == "powerSaver") return { 40, 35 };
    return { 80, 60 };   // performance
}

ProfileAllocation AppSettings::AllocationFor(std::string_view profile) {
    if (profile == "balanced") return { 55, 35, 45 };
    if (profile == "powerSaver") return { 35, 25, 30 };
    return { 68, 42, 55 };   // performance
}

// ---------------------------------------------------------------------------
// Load / save
// ---------------------------------------------------------------------------

AppSettings::AppSettings(std::string storageFile) : storageFile_(std::move(storageFile)) {}

Result<void> AppSettings::Load() {
    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    if (!fs.Exists(storageFile_)) return {};

    auto text = fs.ReadText(storageFile_);
    if (!text.ok()) return text.error();
    auto parsed = json::Parse(text.value());
    if (!parsed.ok())
        return Error::Make(Err::ParseError, kModule,
                           std::format("the settings file is damaged, using the defaults: {}", parsed.error().message));
    const J* stored = parsed.value().Find("settings");
    if (!stored || !stored->asObject()) return {};

    // Only values that still pass the current rules are kept: a hand-edited or older file cannot smuggle in a bad one.
    for (const auto& [key, value] : *stored->asObject()) {
        const SettingDef* def = Find(key);
        if (def && Validate(*def, value).ok() && !(value.ToString() == def->dflt.ToString()))
            values_[key] = value;
    }
    return {};
}

Result<void> AppSettings::SaveLocked() const {
    J::Object stored;
    for (const auto& [key, value] : values_) stored[key] = value;
    J::Object root;
    root["version"] = J::Number(1);
    root["settings"] = J(std::move(stored));
    const std::string text = J(std::move(root)).ToString();

    auto& fs = platform::PlatformAccessor::Get().Filesystem();
    const size_t cut = storageFile_.find_last_of("/\\");
    if (cut != std::string::npos && cut > 0) {
        auto made = fs.CreateDirectories(std::string_view(storageFile_).substr(0, cut));
        if (!made.ok()) return made.error();
    }
    return fs.Write(storageFile_, text);
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

J AppSettings::Get(std::string_view key) const {
    const SettingDef* def = Find(key);
    if (!def) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = values_.find(key);
    return it != values_.end() ? it->second : def->dflt;
}

bool AppSettings::GetBool(std::string_view key) const { return Get(key).asBool(); }
long long AppSettings::GetInt(std::string_view key) const { return Get(key).asInt(); }
std::string AppSettings::GetString(std::string_view key) const { return std::string(Get(key).asString()); }

Result<void> AppSettings::Validate(const SettingDef& def, const J& value) const {
    auto bad = [&](std::string_view why) {
        return Error::Make(Err::InvalidArgument, kModule, std::format("'{}' {}", def.label, why));
    };
    switch (def.kind) {
        case SettingKind::Bool:
            if (value.type() != J::Type::Bool) return bad("must be true or false");
            return {};
        case SettingKind::Text:
            if (value.type() != J::Type::String) return bad("must be text");
            if (value.asString().size() > 4096) return bad("is too long");
            return {};
        case SettingKind::Choice: {
            if (value.type() != J::Type::String) return bad("must be one of the listed choices");
            const auto it = std::find_if(def.choices.begin(), def.choices.end(),
                                         [&](const SettingChoice& c) { return c.value == value.asString(); });
            if (it == def.choices.end()) return bad(std::format("has no choice '{}'", value.asString()));
            return {};
        }
        case SettingKind::Int: {
            if (value.type() != J::Type::Number) return bad("must be a number");
            const double n = value.asNumber();
            if (!std::isfinite(n) || n != std::floor(n)) return bad("must be a whole number");
            if (!def.choices.empty()) {
                const std::string text = std::to_string(static_cast<long long>(n));
                const bool listed = std::any_of(def.choices.begin(), def.choices.end(),
                                                [&](const SettingChoice& c) { return c.value == text; });
                if (!listed) return bad(std::format("cannot be {}", text));
            } else if (n < static_cast<double>(def.min) || n > static_cast<double>(def.max)) {
                return bad(std::format("must be between {} and {}", def.min, def.max));
            }
            return {};
        }
    }
    return {};
}

Result<void> AppSettings::Set(std::string_view key, const J& value) {
    const SettingDef* def = Find(key);
    if (!def) return Error::Make(Err::NotFound, kModule, std::format("there is no setting '{}'", key));
    if (auto ok = Validate(*def, value); !ok.ok()) return ok;

    std::vector<Listener> toTell;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = values_.find(key);
        const J& current = it != values_.end() ? it->second : def->dflt;
        if (current.ToString() == value.ToString()) return {};

        // Stored only while it differs from the default, so the file stays a short list of what the user changed.
        const auto previous = it != values_.end() ? std::optional<J>(it->second) : std::nullopt;
        if (value.ToString() == def->dflt.ToString()) values_.erase(std::string(key));
        else values_[std::string(key)] = value;
        if (auto saved = SaveLocked(); !saved.ok()) {
            if (previous) values_[std::string(key)] = *previous; else values_.erase(std::string(key));
            return saved;
        }
        for (const auto& [id, l] : listeners_) toTell.push_back(l);
    }
    for (const Listener& l : toTell) l(std::string(key));
    return {};
}

Result<void> AppSettings::Reset(std::string_view key) {
    const SettingDef* def = Find(key);
    if (!def) return Error::Make(Err::NotFound, kModule, std::format("there is no setting '{}'", key));
    return Set(key, def->dflt);
}

size_t AppSettings::ResetAll() {
    size_t changed = 0;
    for (const SettingDef& d : Definitions()) {
        if (d.group == "session") continue;
        if (Get(d.key).ToString() != d.dflt.ToString() && Set(d.key, d.dflt).ok()) ++changed;
    }
    return changed;
}

size_t AppSettings::Subscribe(Listener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t id = nextListener_++;
    listeners_[id] = std::move(listener);
    return id;
}

void AppSettings::Unsubscribe(size_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(id);
}

presentation::ScriptureSettings AppSettings::SlideBuilderOptions(const std::string& p) const {
    presentation::ScriptureSettings s;
    s.verseNumbers = GetBool(p + ".verseNumbers");
    s.versesOnIndividualLines = GetBool(p + ".versesOnIndividualLines");
    s.splitLongVerses = GetBool(p + ".splitLongVerses");
    s.splitLongVersesSuffix = GetBool(p + ".splitLongVersesSuffix");
    s.longVersesChars = static_cast<int>(GetInt(p + ".longVersesChars"));
    s.longVersesTolerance = static_cast<int>(GetInt(p + ".longVersesTolerance"));
    s.smartSplit = GetBool(p + ".smartSplit");
    s.versesPerSlide = static_cast<int>(GetInt(p + ".versesPerSlide"));
    return s;
}

presentation::ScriptureSettings AppSettings::Scripture() const { return SlideBuilderOptions("scripture"); }
// The Table's options: the same settings under the "table." keys.
presentation::ScriptureSettings AppSettings::TheTable() const { return SlideBuilderOptions("table"); }

ResourceCaps AppSettings::EffectiveCaps() const {
    if (Mode() == "manual")
        return { static_cast<int>(GetInt("smart.gpuBudgetPct")), static_cast<int>(GetInt("smart.cpuBudgetPct")) };
    return CapsFor(Profile());
}

} // namespace bps::settings
