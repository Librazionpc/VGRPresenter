#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVector>
#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>
#include <QVariantList>
#include <qqml.h>

// Fully defined, not forward-declared: adoptStyleRoster takes it by pointer and
// moc needs the complete type to build the metatype for that slot.
#include "models/StyleListModel.h"

namespace bps::presentation {

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
    // The row's STABLE identity, and the only thing that may name it on the
    // engine side. A NAME cannot do that job: it is an editable label, so a
    // rename would orphan everything keyed on it — this row's node in the
    // production graph (Settings · Routing's wires) first of all. Assigned
    // once when the row is created, healed here for older rosters, and
    // persisted through OutputStore (`id`), exactly like StyleItem::id and
    // BusItem's bus number — the same "identity, never position or label"
    // rule the routing models already follow.
    QString id;
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
    // "direct" uses the output's selected video bus full-frame; "compose"
    // paints the output's design and only its wired bus windows.
    QString videoMode = QStringLiteral("direct");
    // THIS OUTPUT's own content gates — the WIRES from the Overlays / Media cards
    // at the head of Settings · Routing's Styles column. PER OUTPUT on purpose:
    // they used to live on the STYLE, so closing media for one screen closed it for
    // every screen wearing that theme. A style's own gates still apply on top (both
    // must allow), so nothing here can open what a theme forbids.
    //
    // FALSE is the default because the wire IS the permission: an output with no
    // wire from the Overlays card paints no overlay, and the same for media (see
    // StoredOutput::showOverlay). The row's own O/M chips are the same flag.
    bool showOverlay = false;
    bool showMedia = false;
    // LEGACY — a per-output Fill that NOTHING composes from any more. How a
    // taken file sits is the FILE's own choice (Media right-click → Fill), else
    // contain: one picture, one shape on every output (SceneBuilder
    // ::AddCompositorLayers). Two outputs left on different fills is what made
    // one take read as two pictures on the wall. The field is still read from
    // and written back to the roster so an older store round-trips unchanged.
    QString mediaFit = QStringLiteral("contain");
    // Which audio endpoint THIS row's mix plays on, as the audio roster's id
    // ("waveout:<n>"). Empty = the program stream, which is where every row's
    // audio has always gone (and what an older roster means). Set = that device's
    // own render stream, so an HDMI output's sound comes out of that monitor
    // rather than the booth speakers (see OutputStore::audioDevice).
    QString audioDevice;
    // REC / STREAM rows: the stable id of the recording / streaming preset this
    // output uses (PresetListModel). "" = none picked.
    QString presetId;
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
        // What this output's STYLE does with a slide, in the words Settings ·
        // Outputs shows under the Style pill: whether it restyles the content
        // tabs' designs while a show keeps its own authored layout, or it lays
        // shows out too, or it has no template layout at all (background only).
        // Computed from the SAME engine rule the scene builder composes with
        // (SlideRules::LayoutScope), so the sentence cannot drift from what the
        // NDI/HDMI picture does — which is the whole point of printing it.
        StyleLayoutRole,
        // This output's media fill choice ("contain" | "cover" | "fill") —
        // legacy, see OutputItem::mediaFit: reported, never composed from.
        MediaFitRole,
        // The row's stable id (see OutputItem::id). Settings · Routing keys its
        // output node on this, so a wire survives a rename.
        IdRole,
        // This row's audio endpoint ("waveout:<n>", "" = the program). Settings ·
        // Outputs' picker, and what RoutingService reads to decide whether the
        // row plays on a stream of its own.
        AudioDeviceRole,
        // The preset a REC / STREAM row records / streams with (PresetListModel id).
        PresetIdRole,
        VideoModeRole,
        // This row's own Overlays / Media gates (see OutputItem::showOverlay).
        OverlayGateRole,
        MediaGateRole,
    };
    Q_ENUM(Role)

    explicit OutputListModel(QObject *parent = nullptr);
    // THE roster, for C++ AND QML.
    //
    // EAGER, not lazy (fixed 2026-10-01). QML_SINGLETON alone let QML build
    // the object itself on first property access, so a C++ reader running
    // earlier got null and silently did nothing.
    //
    // AND WHICHEVER OBJECT EXISTS IS THE ROSTER (fixed 2026-10-24). QML does
    // not call create() below — it DEFAULT-CONSTRUCTS the registered singleton
    // (see main.cpp's note) — so a plain `static` here built a SECOND roster:
    // QML edited one, every C++ reader resolved the other, and an output added
    // mid-session reached the settings screen but never the NDI feed (no row in
    // the feed's plan, so no sender was armed for it — it only appeared after a
    // restart, when both copies hydrate from the same OutputStore). Returning
    // s_instance makes the object the constructor registered the one every
    // reader gets, so the two halves cannot disagree about the roster.
    static OutputListModel *instance();
    // Hand over the instance the QML engine actually built, so instance()
    // resolves it rather than a second, private roster (see main.cpp, which
    // makes this call after loadFromModule — the same one-object-per-model rule
    // the routing models follow). Safe to call with null (logged, no-op).
    static void adoptEngineInstance(OutputListModel *model);
    // See StyleListModel::create's comment: the engine does not call this,
    // which is why the style roster is handed over explicitly at startup.
    static OutputListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // type: "HDMI" | "NDI" | "SDI" | "REC" | "STREAM" — stored as the
    // item's kind and baked into the generated badge.
    // ---- The style roster C++ reads -------------------------------------
    // QML builds its OWN StyleListModel: the engine does not call that class's
    // create() factory (it registers the type with qmlRegisterTypesAndRevisions
    // and then default-constructs the singleton), so StyleListModel::instance()
    // and the object the Styles screen edits are two different rosters unless
    // they are joined explicitly. Main.qml joins them at startup by handing its
    // instance in here, and EVERY style lookup goes through styleRoster() —
    // otherwise a style edit reaches the tiles, the output windows and the
    // engine's spec only after a restart. PUBLIC, not private: a Q_INVOKABLE in
    // a private section is in the metaobject but is NOT exposed as a callable
    // QML property (QML reported "Property 'adoptStyleRoster' ... is not a
    // function" while this sat below `private:`).
    Q_INVOKABLE void adoptStyleRoster(StyleListModel *styles);
    // The roster Main.qml handed over, for C++ readers that resolve styles
    // WITHOUT going through an OutputListModel object — StyleSpecBuilder (the
    // preview's composition, and every spec this model pushes) is the one that
    // matters.
    // STATIC on purpose: QML builds its own OutputListModel as well (the
    // engine default-constructs the registered singleton instead of calling
    // create()), and the hand-over lands on THAT object, so a reader holding
    // the C++ `instance()` would see no adoption and fall back to
    // StyleListModel::instance() — a second roster holding the style as it was
    // at boot. Composing a monitor tile through it produced a different
    // template and background from the real output ("it only happens on the
    // preview tile"). Null until the hand-over; callers fall back to
    // StyleListModel::instance().
    static StyleListModel *adoptedStyleRoster();

    Q_INVOKABLE void addScreen(const QString &name, const QString &type,
                               const QString &resolution, const QString &refresh,
                               const QString &testPattern = QStringLiteral("none"));
    Q_INVOKABLE void addOutput();
    // Refuses for the Main Output (see kMainOutputName in the .cpp) — the
    // UI hides the Delete affordance for it; this is the model-side backstop.
    Q_INVOKABLE void removeOutput(int index);
    Q_INVOKABLE bool isMainOutput(int index) const;
    Q_INVOKABLE void setActive(int index);
    // Returns false when disabling this row would leave no enabled output.
    Q_INVOKABLE bool setEnabled(int index, bool on);
    Q_INVOKABLE void duplicateOutput(int index);
    Q_INVOKABLE void renameOutput(int index, const QString &name);
    // One name per row. A name IS this roster's key everywhere else — the NDI
    // provider map, the per-output send clock, the published per-row state and
    // the styled frame buffer are all indexed by it — so two rows sharing one
    // name are ONE output to every reader: the second can never be fed and
    // nothing can report it (the live report "the output I just created isn't
    // sending" — its auto name, `New Screen <row count + 1>`, collided with a
    // row that already had that number). Used by addScreen, renameOutput and
    // the hydrate-time heal for rosters saved before this rule.
    QString uniqueOutputName(const QString &candidate, int skipIndex = -1) const;
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setResolution(int index, const QString &res);
    Q_INVOKABLE void setRefresh(int index, const QString &refresh);
    Q_INVOKABLE void setTestPattern(int index, const QString &pattern);
    // Assign a style by row (the picker's natural currency; "" = None).
    Q_INVOKABLE void setStyle(int index, const QString &styleId);
    Q_INVOKABLE void toggleContent(int index, const QString &key);

    // ---- Stable identity (see OutputItem::id) ----
    // The id of a row, and its row again ("" / -1 for a bad or vanished one).
    // RoutingService builds each row's engine node id from this, so the two
    // lookups are the only way anything outside should reach an identity: a
    // QML delegate can read the `outputId` role instead.
    Q_INVOKABLE QString stableIdFor(int row) const;
    Q_INVOKABLE int rowForStableId(const QString &id) const;

    // Snapshot for edit dialogs — one QVariantMap per output with the same
    // keys as the role names, so QML editors don't keep parallel state.
    Q_INVOKABLE QVariantMap getOutput(int index) const;
    // Has the PERSISTED roster actually been read? False until a hydrate has run
    // with the engine booted, which is the only hydrate that can see kernel.json
    // (this singleton is built at QML load, before the Database is opened, so its
    // constructor's read always comes back empty). Anything that treats "no rows"
    // as "the operator deleted them" — RoutingService prunes graph nodes the
    // roster does not own — must ask this first, or a boot-time read of an empty
    // roster erases what the graph just restored.
    bool rosterReady() const { return rosterReady_; }
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
    // Which endpoint this row's audio plays on ("waveout:<n>", "" = the program).
    // Persisted and re-bound: the routing pass opens/closes the stream, because a
    // wire is what decides whether the row has anything to play there.
    Q_INVOKABLE void setAudioDevice(int index, const QString &deviceId);
    // Wire a REC / STREAM row to a preset ("" = none). Persisted; re-asks Routing,
    // because a stream preset's target is part of the row's output node.
    Q_INVOKABLE void setPresetId(int index, const QString &presetId);
    // Rows wired to this preset, and the model-side backstop when it is deleted.
    int presetUsage(const QString &presetId) const;
    void detachPresetEverywhere(const QString &presetId);
    // A preset was edited: rows that wear it re-read.
    void presetChanged(const QString &presetId);
    Q_INVOKABLE void setVideoMode(int index, const QString &mode);
    // THIS ROW's own gates, written by the two toggle chips on its Settings ·
    // Routing card. Both re-push the engine's per-output specs, so the picture
    // changes on the next frame rather than at the next style edit.
    Q_INVOKABLE void setShowOverlay(int index, bool on);
    Q_INVOKABLE void setShowMedia(int index, bool on);
    // RoutingService's publish for the LIVE paths: `busNumberByRow` is each row's
    // ON-AIR video bus (0 = none), `wiredByRow` its whole wired video SET. The set
    // is what gates a design's bus windows per output — a row with no wire to a bus
    // must not paint that bus, even though the block is in the shared composition.
    void setRoutedVideoBuses(const QVector<int> &busNumberByRow,
                             const QVector<QVector<int>> &wiredByRow);

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
    //
    // The ANSWER is the engine's (SlideRules::StyleAdmitsFamily, asked through
    // the spec the style reaches the outputs as): a second reading of the four
    // flags answered differently in the all-gates-off state, where the engine
    // says "unconfigured — admit everything" and the copy said "refused".
    Q_INVOKABLE bool activeStyleAllows(const QString &contentType) const;

    // The template the OUTPUT really renders `contentType` with: the active
    // output's style names a template for that content family (Settings ·
    // Styles · Scripture / The Table) and SceneBuilder prefers a style's
    // family template over the blocks a slide carries — so a scripture or The
    // Table pick goes on air in the STYLE's template, not in the one the
    // Reference pane itself picked. Empty = the style names none for that
    // family, and the tab's own pick is what reaches the output. QML calls
    // this for the pane's template row and its edit pencil ("edit template: I
    // meant the template selected in Styles and used by output and go live").
    // Read off the style's COMPOSED spec (its baked family slots), not off the
    // style row's property: the bake is what SceneBuilder lays the family out
    // through, fallbacks included.
    Q_INVOKABLE QString activeStyleTemplateId(const QString &contentType) const;
    Q_INVOKABLE QString activeStyleShowCategoryTemplateId(const QString &category) const;

    // This output's style background AS THE MODEL ROLE sees it: { color, image,
    // hasImage, clearOnText }, resolved through the style roster for this
    // output's style id.
    //
    // This is NOT what a surface paints from any more. The monitor tile and the
    // output window draw the style's facts out of the ONE composition call that
    // composes the frame the outputs consume (LiveOutputService
    // .previewWithStyle), so no surface has to resolve a style role for itself
    // and disagree with the frame about it. What is left here is the MODEL'S own
    // reading — the baseline the self-test compares the tile's composed picture
    // against (Q_INVOKABLE so the scenario can log the model's state beside the
    // monitor-wall grab), and the roster relay that makes a style edit visible.
    // Nothing in qml/components reads it.
    Q_INVOKABLE QVariantMap styleBackground(int index) const;

