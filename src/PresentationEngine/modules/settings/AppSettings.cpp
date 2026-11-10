#include "modules/settings/AppSettings.hpp"

#include "core/recovery/DocumentRecovery.hpp"
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
void AddFillOptions(std::vector<SettingDef>& d, const std::string& p, const std::string& One,
                            const std::string& one, const std::string& many) {
    d.push_back(Bool(p + ".verseNumbers", p, One + " numbers", true));
    d.push_back(Bool(p + ".splitLongVerses", p, "Divide long " + many, false));
    d.push_back(IntRange(p + ".wordsPerSlide", p, "Max words per slide", 40, 3, 1000,
                         "A slide shows at most this many words - a longer " + one + " is divided into as many even parts as this number needs"));
    d.push_back(Text(p + ".template", p, "Template"));
}

std::vector<SettingDef> BuildDefinitions() {
    std::vector<SettingDef> d;

    // ---- Appearance ----
    // Light/Dark are real now: the app's Theme singleton switches its whole
    // token set on this value, so picking one recolours the interface live.
    d.push_back(Choice("appearance.theme", "appearance", "Theme", "dark", {
        C("dark", "Dark", "The app's own dark chrome."),
        C("light", "Light", "A light version of the same chrome."),
    }));
    d.push_back(Choice("appearance.language", "appearance", "Language", "en-US", {
        C("en-US", "English (US)"), C("en-GB", "English (UK)"),
        C("de-DE", "Deutsch"), C("es-ES", "Espa\xc3\xb1" "ol"), C("fr-FR", "Fran\xc3\xa7" "ais"),
    }, "Formats dates and numbers now; UI text follows once translated."));
    d.push_back(Bool("appearance.lockInMode", "appearance", "Lock In Mode", false,
                     "Pauses background work during the live show — only errors interrupt — and overrides Smart Config."));
    d.push_back(Choice("appearance.accent", "appearance", "Accent color", "purple", {
        Accent("red", "Red", "#ff4d3d", "#ff6b61"),
        Accent("blue", "Blue", "#4da6ff", "#8ecbff"),
        Accent("green", "Green", "#4ade80", "#7ee2a8"),
        Accent("purple", "Purple", "#6c5ce7", "#9b8ff5"),
    }));
    d.push_back(IntRange("appearance.textScale", "appearance", "Text size", 120, 80, 160,
                         "Scales the whole interface, text included. 100 is the design size."));

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
    d.push_back(Bool("preferences.restoreLastSession", "preferences", "Resume last session", false,
                     "Opens back to the screen and show you left, instead of the home screen."));
    d.push_back(Bool("preferences.closeToTray", "preferences", "Close to tray", false,
                     "Keeps running in the system tray instead of quitting."));
    d.push_back(Bool("preferences.hardwareAcceleration", "preferences", "Hardware rendering (GPU)", false,
                     "Off by default — the GPU path is still being finished."));

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
    d.push_back(Bool("backups.includeSettings", "backups", "Back up settings", false,
                     "Your preferences, appearance and setup choices"));
    d.push_back(Bool("backups.includeOverlays", "backups", "Back up overlays", false,
                     "The overlay library you have built"));
    d.push_back(Bool("backups.includeTemplates", "backups", "Back up templates", false,
                     "The template library you have built"));

    // ---- Notifications & logs ----
    d.push_back(Bool("notifications.show", "notifications", "Show notifications", true,
                     "Toasts for saves, backups and warnings. Errors always show."));
    d.push_back(Choice("notifications.logLevel", "notifications", "Log level", "info", {
        C("error", "Error"), C("warning", "Warning"), C("info", "Info"), C("debug", "Debug"),
    }, "How much the engine writes to engine.log."));

    // ---- Outputs ----
    d.push_back(Choice("outputs.defaultMediaFit", "outputs", "Default media fit", "contain", {
        C("contain", "Contain", "Whole picture, letterboxed"),
        C("cover", "Cover", "Fill the frame, cropping the edges"),
        C("fill", "Fill", "Stretch to the frame"),
    }, "The shape for a picture whose own file does not name one."));

    // ---- The reading step (Alt+Down / Alt+Up) ----
    // CURSOR FREQUENCY: how many lines one press moves (Alt+Down / Alt+Up, the wall's ‹ ›, the next-timer). 1 is the old
    // one-line-at-a-time walk; a higher number reads faster per press (a step never carries across a page turn).
    d.push_back(IntRange("reading.stepLines", "reading", "Cursor frequency", 1, 1, 20,
                         "How many lines one press of the arrow moves. 1 walks a line at a time; raise it to read faster with fewer presses."));
    d.push_back(IntRange("reading.stepMs", "reading", "Reading step speed", 190, 60, 600,
                         "How long one line takes to travel, in milliseconds. Slower is calmer to read along with. Shown beside the tab's own slide options."));
    // MOTION is the ease. "Smooth" is the default: it leaves the key press at a
    // readable speed and settles into the line, which is what a prompter does.
    d.push_back(Choice("reading.stepEasing", "reading", "Reading step motion", "smooth", {
        C("smooth", "Smooth", "Eases out of the key press and lands - the default."),
        C("settle", "Settle", "Moves at once, then slows into the new line."),
        C("even", "Even", "One constant speed, no acceleration."),
    }, "How the words accelerate and settle during a line step."));

    // ---- Resource profile ----
    d.push_back(Choice("resources.profile", "resources", "Resource profile", "performance", {
        C("performance", "Performance"), C("balanced", "Balanced"), C("powerSaver", "Power Saver"),
    }, "Sets the GPU/CPU budgets Smart Config uses."));

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
    struct FillTab {
        const char *key;        // the settings prefix (also the template type)
        const char *one;        // "Verse"
        const char *lowerOne;   // "verse"
        const char *lowerMany;  // "verses"
    };
    static constexpr FillTab kFillTabs[] = {
        { "scripture", "Verse",     "verse",     "verses" },
        { "table",     "Paragraph", "paragraph", "paragraphs" },
        { "shows",     "Line",      "line",      "lines" },
    };
    for (const FillTab &tab : kFillTabs)
        AddFillOptions(d, tab.key, tab.one, tab.lowerOne, tab.lowerMany);

    d.push_back(Text("scripture.scrollWalk", "scripture", "Scroll the passage"));
    d.push_back(Text("table.scrollWalk", "table", "Scroll the passage"));
    d.push_back(Text("shows.scrollWalk", "shows", "Scroll the passage"));
    // WHICH show advances by itself, using that show's own per-slide times (the wall's NEXT TIMER popover
    // writes them into the show). Recorded per show on purpose: turning the timer on for one show must not
    // make every other show slide by itself.
    d.push_back(Text("shows.timedShow", "shows", "Show that advances by itself"));

    // ---- What the app remembers between runs (not shown as settings) ----
    d.push_back(Text("session.scriptureBible", "session", "Last Bible"));
    d.push_back(Text("session.scriptureParallelBibles", "session", "Parallel Bible versions"));
    d.push_back(Text("session.lastProject", "session", "Last project"));
    // What this machine demonstrated it can carry as an NDI feed ("<n> Mpx/s · <detail>"), written by the
    // feed as it sustains a rate and read back by the GO LIVE load check. App-managed, not a preference.
    d.push_back(Text("outputs.ndi.sustainedLoad", "outputs", "NDI load this machine sustained"));
    // Per OUTPUT, because the machine's total cannot say which output is the one asking for too much:
    // "<name>|<W>x<H>@<fps>;<name>|..." — what each output actually held, written as it sustains a rate.
    d.push_back(Text("outputs.ndi.sustainedPerOutput", "outputs", "NDI load each output sustained"));
    d.push_back(Text("session.lastShowPath", "session", "Last show"));
    d.push_back(Text("session.lastView", "session", "Last screen"));
    d.push_back(Text("session.lastSettingsSection", "session", "Last settings page"));
    d.push_back(Text("session.lastDesign", "session", "Last design edited"));
    // Quick search's pick memory: the results the user chose before, as a JSON
    // array — a picked result floats to the top of its group on the next search.
    d.push_back(Text("session.quickSearchPicks", "session", "Quick search picks"));
    d.push_back(Text("session.audioRoster", "session", "Audio roster"));
    d.push_back(Text("session.videoRoster", "session", "Video roster"));
    d.push_back(Text("session.recordingConfig", "session", "Recording config"));
    d.push_back(Text("session.scriptureReading", "session", "Scripture position"));
    d.push_back(Text("session.tableReading", "session", "The Table position"));
    d.push_back(Text("session.showsReading", "session", "Shows position"));
    d.push_back(Text("session.overlaysReading", "session", "Overlays position"));
    d.push_back(Text("session.templatesReading", "session", "Templates position"));
    d.push_back(Text("plugins.featureStates", "plugins", "Plugin feature states"));
    d.push_back(IntRange("session.slideColumns", "session", "Slides across", 4, 2, 10));
    d.push_back(Choice("session.slideView", "session", "Slide view", "grid", {
        C("grid", "Grid"), C("list", "List"), C("lyrics", "Lyrics"),
    }));
    d.push_back(IntRange("session.referencePaneBooksWidth", "session", "Reference books column width", 150, 50, 340));
    d.push_back(IntRange("session.referencePaneChaptersWidth", "session", "Reference chapters column width", 52, 40, 420));
    d.push_back(IntRange("session.referencePanePreviewWidth", "session", "Reference preview column width", 360, 260, 560));
    d.push_back(IntRange("session.projectsColumnWidth", "session", "Projects column width", -1, -1, 560));
    d.push_back(IntRange("session.outputsColumnWidth", "session", "Output monitors column width", -1, -1, 560));
    d.push_back(IntRange("session.templateColumnWidth", "session", "Template column width", -1, -1, 560));
    d.push_back(IntRange("session.dockHeightTop", "session", "Library dock height", -1, -1, 2000));
    d.push_back(Text("shows.defaultTemplate", "shows", "Default template"));
    d.push_back(Text("session.showsCategoryTemplates", "session", "Shows category templates"));
    d.push_back(Text("song.template", "song", "Song template"));
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

AppSettings::AppSettings(std::string storageFile, std::string baselineFile)
    : storageFile_(std::move(storageFile)), baselineFile_(std::move(baselineFile)) {}

Result<void> AppSettings::Load() {
    J storedDocument;
    bool haveStored = false;
    {
        auto& fs = platform::PlatformAccessor::Get().Filesystem();
        if (fs.Exists(storageFile_)) {
            auto text = fs.ReadText(storageFile_);
            if (!text.ok()) {
                std::lock_guard<std::mutex> lock(mutex_);
                values_.clear();
                return text.error();
            }
            auto parsed = json::Parse(text.value());
            const std::string problem =
                parsed.ok() ? recovery::DocumentShapeProblem(
                                  parsed.value(), "settings", J::Type::Object)
                            : parsed.error().message;
            if (!problem.empty()) {
                (void)recovery::SetAsideDamagedDocument(storageFile_, kModule, problem);
            } else {
                storedDocument = *parsed.value().Find("settings");
                haveStored = true;
            }
        }
    }

    // The SHIPPED baseline (what the installer packages) is read the same way —
    // before the lock, because a PAL read never runs under it.
    J baselineDocument;
    bool haveBaseline = false;
    if (!baselineFile_.empty()) {
        auto& fs = platform::PlatformAccessor::Get().Filesystem();
        if (fs.Exists(baselineFile_)) {
            if (auto text = fs.ReadText(baselineFile_); text.ok()) {
                if (auto parsed = json::Parse(text.value()); parsed.ok()) {
                    if (const J* shipped = parsed.value().Find("settings"); shipped && shipped->asObject()) {
                        baselineDocument = *shipped;
                        haveBaseline = true;
                    }
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    if (haveStored) {
        // Only values that still pass the current rules are kept: a hand-edited or older file cannot smuggle in a bad one.
        for (const auto& [key, value] : *storedDocument.asObject()) {
            const SettingDef* def = Find(key);
            if (def && Validate(*def, value).ok() && !(value.ToString() == def->dflt.ToString()))
                values_[key] = value;
        }
    }

    if (haveBaseline) {
        for (const auto& [key, value] : *baselineDocument.asObject()) {
            if (values_.contains(key)) continue;
            const SettingDef* def = Find(key);
            if (!def || !Validate(*def, value).ok()) continue;
            if (value.ToString() == def->dflt.ToString()) continue;   // same as the code default: nothing to adopt
            values_[key] = value;
        }
    }
    return {};
}

std::string AppSettings::BuildJsonLocked() const {
    J::Object stored;
    for (const auto& [key, value] : values_) stored[key] = value;
    J::Object root;
    root["version"] = J::Number(1);
    root["settings"] = J(std::move(stored));
    return J(std::move(root)).ToString();
}

Result<void> AppSettings::CommitJson(const std::string& json) const {
    if (storageFile_.empty() || json.empty()) return {};
    return platform::PlatformAccessor::Get().Filesystem().WriteAtomic(storageFile_, json);
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
    std::optional<J> previous;   // what a failed write puts back
    std::string json;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = values_.find(key);
        const J& current = it != values_.end() ? it->second : def->dflt;
        if (current.ToString() == value.ToString()) return {};

        // Stored only while it differs from the default, so the file stays a short list of what the user changed.
        previous = it != values_.end() ? std::optional<J>(it->second) : std::nullopt;
        if (value.ToString() == def->dflt.ToString()) values_.erase(std::string(key));
        else values_[std::string(key)] = value;
        json = BuildJsonLocked();
        for (const auto& [id, l] : listeners_) toTell.push_back(l);
    }
    // The PAL write runs with the lock RELEASED; on failure the previous value
    // goes back under it. Listeners are told after both, exactly as before.
    if (auto saved = CommitJson(json); !saved.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (previous) values_[std::string(key)] = *previous; else values_.erase(std::string(key));
        return saved;
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

presentation::BookFillOptions AppSettings::FillOptionsFor(const std::string& p) const {
    presentation::BookFillOptions s;
    s.numbers = GetBool(p + ".verseNumbers");
    s.splitLongItems = GetBool(p + ".splitLongVerses");
    s.splitSuffix = false;
    s.wordsPerSlide = static_cast<int>(GetInt(p + ".wordsPerSlide"));
    s.maxPerSlide = p == "table" ? 1 : 3;
    return s;
}

presentation::BookFillOptions AppSettings::Scripture() const { return FillOptionsFor("scripture"); }
// The Table's options: the same settings under the "table." keys.
presentation::BookFillOptions AppSettings::TheTable() const { return FillOptionsFor("table"); }

ResourceCaps AppSettings::EffectiveCaps() const {
    if (Mode() == "manual")
        return { static_cast<int>(GetInt("smart.gpuBudgetPct")), static_cast<int>(GetInt("smart.cpuBudgetPct")) };
    return CapsFor(Profile());
}
} // namespace bps::settings