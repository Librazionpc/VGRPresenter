#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QTimer>
#include <QVariantMap>
#include <qqml.h>

// One effect slot in an input's effects rack (reference:
// VGRPresenter_Settings_Audio_Video_Add_Compressor.qml's rack — Gain /
// Equalizer / Compressor / Reverb / Limiter / Noise Gate / Delay).
// HALF-REAL today, by design: `enabled`/`value` are pushed onto REAL
// production-graph processing stages (AudioInputListModel::pushEffectsToGraph
// — the graph is the engine's routing/DSP state), while the rack's stored
// numbers are still cosmetic UI state, not live DSP parameters read by a
// playback device. Don't add "there is no engine" claims here without
// checking pushEffectsToGraph first — this struct is mid-migration.
struct AudioEffect
{
    // "gain" | "eq" | "compressor" | "reverb" | "limiter" | "noiseGate" | "delay"
    QString key;
    QString label;
    bool enabled = false;
    qreal value = 0;
    qreal minValue = 0;
    qreal maxValue = 100;
    // Unit lives with the effect (" dB", "-band", ":1", "%", " ms") so QML
    // never hardcodes a per-key lookup table.
    QString suffix;
};

// One audio source available for routing — a microphone, a line input, the
// system's audio (input or output capture), or a plain media player.
// Settings · Audio & Video's left column
// (VGRPresenter_Settings_Audio_Video.qml's "AUDIO INPUTS" list).
struct AudioInputItem
{
    // Stable identity — survives row removals (rows after it shift; this
    // doesn't). BusListModel routes key graph edges on it, and the routing
    // matrix stores bus numbers against it, so neither follows row order.
    // Assigned at add/duplicate time; PERSISTED with the roster (so a
    // restored row keeps its graph routes) but never shown.
    QString id;
    QString name;
    // "device" | "media" — a real sound device (mic, line in, system/output
    // capture — anything the OS mixer sees) or a plain media player; drives
    // the Add/Edit kind chips. Display-only today, same role as
    // OutputItem::kind.
    QString kind = QStringLiteral("device");
    // Display caption under the name ("System sounds", "120 BPM · click").
    QString sublabel;
    // 0-100 source fader. New routed sources start at 75 (the same practical
    // headroom as new buses); the live meter remains a separate device read.
    qreal level = 75;
    bool muted = false;
    // Hardware latency compensation in milliseconds (the dialog's Delay
    // stepper). 0..1000, step 10 in the UI.
    int delayMs = 0;
    // Channel count — defaults to 2 (the reference's two channel rows);
    // drives the dialogs' Channels block rows. A live meter tap's layout
    // (mono/stereo/N) overrides it for DISPLAY while metering; this stored
    // count is the fallback for non-device rows and when nothing meters.
    int channels = 2;
    // Per-channel gain faders (0..1, index = channel row; default 1 = unity
    // for every channel the roster knows about) — the green knobs in the
    // dialogs' channel rows. The gain is stored on the graph source node and
    // applied independently to each channel by AudioMixer.
    QList<qreal> channelGains;
    // The per-side TRIM each channel was closed with (0..1, index = channel
    // row; absent or 0 = nothing remembered). Closing a side writes gain 0 to
    // channelGains, which loses the level it was at — this is where that level
    // is kept so reopening it gives back the trim instead of unity (see
    // models/ChannelTrim.h, the one definition of that policy). Persisted with
    // the rest of the row so it also survives a restart.
    QList<qreal> channelTrims;
    // Routing matrix (the Routing… modal): per-input-channel lists of BUS
    // NUMBERS (BusItem::id — stable) that channel feeds, NOT row indices —
    // rows shift on removal, bus numbers don't. QML still passes/receives
    // rows (the model translates at the boundary, its one translation job).
    // Auto mode overrides the matrix visually (every channel → every bus)
    // without touching stored data — toggling Auto off reveals the manual
    // pattern again.
    bool routingAuto = false;
    QList<QList<int>> channelRoutes;
    QList<AudioEffect> effects;
};

