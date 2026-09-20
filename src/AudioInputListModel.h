#pragma once

#include <QAbstractListModel>
#include <QList>
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
    // Path mode — the Add/Edit dialog's Off / On / Auto Off / Auto On
    // segmented control (0..3). Off until manually enabled by default.
    int mode = 0;
    // Hardware latency compensation in milliseconds (the dialog's Delay
    // stepper). 0..1000, step 10 in the UI.
    int delayMs = 0;
    // Channel count — defaults to 2 (the reference's two channel rows);
    // drives the dialogs' Channels block rows and the signal monitor's
    // strip count (1 = mono, 2 = L/R stereo). Not user-adjustable in the
    // dialogs — a device's real channel count belongs to the engine.
    int channels = 2;
    // Routing matrix (the Routing… modal): per-input-channel lists of bus
    // row indices that channel feeds. Auto mode overrides the matrix
    // visually (every channel → every bus) without touching stored data —
    // toggling Auto off reveals the manual pattern again.
    bool routingAuto = false;
    QList<QList<int>> channelRoutes;
    QList<AudioEffect> effects;
};

// QML singleton backing Settings · Audio & Video's Audio Inputs column and
// the routing a BusListModel bus stores against it (by row index — same
// convention as OutputItem::styleIndex into StyleListModel).
class AudioInputListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        KindRole,
        SublabelRole,
        LevelRole,
        MutedRole,
        ModeRole,
        DelayMsRole,
        ChannelsRole,
        RoutingAutoRole,
        RoutingRole,
        EffectsRole,
    };
    Q_ENUM(Role)

    explicit AudioInputListModel(QObject *parent = nullptr);

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
    Q_INVOKABLE void setLevel(int index, qreal level);
    Q_INVOKABLE void setMuted(int index, bool muted);
    // Path mode (clamped 0..3), latency compensation (clamped 0..1000 ms)
    // and channel count (clamped 1..8) — the pro-audio form's rows.
    Q_INVOKABLE void setMode(int index, int mode);
    Q_INVOKABLE void setDelayMs(int index, int delayMs);
    Q_INVOKABLE void setChannels(int index, int channels);

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

private:
    QList<AudioInputItem> m_inputs;
};
