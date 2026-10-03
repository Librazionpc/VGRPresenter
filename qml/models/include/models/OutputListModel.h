#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QVariantMap>
#include <QVariantList>
#include <qqml.h>

namespace bps::presentation {
struct OutputStyleSpec;
}

class QScreen;
class SettingsService;

// One content toggle in an output's edit dialog — which canvas item kinds
// that output is allowed to show. Kinds mirror the Edit canvas's item types
// (text/camera/media/clock/timer/shape) — deliberately NOT FreeShow's
// shows/songs/etc., which are content-library concepts, not output concepts.
struct OutputContentToggle
{
    QString key;
    QString label;
    bool enabled = true;
};

struct OutputItem
{
    QString name;
    QString badge;
    // "HDMI" | "NDI" | "SDI" | "REC" | "STREAM" (legacy rows may carry
    // "Screen"/"Stream") — the Add dialog's Output type chips.
    QString kind;
    QString res;
    // Refresh rate as chosen in the Add/Edit form ("60 Hz"…). Display-only
    // today; a real output backend will need it.
    QString refresh;
    // "smpte" | "gradient" | "checker" | "solidred" | "none"
    QString testPattern = QStringLiteral("none");
    // Physical display this output renders on (QScreen::name, "" = unassigned
    // — windowed/NDI/REC outputs don't sit on a display). The form's placement
    // map assigns it; resolution derives from the display's current mode.
    QString screenName;
    // FreeShow's boundsLocked: a locked output keeps its display assignment;
    // the form's map greys out and the picker refuses reassignment.
    bool boundsLocked = false;
    // Whether this output's REAL destination window (OutputWindow.qml, a
    // physical/HDMI screen — not the small in-app preview tile) stays above
    // other windows. Off by default (see OutputStore.hpp's field).
    bool stayOnTop = false;
    // Whether this output's REAL destination window covers its whole bound
    // screen (on by default — a real projector/HDMI output's actual job)
    // or starts at half-size, centered and movable (a safe test mode for
    // a single dev monitor). See OutputStore.hpp's field.
    bool fullscreenOutput = true;
    bool active = false;
    // User-level on/off — a disabled screen is dimmed everywhere and can't
    // go live. Named for the role; QML's Item already owns "enabled".
    bool isEnabled = true;
    // Style applied from StyleListModel — referenced by the style's STABLE id
    // ("" = none). An id reference survives restarts and roster edits: removing
    // a style can't re-point every later output at the wrong theme the way an
    // index reference would (FreeShow keys outputs→styles by id the same way).
    QString styleId;
    // NDI (and other network) outputs: carry ONLY their own transport — no
    // physical screen window, and no audience-facing window on this machine
    // (the receiver's monitor IS the audience screen). FreeShow's NDI output
    // is on-air-only by definition; its preview lives in the monitor wall.
    // Screen-window gating (OutputWindowManager → OutputWindow) and the
    // engine's loop priority (OutputListModel::outputsForScreens) both read
    // this, so one flag keeps every consumer of "is this a screen output"
    // consistent. Default false = the HDMI Main Output keeps its window.
    bool onAirOnly = false;
    QList<OutputContentToggle> content;
};