// QML singleton backing Settings · Audio & Video's Audio Inputs column and
// the routing a BusListModel bus stores against it.
//
// ROUTING KEYS ON STABLE IDS, NOT ROWS: BusListModel's graph edges are cut
// to "asrc:<id>" nodes (one per row, created on demand), and the per-channel
// matrix stores bus numbers. Row indices exist only at the QML boundary —
// toggleChannelRoute/toggleAudioRoute take rows, the models translate to
// ids/numbers internally, so deleting a row can never shift anyone else's
// routes. Static accessors let the three models translate across themselves
// (all QML-singleton instances; created before any routing call runs).
class AudioInputListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // C++ access to the singleton.
    //
    // EAGER, not lazy (fixed 2026-10-01). It used to be "null until QML
    // constructs it", which quietly assumed every C++ reader runs after the
    // Audio & Video screen exists — and the static row/stable-id translation
    // entry points below are called from BusListModel's rebuild path, which
    // runs at construction, before any screen. create() below hands QML the
    // same static instance C++ reads, so existence is not a race.
    static AudioInputListModel *Instance();
    static AudioInputListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);
    // THE ONE OBJECT (see StyleListModel::create's note): the QML engine
    // DEFAULT-CONSTRUCTS a QML_SINGLETON C++ type, so the roster the board holds
    // is not the object Instance() returns unless it is handed over. main.cpp
    // resolves the engine's own instance once the module is imported and adopts
    // it here; whatever instance is displaced — the one C++ built for itself —
    // is deactivated, so it can no longer write the roster or push state into
    // the graph. Two live rosters is what let a row resolve to "no such source"
    // (the reader held the list that never saw the new row) and what let a
    // removed row's route be re-pushed from the other view's stale matrix.
    static void adoptInstance(AudioInputListModel *roster);

    // Row <-> stable id. rowForStableId returns -1 when the id belongs to a
    // removed row — callers treat that as "no such route".
    static QString stableIdForRow(int row);
    static int rowForStableId(const QString &id);

    // The row's stable id, HEALING a row that has none: the board addresses a
    // row by its stable id ("asrc:<id>"), and an id-less row used to make its
    // own routing impossible — the connect path reads an empty id as "this row
    // has nothing to route" and refuses the drop, with no way for the user to
    // fix it ("a new audio source refuses to connect"). A row is now given its
    // id the moment routing asks for it, so every row on the board can be
    // connected. Returns an empty string only for a row that does not exist.
    Q_INVOKABLE QString idForRow(int row);
    enum Role {
        NameRole = Qt::UserRole + 1,
        KindRole,
        SublabelRole,
        LevelRole,
        MutedRole,
        DelayMsRole,
        ChannelsRole,
        ChannelGainsRole,
        RoutingAutoRole,
        RoutingRole,
        EffectsRole,
    };
    Q_ENUM(Role)

    explicit AudioInputListModel(QObject *parent = nullptr);
    ~AudioInputListModel() override;

    // The reference's rack defaults — Gain/EQ/Compressor/Reverb on,
    // Limiter/Noise Gate/Delay off. Shared by the seeds, addInput() and the
    // Add Source dialog's pre-row snapshot (defaultEffectsTemplate()).
    static QList<AudioEffect> defaultEffects();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addInput();
    Q_INVOKABLE void removeInput(int index);
    // Insert a copy of row `index` (name suffixed " · copy", mute NOT
    // copied); returns the new row's index, -1 on a bad index. Same
    // convention as OutputListModel::duplicateOutput.
    Q_INVOKABLE int duplicateInput(int index);
    Q_INVOKABLE void renameInput(int index, const QString &name);
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setSublabel(int index, const QString &sublabel);
    // Native audio-file picker (mp3/wav/flac/...) for the Media-kind Select
    // Media picker's "Browse this PC..." option — files not (yet) indexed
    // by the Media Library. "" when cancelled. Lives here (not QML) so the
    // engine's dialog service is used, same as every other file picker in
    // the app (see StyleListModel::pickImageFile).
    Q_INVOKABLE QString pickAudioFile() const;
    Q_INVOKABLE void setLevel(int index, qreal level);
    Q_INVOKABLE void setMuted(int index, bool muted);
    // Latency compensation (clamped 0..1000 ms) and channel count (clamped
    // 1..8) — the pro-audio form's rows.
    Q_INVOKABLE void setDelayMs(int index, int delayMs);
    Q_INVOKABLE void setChannels(int index, int channels);
    // Per-channel gain fader (0..1, clamped) — the channel row's knob. Any OPEN
    // level written here is also remembered as that side's trim, so no caller
    // has to remember to record it before closing the side.
    Q_INVOKABLE void setChannelGain(int index, int channel, qreal gain);
    Q_INVOKABLE qreal channelGain(int index, int channel) const;
    // The L/R pill's write, and the ONE place the reopen policy lives: a closed
    // side reopens at the trim it was closed with (models/ChannelTrim.h), an
    // open side closes. Callers stop hand-rolling `closed ? 1.0 : 0.0` — that
    // expression WAS the bug, with its own copy on every surface.
    Q_INVOKABLE void toggleChannelGain(int index, int channel);
    // The gain this side should come back at (never the 0 that "closed" stores):
    // the remembered trim, else the level it is holding open now, else unity.
    // The Settings dialogs' channel rows read THIS, so their knobs and the Media
    // pane's pills reopen a side identically.
    Q_INVOKABLE qreal channelTrim(int index, int channel) const;
    // Remember a trim WITHOUT changing the gain. setChannelGain already
    // remembers every open level it writes, so this is only for an explicit
    // "this IS the trim" (a knob moved while the side is closed).
    Q_INVOKABLE void setChannelTrim(int index, int channel, qreal trim);

    // Routing matrix — the Routing… modal's data. getRouting returns
    // { auto, channels, buses, routes } in one read (buses are BusListModel
    // rows, read by QML separately); toggles are per cell. setChannelRoutes
    // batch-writes a whole matrix (the Add dialog's buffered state),
    // replacing whatever was stored.
    Q_INVOKABLE QVariantMap getRouting(int index) const;
    Q_INVOKABLE void setRoutingAuto(int index, bool auto_);
    Q_INVOKABLE void toggleChannelRoute(int index, int channel, int busIndex);
    Q_INVOKABLE void setChannelRoutes(int index, const QVariantList &perChannel);

    // Effects rack writes — per-effect by key, bounded-checked like every
    // other setter, no-op when unchanged, dataChanged({EffectsRole}).
    Q_INVOKABLE void setEffectEnabled(int index, const QString &key, bool enabled);
    Q_INVOKABLE void setEffectValue(int index, const QString &key, qreal value);

    // Snapshot of defaultEffects() for the Add Source dialog — it collects
    // effect state BEFORE any row exists, then writes it through
    // setEffectEnabled/setEffectValue after addInput().
    Q_INVOKABLE QVariantList defaultEffectsTemplate() const;

    // Snapshot for the Edit Audio Input dialog — same convention as
    // OutputListModel::getOutput / StyleListModel::getStyle.
    Q_INVOKABLE QVariantMap getInput(int index) const;

    // ---- Roster persistence ---------------------------------------------
    // The whole roster (rows + their settings + stable ids) as a JSON string
    // for the settings store (session.audioRoster), and restore from it.
    // saveRoster is debounced (a dragging fader fires dozens of writes); the
    // destructor flushes any pending save. Restore is idempotent and returns
    // the number of rows rehydrated; ids are preserved so restored rows keep
    // their graph routes (asrc:<id> edges survive because BusListModel's
    // restore re-links by the same ids).
    Q_INVOKABLE void saveRoster();
    Q_INVOKABLE int restoreRoster();
    // Has the store ANSWERED this roster yet? It needs the engine booted and the
    // settings store alive, so "empty" means "not loaded" until this turns true.
    // BusListModel's ghost sweep (see pruneOrphanSourceEdges) treats a roster as
    // the authority for which source nodes are still live, and must not sweep a
    // kind whose rows simply have not arrived.
    static bool restoreDone();

    // ---- REAL DSP PUSH (the production graph) ---------------------------
    // The effects rack / level / mute of one row, translated onto that
    // row's PRODUCTION GRAPH SOURCE NODE ("asrc:<id>") as engine-side
    // VolumeControl + ProcessingStages — the same stages the routing board's
    // graph carries (ProductionTypes: Eq/Compressor/Limiter/Gate/Delay/Gain).
    // No-op when the row is unrouted (its node does not exist — processing on
    // an unpatched source means nothing) or the engine is not booted. Called
    // from every effects/level/mute write so the board IS the graph's truth.
    void pushEffectsToGraph(int index);

    // ROUTING MATRIX → GRAPH. The matrix (the Routing… modal) is per CHANNEL ×
    // BUS, while a graph edge is per source→bus, so the two halves split: this
    // ensures the EDGE exists for every bus the row's matrix names (a checked
    // cell anywhere means that source feeds that bus) and writes the CHANNEL
    // MASK the mixer reads ("uiRouteMask:<busNode>", bit c = channel c is routed
    // there) so a row routed on one channel only sounds on that channel of that
    // bus.
    //
    // WHY THIS HAD TO EXIST: the matrix's selections were STORED (the session
    // roster) and read back into the dialog, but nothing ever told the engine —
    // a route made there produced no edge, no audio and no meter, which is the
    // "the engine is not saving connections in the audio and video settings"
    // report. It also cuts the edges it made ITSELF and no longer names (the
    // "uiRouteMask:<busNode>" meta on the row's node is that marker): unticking
    // the last channel of a bus used to leave the edge AND the stale channel mask
    // in the graph, so the row kept feeding — and sounding on — a bus its matrix
    // no longer routed to. A route made on the BOARD or in the bus dialog carries
    // no mask and is never touched here, so the surfaces still cannot fight over
    // one edge. No-op when the engine is not booted or the bus row is gone.
    void pushRoutingToGraph(int index);

private:
    static inline AudioInputListModel *s_instance = nullptr;

    // False once this instance has been displaced by an adopted one (see
    // adoptInstance): every write path below no-ops, because this object is not
    // the roster anyone is looking at.
    bool m_active = true;
    void deactivate();

    // The single write of the trim list (the public faces above only decide
    // WHEN a trim is worth remembering). true when it changed anything.
    bool writeChannelTrim(int index, int channel, qreal trim);

    QList<AudioInputItem> m_inputs;
    int m_nextId = 1;   // stable-id counter ("a<n>"); seeded past the restored roster's highest id
    bool m_restoreDone = false;     // the store has answered once (see restoreDone)
    QTimer *saveTimer_ = nullptr;   // debounce for saveRoster()
    void scheduleSave();            // bump the debounce (every mutation calls this)
    QString serializeRoster() const;
    void queueRosterWrite();        // one line through the settings store
};
