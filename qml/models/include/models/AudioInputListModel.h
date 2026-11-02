#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QTimer>
#include <QVariantMap>
#include <qqml.h>

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

struct AudioInputItem
{
    QString id;
    QString name;
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
    int channels = 2;
    QList<qreal> channelGains;
    QList<qreal> channelTrims;
    bool routingAuto = false;
    QList<QList<int>> channelRoutes;
    QList<AudioEffect> effects;
};

class AudioInputListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static AudioInputListModel *Instance();
    static AudioInputListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);
    static void adoptInstance(AudioInputListModel *roster);

    // Row <-> stable id. rowForStableId returns -1 when the id belongs to a
    // removed row — callers treat that as "no such route".
    static QString stableIdForRow(int row);
    static int rowForStableId(const QString &id);

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

    static QList<AudioEffect> defaultEffects();

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addInput();
    Q_INVOKABLE void removeInput(int index);
    Q_INVOKABLE int duplicateInput(int index);
    Q_INVOKABLE void renameInput(int index, const QString &name);
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setSublabel(int index, const QString &sublabel);
    Q_INVOKABLE QString pickAudioFile() const;   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread
    Q_INVOKABLE void setLevel(int index, qreal level);
    Q_INVOKABLE void setMuted(int index, bool muted);
    // Latency compensation (clamped 0..1000 ms) and channel count (clamped
    // 1..8) — the pro-audio form's rows.
    Q_INVOKABLE void setDelayMs(int index, int delayMs);
    Q_INVOKABLE void setChannels(int index, int channels);
    Q_INVOKABLE void setChannelGain(int index, int channel, qreal gain);
    Q_INVOKABLE qreal channelGain(int index, int channel) const;
    Q_INVOKABLE void toggleChannelGain(int index, int channel);
    Q_INVOKABLE qreal channelTrim(int index, int channel) const;
    Q_INVOKABLE void setChannelTrim(int index, int channel, qreal trim);

    Q_INVOKABLE QVariantMap getRouting(int index) const;
    Q_INVOKABLE void setRoutingAuto(int index, bool auto_);
    Q_INVOKABLE void toggleChannelRoute(int index, int channel, int busIndex);
    Q_INVOKABLE void setChannelRoutes(int index, const QVariantList &perChannel);

    // Effects rack writes — per-effect by key, bounded-checked like every
    // other setter, no-op when unchanged, dataChanged({EffectsRole}).
    Q_INVOKABLE void setEffectEnabled(int index, const QString &key, bool enabled);
    Q_INVOKABLE void setEffectValue(int index, const QString &key, qreal value);

    Q_INVOKABLE QVariantList defaultEffectsTemplate() const;

    // Snapshot for the Edit Audio Input dialog — same convention as
    // OutputListModel::getOutput / StyleListModel::getStyle.
    Q_INVOKABLE QVariantMap getInput(int index) const;

    // ---- Roster persistence ---------------------------------------------
    Q_INVOKABLE void saveRoster();
    Q_INVOKABLE int restoreRoster();
    static bool restoreDone();

    // ---- REAL DSP PUSH (the production graph) ---------------------------
    void pushEffectsOffThread(int index);

    void pushRoutingOffThread(int index);

private:
    static inline AudioInputListModel *s_instance = nullptr;

    bool m_active = true;
    void deactivate();

    // The single write of the trim list (the public faces above only decide
    // WHEN a trim is worth remembering). true when it changed anything.
    bool writeChannelTrim(int index, int channel, qreal trim);

    bool writeChannelGain(int index, int channel, qreal gain);

    QList<AudioInputItem> m_inputs;
    int m_nextId = 1;   // stable-id counter ("a<n>"); seeded past the restored roster's highest id
    bool m_restoreDone = false;     // the store has answered once (see restoreDone)
    QTimer *saveTimer_ = nullptr;   // debounce for saveRoster()
    void scheduleSave();            // bump the debounce (every mutation calls this)
    QString serializeRoster() const;
    void queueRosterWrite();        // one line through the settings store
};