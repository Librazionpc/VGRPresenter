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
    // NO "... on individual lines" ROW and NO "Smart split" ROW (user call:
    // "remove the paragraph on individual lines feature and smart split complete,
    // leave the rest"). Both were layout toggles, not preferences the operator
    // could predict: the first decided whether a slide's items run together or
    // stack, the second whether the template's own text box held fewer of them.
    // Their SHIPPED defaults are what the fill now does unconditionally — see
    // FillOptionsFor — so every design and preview lays out exactly as it did.
    d.push_back(Bool(p + ".splitLongVerses", p, "Divide long " + many, false));
    // NO "Number the parts (1a, 1b)" ROW (user call: "you can remove this"). It
    // only ever appeared under "Divide long …", and it was the third way of
    // saying the same thing: what a divided item's later parts are called. Its
    // SHIPPED default is what the fill now does unconditionally (see
    // FillOptionsFor) — the first part keeps its number and a continuation is
    // shown as more of the same item, which is what every design's previews were
    // built against.
    // WORDS, not characters (user call: "change size to max words per slide"):
    // the number an operator can actually judge. Its old name said "Size" and its
    // value counted characters in ONE item, so the thing it measured was the
    // length of a verse rather than how much a slide shows.
    d.push_back(IntRange(p + ".wordsPerSlide", p, "Max words per slide", 40, 3, 1000,
                         "A slide shows at most this many words - a longer " + one + " is divided into as many even parts as this number needs"));
    // NO "Tolerance" ROW (user call: "what is this, this is wrong — simplify, max
    // words per slide determines what gets pushed to the next 3-1"). It was a
    // percent past the budget a final piece might run instead of being divided
    // again: a second, invisible rule on top of a number the operator could see,
    // and the reason a 40-word budget produced four-word parts. Where a long item
    // is cut now follows from the words-per-slide number alone (see
    // BookFillOptions::wordsPerSlide); the saved `longVersesTolerance` value is
    // left in the settings store untouched.
    // NO "Max " + many + " ROW (user call: "the max paragraph I don't see it
    // feature, just set to one by default internal and remove its toggle"). It was
    // never a real preference: the only layouts it could produce were "one verse to
    // a slide" and "several", and several is the template author's call, not a
    // number an operator can predict. It reads as 1 now (see FillOptionsFor); the
    // saved `versesPerSlide` value is left in the settings store untouched.
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
    // The app's locale: dates, times and numbers follow it immediately (Qt's
    // own CLDR data covers these), and a matching translation is picked up when
    // one is installed, so nothing here depends on a .qm being shipped.
    d.push_back(Choice("appearance.language", "appearance", "Language", "en-US", {
        C("en-US", "English (US)"), C("en-GB", "English (UK)"),
        C("de-DE", "Deutsch"), C("es-ES", "Espa\xc3\xb1" "ol"), C("fr-FR", "Fran\xc3\xa7" "ais"),
    }, "Dates, times and numbers follow it at once; translated interface strings appear as the build ships them."));
    // Says the half that is invisible from this row: Lock In Mode outranks the
    // Smart Config mode, so the engine runs in Presentation mode while it is on
    // (Smart Config states the same precedence on its own page — see
    // SmartConfigScreen's locked-in note).
    d.push_back(Bool("appearance.lockInMode", "appearance", "Lock In Mode", false,
                     "Keeps the engine focused on the live show: background work is paused, only errors interrupt you, "
                     "and it takes precedence over the Smart Config mode (the engine runs in Presentation mode while it is on)."));
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
    // OFF by default: the app starts on the Show screen ("home") with nothing
    // opened. The restore used to be the default, and its most visible effect
    // was entering EDIT mode with a show on the canvas — the report: "if I was in
    // the edit canvas, or in Settings, or in a template edit before I quit, when
    // I open it back it automatically opens [a project] in edit mode". Turning it
    // on is a deliberate choice to resume instead of start.
    d.push_back(Bool("preferences.restoreLastSession", "preferences", "Resume last session", false,
                     "Comes back to where you left: the screen, the show or design, and the Settings page you had open — instead of the home screen."));
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
    // What else a backup may carry beyond the open show. Each is opt-in, so a
    // backup is only as large as the user asked for; the app copies whichever
    // are on into the same dated backups folder (BackupStore).
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
    }, "How much the engine writes to its log (engine.log): Debug is verbose, Error keeps it quiet."));

    // ---- Resource profile ----
    d.push_back(Choice("resources.profile", "resources", "Resource profile", "performance", {
        C("performance", "Performance"), C("balanced", "Balanced"), C("powerSaver", "Power Saver"),
    }, "Runtime priority for rendering, encoding and outputs — it sets the GPU and CPU budgets Smart Config shows; "
       "in Smart Config's Manual mode the two budget sliders take precedence."));

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
    // THE TABS AND THEIR WORDS, in ONE table. These were one call per tab with
    // Shows added separately, so a tab that existed in the UI without a line here
    // got NO option definitions at all — its settings card read empty and every
    // write refused with "there is no setting 'theirs.verseNumbers'" — and the
    // order of the definitions (which is the order the cards show) depended on the
    // order of the calls. Every tab is the same shape: a key, the word for one
    // item of its content in both sizes and both cases.
    // The capitalised PLURAL ("Verses") is deliberately not a field: its only
    // row was the removed "Max verses/paragraphs" cap, and the rows that remain
    // say "Verse numbers" and "Divide long verses" — the lower-case words below.
    struct FillTab {
        const char *key;        // the settings prefix (also the template type)
        const char *one;        // "Verse"
        const char *lowerOne;   // "verse"
        const char *lowerMany;  // "verses"
    };
    static constexpr FillTab kFillTabs[] = {
        { "scripture", "Verse",     "verse",     "verses" },
        { "table",     "Paragraph", "paragraph", "paragraphs" },
        // Shows: the same option shape for the shows tab's template sidebar (its
        // "line" wording) — the UI rows read these definitions for labels/ranges;
        // the show-side slide builder consumes them as it lands.
        { "shows",     "Line",      "line",      "lines" },
    };
    for (const FillTab &tab : kFillTabs)
        AddFillOptions(d, tab.key, tab.one, tab.lowerOne, tab.lowerMany);

    // ---- What the app remembers between runs (not shown as settings) ----
    d.push_back(Text("session.scriptureBible", "session", "Last Bible"));
    // The EXTRA Bible versions shown BESIDE the selected one — the parallel layouts
    // ("Scripture 2", "Scripture 3", "Scripture 4", "Scripture Lower Third 2") read
    // them as {scripture2_text} / {scripture3_text} / {scripture4_text}. Their ids,
    // comma-separated, in slot order. Declared here for the same reason every
    // session key is: an undeclared key refuses every write (and the tab's choice
    // would silently fail to come back).
    d.push_back(Text("session.scriptureParallelBibles", "session", "Parallel Bible versions"));
    d.push_back(Text("session.lastProject", "session", "Last project"));
    d.push_back(Text("session.lastShowPath", "session", "Last show"));
    d.push_back(Text("session.lastView", "session", "Last screen"));
    // The PAGE inside Settings the session ended on, or "" when Settings was shut
    // (see AppBehaviors' resume and Main.qml's rememberSettingsPage): kept apart
    // from lastView above so "I had Audio & Video open" is remembered without
    // making Settings look like a screen of its own. "Resume last session" is what
    // reads it, and it is off by default, so nothing opens a dialog on its own.
    d.push_back(Text("session.lastSettingsSection", "session", "Last settings page"));
    // The design ("template:<id>" / "overlay:<id>") the last session was
    // editing, "" when it ended on a show. Restoring the SCREEN alone brought a
    // template or overlay edit back as the last SHOW on the canvas in edit mode;
    // this is what lets the edit that was interrupted come back AS itself.
    d.push_back(Text("session.lastDesign", "session", "Last design edited"));
    // Quick search's pick memory: the results the user chose before, as a JSON
    // array — a picked result floats to the top of its group on the next search.
    d.push_back(Text("session.quickSearchPicks", "session", "Quick search picks"));
    // The audio board's roster (Settings · Audio & Video): every input row
    // with its device/name/level/mute/mode/delay/channel count/gains, as a
    // JSON array written by AudioInputListModel (debounced) so the board
    // comes back between runs instead of starting empty every launch.
    d.push_back(Text("session.audioRoster", "session", "Audio roster"));
    // The video board's roster (the camera/ndi sources the user added on
    // Settings · Audio & Video), written by VideoSourceListModel under the
    // SAME contract as the audio key — declared here or every write/read
    // refuses with "there is no setting 'session.videoRoster'".
    d.push_back(Text("session.videoRoster", "session", "Video roster"));
    // The Recording & Streaming screen's settings (platform, server URL,
    // stream key, bitrates, container/encoder, screens-to-record), as one
    // JSON object written by RecordingService — declared here or every
    // write/read refuses with "there is no setting 'session.recordingConfig'".
    d.push_back(Text("session.recordingConfig", "session", "Recording config"));
    // The Plugins screen's feature switches ({"ndi": true, ...}), written by
    // EngineBridge::setPluginFeatureEnabled and replayed at boot — declared
    // here or every write/read refuses with "there is no setting
    // 'plugins.featureStates'" (which silently disabled persistence: the
    // user's ON rebooted as OFF, and the self-test caught the send refusal).
    d.push_back(Text("plugins.featureStates", "plugins", "Plugin feature states"));
    // The once-per-install firewall-authorization flag (Outputs screen's NDI
    // ask) — same "must be declared" contract as the rosters above.
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
    // The main screen's left sidebar (the Projects pane) - dragged by its SplitHandle and remembered so a restart
    // doesn't put it back to its responsive default. -1 is the "auto" sentinel the pane itself writes when the
    // handle is double-clicked: it is the default, so a reset erases the entry and the column follows the window
    // width again. Declared here or every write/read refuses with "there is no setting
    // 'session.projectsColumnWidth'" every time the pane's save timer fires.
    d.push_back(IntRange("session.projectsColumnWidth", "session", "Projects column width", -1, -1, 560));
    // The main screen's other two side columns, on the same contract: the output
    // monitors column on the right and the template inspector inside the shows
    // pane. Each keeps its OWN value, so dragging one cannot move the other, and
    // both come back at the width the user left them at ("the stuff keeps going
    // back and not following my adjustment"). Same -1 = auto sentinel.
    d.push_back(IntRange("session.outputsColumnWidth", "session", "Output monitors column width", -1, -1, 560));
    d.push_back(IntRange("session.templateColumnWidth", "session", "Template column width", -1, -1, 560));
    // Where the library dock starts (the main screen's bottom splitter), also
    // remembered: it is the last splitter whose edge the user sets up only to lose
    // it on the next boot. -1 = the default 54% of the workspace.
    d.push_back(IntRange("session.dockHeightTop", "session", "Library dock height", -1, -1, 2000));
    // The Shows template inspector's picks. "All"'s own pick is the fallback every category without one
    // inherits; the per-category overrides ride ONE declared JSON key ({"<category>": "<templateId>"}), the
    // same shape session.audioRoster uses. A category's name only exists once the user creates it and
    // settings keys must be declared up front, so the old per-name "shows.categoryTemplate.<name>" refused
    // every pick with "there is no setting …" and nothing was ever remembered.
    d.push_back(Text("shows.defaultTemplate", "shows", "Default template"));
    d.push_back(Text("session.showsCategoryTemplates", "session", "Shows category templates"));
    // Quick Lyrics' fresh-typed slides pick their template through the "song" profile
    // (SlideBuilder::templateId): declared here so a pick can actually be stored, instead of the read
    // falling back to tpl-default forever.
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