// Mock CRUD backend for the app's output roster (screens/streams a show can
// be sent to). A QML singleton — not a per-screen instance — because both
// Settings · Outputs (OutputsScreen.qml) and the Edit screen's output-
// monitor grid (EditScreen.qml) render the SAME roster from two different,
// unrelated branches of the QML tree; a plain property couldn't be handed
// down to both without threading it through Main.qml. Adding/removing an
// output, renaming, restyling or toggling content now shows up in both
// places at once instead of two hand-typed arrays silently drifting apart.
//
// Style plumbing: outputs reference styles by id (StyleListModel's stable
// ids). When the output that goes live carries a style, the model composes
// the engine's OutputStyleSpec and pushes it through SettingsService into
// PresentationEngine::SetActiveOutputStyle — the live render loop picks it
// up on its next frame (the 1s re-Prepare).
class OutputListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        BadgeRole,
        KindRole,
        ResRole,
        RefreshRole,
        TestPatternRole,
        ScreenNameRole,
        BoundsLockedRole,
        StayOnTopRole,
        FullscreenOutputRole,
        ActiveRole,
        EnabledRole,
        StyleIdRole,
        StyleNameRole,
        // True for network-kind outputs (NDI): they carry ONLY their own
        // transport — no physical-screen window (OutputWindow), no place in
        // the live loop's screen priority (outputsForScreens). The transport
        // mutual-exclusion contract's per-row switch.
        OnAirOnlyRole,
        ContentRole,
        // This output's style background as a QVariantMap { color, image,
        // hasImage } — what a CLEAR output (nothing on air, or a cleared
        // screen) shows in its monitor tile, and what sits under the block
        // render while live. A ROLE so every tile's preview re-evaluates the
        // moment the style or its assignment changes: an INVOKABLE (the old
        // activeStyleBackground()) is opaque to the QML engine, so the binding
        // captured its value once at creation and never moved again.
        StyleBackgroundRole,
        // This output's own gated frame-buffer name ("__out_<hash>__" when
        // the output wears a style, "" otherwise) — the monitor tile reads
        // image://livepreview/<name> so a live output shows ITS OWN gated
        // pass (its style's content rules), not the shared preview feed.
        FrameBufferRole,
    };
    Q_ENUM(Role)

    explicit OutputListModel(QObject *parent = nullptr);
    // Model-to-model callers (StyleListModel::removeStyle) use this.
    //
    // EAGER, not lazy (fixed 2026-10-01). QML_SINGLETON alone lets QML build
    // the object itself on first property access, so a C++ reader running
    // earlier got null and silently did nothing. create() below hands QML the
    // same static instance C++ reads, so the two cannot disagree about
    // whether it exists.
    static OutputListModel *instance();
    static OutputListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // type: "HDMI" | "NDI" | "SDI" | "REC" | "STREAM" — stored as the
    // item's kind and baked into the generated badge.
    Q_INVOKABLE void addScreen(const QString &name, const QString &type,
                               const QString &resolution, const QString &refresh,
                               const QString &testPattern = QStringLiteral("none"));
    Q_INVOKABLE void addOutput();
    // Refuses for the Main Output (see kMainOutputName in the .cpp) — the
    // UI hides the Delete affordance for it; this is the model-side backstop.
    Q_INVOKABLE void removeOutput(int index);
    Q_INVOKABLE bool isMainOutput(int index) const;
    Q_INVOKABLE void setActive(int index);
    Q_INVOKABLE void setEnabled(int index, bool on);
    Q_INVOKABLE void duplicateOutput(int index);
    Q_INVOKABLE void renameOutput(int index, const QString &name);
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setResolution(int index, const QString &res);
    Q_INVOKABLE void setRefresh(int index, const QString &refresh);
    Q_INVOKABLE void setTestPattern(int index, const QString &pattern);
    // Assign a style by row (the picker's natural currency; "" = None).
    Q_INVOKABLE void setStyle(int index, const QString &styleId);
    Q_INVOKABLE void toggleContent(int index, const QString &key);

    // Snapshot for edit dialogs — one QVariantMap per output with the same
    // keys as the role names, so QML editors don't keep parallel state.
    Q_INVOKABLE QVariantMap getOutput(int index) const;
    // Row of the on-air (active) output; -1 when none is active.
    Q_INVOKABLE int activeIndex() const;

    // ---- Physical displays (FreeShow's Screens.svelte: real displays, not
    // a mock map) ----
    // Bumps on every monitor hot-plug, unplug and display-mode change.
    // screensAdded()/screensRemoved() only fire the SIGNAL — this property
    // is what lets a QML binding depend on it (OutputWindow's geometry,
    // ScreenForm's map re-evaluate against it). A binding cannot track a
    // bare signal.
    Q_PROPERTY(int screensRevision READ screensRevision NOTIFY screensChanged)
    int screensRevision() const;

    // One entry per QScreen: { name, label, x, y, width, height, refresh } —
    // geometry in its native pixels so the form's mini-map can lay the tiles
    // out relative to each other exactly like the desktop.
    Q_INVOKABLE QVariantList displays() const;
    // The display an output is assigned to, as a display() entry ({} = none).
    Q_INVOKABLE QVariantMap displayFor(int index) const;
    // Assign/output-side setters. setScreen also snaps the output's res and
    // refresh to the display's current mode (resolution derives from the
    // display, like FreeShow's outputLabel).
    Q_INVOKABLE void setScreenName(int index, const QString &screenName);
    Q_INVOKABLE void setBoundsLocked(int index, bool locked);
    Q_INVOKABLE void setStayOnTop(int index, bool stayOnTop);
    Q_INVOKABLE void setFullscreenOutput(int index, bool fullscreen);

    // Model-side entry point for StyleListModel::removeStyle — re-points
    // every output that used the removed id to "None" without a save/emit
    // round trip through QML.
    void detachStyleEverywhere(const QString &styleId);

    // Rows (roster order) whose transport is a PHYSICAL SCREEN — enabled
    // HDMI rows without onAirOnly. The live loop's SyncWithDocument consults
    // this for per-output frame priority: an NDI row never wins a screen
    // slot, so a switched-to-NDI Main Output can never start the screen
    // pipeline alongside its feed. Rows come and go from the roster, not
    // from liveness — OutputWindowManager's Instantiator keeps the FULL
    // roster as its model.
    QList<int> outputsForScreens() const;

    // The ACTIVE output's style, re-resolved (by id) from the current
    // roster. Empty string = the on-air output has no style.
    QString activeStyleId() const;

    // The ACTIVE output's style admits `contentType`? (the Edit dialog's four
    // pills — a tab toggled OFF greys out, and go-live refuses its content:
    // FreeShow's "this output is not meant for that content" contract.) No
    // style on the active output = everything is allowed. PUBLIC: QML calls
    // this before going live (a private Q_INVOKABLE is invisible to QML).
    Q_INVOKABLE bool activeStyleAllows(const QString &contentType) const;

    // This output's style background as QML colours: { color, image,
    // hasImage }. What the output renders behind its content when nothing
    // else paints there (the engine's OutputStyleSpec): a monitor tile paints
    // it when the output is CLEAR (off air) — the style's look, not a
    // transparency checkerboard that reads as "unstyled" — and under the
    // block render while live (DesignPreview draws only flat colours, so a
    // style background IMAGE would be invisible on the tile otherwise).
    // Delivered as the StyleBackground ROLE (per OUTPUT, not a global "active"
    // invokable): data() re-resolves through StyleListModel per read, and the
    // dataChanged/rosterChanged relays below make every tile re-evaluate.
    // Q_INVOKABLE: the self-test scenario reads every output's background
    // straight from QML (OutputListModel.styleBackground(i)) to log the
    // state beside the monitor-wall grab.
    Q_INVOKABLE QVariantMap styleBackground(int index) const;

