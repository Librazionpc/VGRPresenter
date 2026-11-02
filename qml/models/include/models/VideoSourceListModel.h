#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include <qqml.h>

class QTimer;

struct VideoSourceItem
{
    QString id;
    QString name;
    QString kind = QStringLiteral("camera");
    // Display caption under the name ("PTZ · Wide", "HDMI in", "MP4 · 4K").
    QString sublabel;
    QString mode;
    // Mute is meaningful for MEDIA rows (they carry audio); camera/screen
    // feeds have no audio of their own to mute.
    bool muted = false;
    qreal level = 0;   // silence by default — a new source starts muted-down
    // PER-SIDE trim of this row's audio, index 0 = L, 1 = R (1.0 = open,
    // 0.0 = that side closed). The SAME convention AudioInputListModel's
    QList<qreal> channelGains;
    QList<qreal> channelTrims;
};

class VideoSourceListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static VideoSourceListModel *Instance();
    static VideoSourceListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);
    static void adoptInstance(VideoSourceListModel *roster);

    // Row <-> stable id. rowForStableId returns -1 when the id belongs to a
    // removed row — callers treat that as "no such route".
    static QString stableIdForRow(int row);
    static int rowForStableId(const QString &id);

    Q_INVOKABLE QString idForRow(int row);
    enum Role {
        NameRole = Qt::UserRole + 1,
        KindRole,
        SublabelRole,
        ModeRole,
        MutedRole,
        LevelRole,
        ChannelGainsRole,
    };
    Q_ENUM(Role)

    explicit VideoSourceListModel(QObject *parent = nullptr);
    ~VideoSourceListModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addSource();
    Q_INVOKABLE void addSourceWith(const QString &name, const QString &kind,
                                   const QString &sublabel, qreal level, bool muted);
    Q_INVOKABLE void removeSource(int index);
    Q_INVOKABLE int duplicateSource(int index);
    Q_INVOKABLE void renameSource(int index, const QString &name);
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setSublabel(int index, const QString &sublabel);
    Q_INVOKABLE QString pickVideoFile() const;   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread
    // Capture mode (OBS-style "1080p60"…) — the video dialogs' Resolution
    // picker; empty string = unset.
    Q_INVOKABLE void setMode(int index, const QString &mode);
    Q_INVOKABLE void setMuted(int index, bool muted);
    Q_INVOKABLE void setLevel(int index, qreal level);
    Q_INVOKABLE qreal channelGain(int index, int channel) const;
    Q_INVOKABLE void setChannelGain(int index, int channel, qreal gain);
    // The video card's L/R pill write — the same one reopen policy the source
    // and bus cards use (models/ChannelTrim.h).
    Q_INVOKABLE void toggleChannelGain(int index, int channel);
    // The gain this side should come back at (never the 0 that "closed" stores).
    Q_INVOKABLE qreal channelTrim(int index, int channel) const;
    Q_INVOKABLE void setChannelTrim(int index, int channel, qreal trim);

    // Snapshot for the Edit Video Source dialog.
    Q_INVOKABLE QVariantMap getSource(int index) const;

    static bool restoreDone();

    void pushAudioOffThread(int index);

private:
    static inline VideoSourceListModel *s_instance = nullptr;

    // False once this instance has been displaced by an adopted one (see
    // adoptInstance).
    bool m_active = true;
    void deactivate();

    // The single write of the trim list (the public faces above only decide WHEN
    // a trim is worth remembering). true when it changed anything.
    bool writeChannelTrim(int index, int channel, qreal trim);

    bool writeChannelGain(int index, int channel, qreal gain);

    // ---- Roster persistence (session.videoRoster via the settings store,
    void scheduleSave();
    QString serializeRoster() const;
    void queueRosterWrite();
    void saveRosterNow();
    int restoreRoster();
    QTimer *saveTimer_ = nullptr;

    QList<VideoSourceItem> m_sources;
    int m_nextId = 1;   // stable-id counter ("v<n>"): renumbered past the restored max at restore time
    bool m_restoreDone = false;   // the store has answered once (see restoreDone)
};