AppSettings::AppSettings(std::string storageFile) : storageFile_(std::move(storageFile)) {}

Result<void> AppSettings::Load() {
    // The PAL read, the parse AND the set-aside of a damaged file all run with
    // NO lock held (filesystem I/O never belongs under a module lock — see the
    // lock-scope rule); the locked section below only consumes what was read.
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
            // A file that PARSES is not automatically a settings file: the
            // shape check condemns a document with no usable 'settings' object
            // for the same reason a parse error does — nothing here is
            // loadable, and letting the next save overwrite it throws away the
            // one copy of what the operator had.
            const std::string problem =
                parsed.ok() ? recovery::DocumentShapeProblem(
                                  parsed.value(), "settings", J::Type::Object)
                            : parsed.error().message;
            if (!problem.empty()) {
                // SELF-HEALING: the damaged file is set aside as evidence and the
                // defaults apply. Returning an error here left the app unable to
                // load settings at all AND let the next save overwrite the one
                // piece of evidence there was; now the read succeeds, the
                // defaults take effect, and the next save rebuilds a healthy
                // file at the same path.
                (void)recovery::SetAsideDamagedDocument(storageFile_, kModule, problem);
            } else {
                storedDocument = *parsed.value().Find("settings");
                haveStored = true;
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    if (!haveStored) return {};

    // Only values that still pass the current rules are kept: a hand-edited or older file cannot smuggle in a bad one.
    for (const auto& [key, value] : *storedDocument.asObject()) {
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

    // ATOMIC (PAL WriteAtomic): settings.json is read back on the next launch, so
    // an interrupted save must leave the PREVIOUS complete file in place rather
    // than a truncated or zero-filled one — the failure mode that made a
    // half-written kernel.json unrecoverable. The call also creates the parent
    // dir, which this block used to do by hand.
    return platform::PlatformAccessor::Get().Filesystem().WriteAtomic(storageFile_, text);
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

presentation::BookFillOptions AppSettings::FillOptionsFor(const std::string& p) const {
    presentation::BookFillOptions s;
    // The struct's members are tab-neutral ("numbers", "wordsPerSlide", …); the
    // config KEYS keep the names these settings were first saved under, so an
    // existing settings file still reads — renaming a key would silently reset
    // every user's verse/paragraph options to their defaults.
    s.numbers = GetBool(p + ".verseNumbers");
    s.splitLongItems = GetBool(p + ".splitLongVerses");
    // The removed "Number the parts" row's shipped default (see AddFillOptions).
    // The saved `splitLongVersesSuffix` value is deliberately NOT re-read: the
    // fill's own rule for a continuation is the one every shipped design and
    // preview was built against, and the option stays in BookFillOptions for the
    // callers that pass options straight in (the unit suite pins both forms).
    s.splitSuffix = false;
    // The ONE number that decides the division (the removed toggles' behaviour is
    // what the fill now does unconditionally: items run together on a slide, and
    // no box-derived second budget is applied — see BookFillOptions).
    s.wordsPerSlide = static_cast<int>(GetInt(p + ".wordsPerSlide"));
    // HOW MANY ITEMS A SLIDE TAKES is each family's own decision now, not a
    // control (see AddFillOptions):
    //   * The Table takes ONE paragraph to a slide — the user's call ("the max
    //     paragraph I don't see it feature, just set to one by default internal
    //     and remove its toggle"): a sermon's paragraph is a whole thought, and
    //     "Max words per slide" is what divides one too long for its box.
    //   * Scripture keeps the packing its shipped layouts and previews assume (a
    //     verse selection shares a slide until the box or the cap says otherwise),
    //     at the number its default always had.
    // Two saved keys are deliberately NOT re-read: `versesPerSlide` (that cap) and
    // `longVersesChars` (the character limit the word budget replaced). Re-reading
    // either would be worse than ignoring it — an old 233 would come back as 233
    // WORDS PER SLIDE, a budget no slide could ever reach.
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