signals:
    void activeStyleChanged();
    // Emitted after the roster is persisted and the engine's output styles
    // are pushed, so live output feeds can safely resolve newly added rows.
    void outputRosterChanged();
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

    // A fresh stable id ("out-<n>", n one past every id in the roster — never
    // reused, so a deleted row's wires can never be inherited by a new one).
    // Used by addScreen, duplicateOutput and the hydrate-time heal.
    QString nextStableId() const;

    int m_screensRevision = 0;

    // The adopted roster, or the C++ singleton as the fallback (null-safe).
    StyleListModel *styleRoster() const;
    // Everything the roster feeds, re-resolved: the tiles' style roles and the
    // engine spec. Runs on adopt AND on every rosterChanged of the ADOPTED
    // roster.
    void styleRosterChanged();

    // Style lookup helpers over styleRoster() (null-safe).
    QString styleIdAt(int row) const;
    QString styleNameAt(int row) const;
    // What this style's TEMPLATE lays out, in the one sentence Settings ·
    // Outputs prints under the Style pill. Answered by the engine's own rule
    // (SlideRules::LayoutScope) applied to the very spec this output pushes, so
    // the sentence and the picture cannot disagree; "" when the row has no
    // style (nothing to describe).
    QString styleLayoutNote(const QString &styleId) const;
    // Composes the engine spec for `styleId` and pushes it to the
    // PresentationEngine. styleId "" pushes an empty spec (unstyled).
    //
    // The COMPOSITION itself is not a member any more: the spec — the style's
    // fields, its per-family template keys, and the blocks of every template
    // design it picks baked in — comes from StyleSpecBuilder, the one function
    // the live preview composes through as well. It used to be written out here
    // twice (this spec and one per other styled output) and once in
    // LiveOutputService, and the three had already drifted: the preview did not
    // carry the style's category and fell back to the Bible layout on a
    // different condition, so the tile could render a layout the outputs were
    // not rendering.
    void pushEngineStyle(const QString &styleId, const QString &slideTag = QString());

