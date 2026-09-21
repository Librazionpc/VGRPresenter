#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include <qqml.h>

// One video source available for routing — a camera, a screen capture, a
// plain media file, or an NDI network feed. Settings · Audio & Video's
// right column ("VIDEO
// SOURCES"). Mirrors AudioInputListModel except for level: only rows whose
// feed carries audio (MEDIA files and NDI streams — NDI embeds audio with
// its video frames) carry a volume; camera/screen feeds are silent (their
// level lives on the bus they route into).
struct VideoSourceItem
{
    // Stable identity — survives row removals (rows after it shift; this
    // doesn't). BusListModel routes key graph edges on it, so removing a
    // row never shifts another row's routes. Assigned at add/duplicate
    // time; never persisted (the roster is session-owned) and never shown.
    QString id;
    QString name;
    // "camera" | "screen" | "media" | "ndi" — the source kinds (capture
    // devices, screen capture, plain media files, NDI network sources);
    // Add/Edit kind chips.
    QString kind = QStringLiteral("camera");
    // Display caption under the name ("PTZ · Wide", "HDMI in", "MP4 · 4K").
    QString sublabel;
    // Capture mode ("1080p60", "720p29.97", "4Kp30" — OBS-style device
    // modes). Empty = unset; the dialogs' preview pill falls back to a
    // nominal readout. A real device's mode list belongs to the engine's
    // video PAL once it exists; until then this is the user's pick from a
    // curated set.
    QString mode;
    // Mute is meaningful for MEDIA rows (they carry audio); camera/screen
    // feeds have no audio of their own to mute.
    bool muted = false;
    // Volume for rows whose feed carries audio — media files and NDI
    // streams (NDI embeds audio with its video), 0-100. Ignored (and shown
    // as no slider) for camera/screen kinds.
    qreal level = 0;   // silence by default — a new source starts muted-down
};

// QML singleton backing Settings · Audio & Video's Video Sources column and
// the routing a BusListModel bus stores against it.
//
// ROUTING KEYS ON STABLE IDS, NOT ROWS: BusListModel's graph edges are cut
// to "vsrc:<id>" nodes (one per row, created on demand). Row indices exist
// only at the QML boundary — toggleVideoRoute takes a row, the models
// translate to ids internally, so deleting a row can never shift anyone
// else's routes. Static accessors let the models translate across each
// other (all QML-singleton instances; created before any routing call runs).
class VideoSourceListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // C++ access to the QML-created singleton (null until QML constructs
    // it) — the cross-model translation entry points below are only ever
    // called from routing flows, which run after the screen exists.
    static VideoSourceListModel *Instance() { return s_instance; }

    // Row <-> stable id. rowForStableId returns -1 when the id belongs to a
    // removed row — callers treat that as "no such route".
    static QString stableIdForRow(int row);
    static int rowForStableId(const QString &id);
    enum Role {
        NameRole = Qt::UserRole + 1,
        KindRole,
        SublabelRole,
        ModeRole,
        MutedRole,
        LevelRole,
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
    // Insert a copy of row `index` (name suffixed " · copy", mute NOT
    // copied); returns the new row's index, -1 on a bad index. Same
    // convention as OutputListModel::duplicateOutput.
    Q_INVOKABLE int duplicateSource(int index);
    Q_INVOKABLE void renameSource(int index, const QString &name);
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setSublabel(int index, const QString &sublabel);
    // Capture mode (OBS-style "1080p60"…) — the video dialogs' Resolution
    // picker; empty string = unset.
    Q_INVOKABLE void setMode(int index, const QString &mode);
    Q_INVOKABLE void setMuted(int index, bool muted);
    Q_INVOKABLE void setLevel(int index, qreal level);

    // Snapshot for the Edit Video Source dialog.
    Q_INVOKABLE QVariantMap getSource(int index) const;

private:
    static inline VideoSourceListModel *s_instance = nullptr;

    QList<VideoSourceItem> m_sources;
    int m_nextId = 1;   // stable-id counter ("v<n>"): unique for the whole session; restarts each launch (rosters aren't persisted)
};
