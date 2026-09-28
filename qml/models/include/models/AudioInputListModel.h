#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QTimer>
#include <QVariantMap>
#include <qqml.h>

// One effect slot in an input's effects rack (reference:
// VGRPresenter_Settings_Audio_Video_Add_Compressor.qml's rack — Gain /
// Equalizer / Compressor / Reverb / Limiter / Noise Gate / Delay). A stored
// value, not a live DSP parameter: there is no audio engine anywhere in this
// app, same mock-backend convention as level/muted.
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
    // 0-100 — a settable level, not a live meter (mock backend, no real
    // audio engine anywhere in this app). Defaults to 0: silence until the
    // user raises it — a new source must never appear "live" on its own.
    qreal level = 0;
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
    // dialogs' channel rows. Display-side gain today (scaled into the
    // channel meters, see ProAudioForm); the engine applies it as real DSP
    // once per-channel gain lands in the graph.
    QList<qreal> channelGains;
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
    // C++ access to the QML-created singleton (null until QML constructs
    // it) — the cross-model translation entry points below are only ever
    // called from routing flows, which run after the screen exists.
    static AudioInputListModel *Instance() { return s_instance; }

    // Row <-> stable id. rowForStableId returns -1 when the id belongs to a
    // removed row — callers treat that as "no such route".
    static QString stableIdForRow(int row);
    static int rowForStableId(const QString &id);
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
    // Per-channel gain fader (0..1, clamped) — the channel row's knob.
    Q_INVOKABLE void setChannelGain(int index, int channel, qreal gain);
    Q_INVOKABLE qreal channelGain(int index, int channel) const;

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

    // ---- REAL DSP PUSH (the production graph) ---------------------------
    // The effects rack / level / mute of one row, translated onto that
    // row's PRODUCTION GRAPH SOURCE NODE ("asrc:<id>") as engine-side
    // VolumeControl + ProcessingStages — the same stages the routing board's
    // graph carries (ProductionTypes: Eq/Compressor/Limiter/Gate/Delay/Gain).
    // No-op when the row is unrouted (its node does not exist — processing on
    // an unpatched source means nothing) or the engine is not booted. Called
    // from every effects/level/mute write so the board IS the graph's truth.
    void pushEffectsToGraph(int index);

private:
    static inline AudioInputListModel *s_instance = nullptr;

    QList<AudioInputItem> m_inputs;
    int m_nextId = 1;   // stable-id counter ("a<n>"); seeded past the restored roster's highest id
    QTimer *saveTimer_ = nullptr;   // debounce for saveRoster()
    void scheduleSave();            // bump the debounce (every mutation calls this)
    QString serializeRoster() const;
    void queueRosterWrite();        // one line through the settings store
};