signals:
    void activeStyleChanged();
    // A monitor was hot-plugged, unplugged, or its mode (resolution /
    // refresh) changed. QML: ScreenForm bumps its displayRev on this;
    // OutputWindow tracks screensRevision (property, not this signal —
    // a binding can't depend on a bare signal) to re-read its screen's
    // geometry live.
    void screensChanged();

private:
    // Re-snaps every screen-bound row's res/refresh to its display's CURRENT
    // mode (FreeShow's "resolution derives from the display" contract) and
    // emits screensChanged(). Fired by QGuiApplication::screenAdded/
    // screenRemoved and each QScreen's geometry/refresh-rate change. The
    // screenName of a row whose monitor was unplugged is deliberately KEPT:
    // displayFor() returns {} while the display is gone (its OutputWindow
    // hides itself via hasDisplay), and when the monitor comes back with the
    // same name (Windows display names are stable, \\.\DISPLAYn) the binding
    // resumes with no user action — which is exactly what a mid-service
    // unplug needs.
    void screensChangedNow();
    // Wires QGuiApplication::screenAdded/screenRemoved + every currently
    // attached screen into screensChangedNow(). Called once from the
    // constructor (the singleton never dies, so one wiring is enough).
    void trackScreens();
    // Wires one screen's geometryChanged/refreshRateChanged into
    // screensChangedNow(). Idempotent (UniqueConnection); the connection
    // dies with the QScreen when it is unplugged.
    void trackScreen(QScreen *screen);

    int m_screensRevision = 0;

    // Style lookup helpers over StyleListModel::instance() (null-safe).
    QString styleIdAt(int row) const;
    QString styleNameAt(int row) const;
    // Composes the engine spec for `styleId` and pushes it to the
    // PresentationEngine. styleId "" pushes an empty spec (unstyled).
    void pushEngineStyle(const QString &styleId);
    // When the spec's templateKey names an ENGINE TEMPLATE design ("tpl-…"),
    // copies that design's blocks into spec.templateBlocks — the engine
    // renders the template as the style's layout. Legacy preset keys and
    // unknown ids bake nothing (the engine keeps its plain-layout fallback).
    void bakeTemplateBlocks(bps::presentation::OutputStyleSpec &spec);
    // Bakes each per-family template design's blocks into the spec's
    // familyTemplateBlocks slots (no-op for ""/preset family keys).
    void bakeFamilyTemplateBlocks(bps::presentation::OutputStyleSpec &spec);
    // Re-pushes the ACTIVE output's style (the one entry point every "the
    // on-air look changed" relay funnels into). No-op before the roster is
    // adopted.
    void pushActiveEngineStyle();
    // Toast when an output's Refresh rate actually changes (Settings · Outputs
    // edit, or a display-mode re-snap) — the operator's cue that NDI/HDMI/SDI
    // output cadence moved. oldRate/newRate are the display strings ("60 Hz");
    // a blank side is "no rate yet" and is deliberately silent.
    void notifyFrameRateChanged(int row, const QString &oldRate, const QString &newRate);
    // Id → row through StyleListModel::rowForId (-1 = unknown id).
    int styleRowForId(const QString &styleId) const;

    // Wires the rosterChanged relay (see the constructor comment for why it
    // can't just be done once in the constructor). Idempotent.
    void connectToStyleRoster();
    // Wires TemplateLibraryService::changed — an edit to the template design
    // a style wears must re-bake + re-push the spec. Same lazy-singleton
    // retry story as connectToStyleRoster. Idempotent.
    void connectToTemplateLibrary();
    // Pushes the active output's saved style once the engine has booted —
    // without this a session's first go-live rendered unstyled (the other
    // push paths all predate boot). Idempotent.
    void connectToEngineBoot();

    QList<OutputItem> m_outputs;
    bool styleRosterConnected_ = false;
    bool templateLibraryConnected_ = false;
    bool engineBootConnected_ = false;

    // Persists the roster into the engine's OutputStore (the kernel
    // database's "outputs" collection) — the style ASSIGNMENT, screen
    // assignment, enabled/test-pattern/content state all survive a
    // restart. Every mutator calls it; hydrate happens in the constructor.
    void saveRoster();
    // Re-reads the roster from the engine's OutputStore: at construction AND
    // on EngineBridge::bootedChanged — this singleton is built at QML load,
    // before the deferred boot has opened kernel.json, so the first read is
    // always empty and the user's saved outputs would never come back.
    void reloadFromStore();

    static QPointer<OutputListModel> s_instance;
    static QList<OutputContentToggle> defaultContent();
};