public:
    // Re-push the active style with the on-air slide's GROUP template slotted in
    // ("" = the plain style). Called when the on-air slide or a group's template
    // changes, so the output and the monitor tile keep composing the same spec.
    void repushActiveStyleForTag(const QString &tag);

    // DOES THIS OUTPUT GET A RENDER PASS OF ITS OWN? Two things earn one: a
    // STYLE (its gates decide what content it shows) and a VIDEO WIRE (Settings ·
    // Routing — the pass composes that bus's feed). Everything else mirrors the
    // shared program feed, which is also why this is the ONE rule the frame
    // buffer name, the live style-set push and LiveOutputService's feed probe
    // all read: a wired output that pushed a spec but was still READ from the
    // shared buffer would render its bus and then show the program picture
    // anyway. Disabled rows never have one (they are not on air).
    Q_INVOKABLE bool hasOwnPass(int row) const;

    // The VIDEO BUS this row's wire names (bus number, 0 = unwired) — what the
    // engine composes for it and what has to be PULLED so there is a frame to
    // compose: LiveOutputService's bus-feed pump watches every bus a wire names,
    // not only the ones a design block draws (otherwise a wire into a screen with
    // no bus block on the slide would draw its wire and show nothing).
    int routedVideoBus(int row) const;

    // The engine-side frame-buffer NAME this row's picture lands in ("" = the
    // row mirrors the shared program feed). The same string FrameBufferRole
    // hands QML, exposed to C++ so a reader that wants to look the buffer up in
    // the render engine (RoutingService's feed health) does not re-derive the
    // keying and cannot drift from it.
    QString frameBufferFor(int row) const;

    // THE WIRED VIDEO BUS, one entry per roster row — filled by RoutingService
    // from the engine's own OutputConfig (the wire's truth, never a QML copy),
    // 0 = nothing wired. It is read when the live style set is pushed: a row
    // whose video is wired gets that bus number in its spec, and the engine
    // composes THAT feed instead of the program picture (see OutputStyleSpec's
    // videoBus). Indexed by ROW because that is how the spec loop walks the
    // roster; a reorder re-pushes, so a stale index cannot outlive its row.
    void setRoutedVideoBuses(const QVector<int> &busNumberByRow);
    // Something the engine's per-output looks are built from changed outside the roster (Scroll's
    // solved type size, see OutputStyleSpec::readingFontPx): push the active style and the
    // per-output set again. No-op before the roster is adopted.
    void repushEngineStyles() { pushActiveEngineStyle(); }

private:
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
    // See setRoutedVideoBuses: bus NUMBER per roster row, 0 = unwired. Empty
    // until the first routing refresh publishes.
    QVector<int> routedVideoBuses_;
    // Per row: every video bus wired to it, by bus NUMBER — the allow-list a
    // gated scene build is given (OutputStyleSpec::gateVideoBuses).
    QVector<QVector<int>> wiredVideoBuses_;
    // The roster Main.qml handed over (empty until then — see adoptStyleRoster).
    QPointer<StyleListModel> styleRoster_;
    // The same hand-over, visible to EVERY OutputListModel object (QML's and
    // the C++ static) — see adoptedStyleRoster().
    static QPointer<StyleListModel> s_adoptedStyleRoster;
    QMetaObject::Connection styleRosterConn_;
    bool styleRosterConnected_ = false;
    bool templateLibraryConnected_ = false;
    bool engineBootConnected_ = false;
    // See rosterReady(): set by the first hydrate that ran with the engine up.
    bool rosterReady_ = false;

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
