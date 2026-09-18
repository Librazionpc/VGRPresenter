#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include <qqml.h>

// One video source available for routing — a camera, a screen capture, or a
// plain media file. Settings · Audio & Video's right column ("VIDEO
// SOURCES"). Mirrors AudioInputListModel except for level: only MEDIA rows
// carry a volume (media players have one; a camera/screen feed doesn't —
// its level lives on the bus it routes into).
struct VideoSourceItem
{
    QString name;
    // "camera" | "screen" | "media" — the three source kinds (capture
    // devices, screen capture, plain media files); Add/Edit kind chips.
    QString kind = QStringLiteral("camera");
    // Display caption under the name ("PTZ · Wide", "HDMI in", "MP4 · 4K").
    QString sublabel;
    // Mute is meaningful for MEDIA rows (they carry audio); camera/screen
    // feeds have no audio of their own to mute.
    bool muted = false;
    // Media-only volume, 0-100. Ignored (and shown as no slider) for
    // camera/screen kinds.
    qreal level = 75;
};

// QML singleton backing Settings · Audio & Video's Video Sources column and
// the routing a BusListModel bus stores against it (by row index — same
// convention as AudioInputListModel / OutputItem::styleIndex).
class VideoSourceListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        KindRole,
        SublabelRole,
        MutedRole,
        LevelRole,
    };
    Q_ENUM(Role)

    explicit VideoSourceListModel(QObject *parent = nullptr);

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
    Q_INVOKABLE void setMuted(int index, bool muted);
    Q_INVOKABLE void setLevel(int index, qreal level);

    // Snapshot for the Edit Video Source dialog.
    Q_INVOKABLE QVariantMap getSource(int index) const;

private:
    QList<VideoSourceItem> m_sources;
};
