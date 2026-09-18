#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include <QVariantList>
#include <qqml.h>

// One mix bus — a named routing target that audio inputs and/or video
// sources can be sent to (Settings · Audio & Video's middle "BUSES &
// ROUTING" column). Routing is stored as indices into AudioInputListModel /
// VideoSourceListModel, the same convention as OutputItem::styleIndex into
// StyleListModel — this is a mock CRUD backend throughout the app, so plain
// row indices are consistent with what's already there.
struct BusItem
{
    QString name;
    // "audio" | "video" | "both" — which input roster(s) this bus accepts
    // routing from. The reference image only shows audio-only and
    // video-only buses; "both" is a real third option the Edit dialog
    // exposes even though nothing seeds it.
    QString type = QStringLiteral("audio");
    qreal level = 75;
    bool muted = false;
    QList<int> routedAudioInputs;
    QList<int> routedVideoSources;
};

// QML singleton backing Settings · Audio & Video's Buses & Routing column.
class BusListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        TypeRole,
        LevelRole,
        MutedRole,
        RoutedAudioInputsRole,
        RoutedVideoSourcesRole,
    };
    Q_ENUM(Role)

    explicit BusListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addBus(const QString &name, const QString &type);
    Q_INVOKABLE void removeBus(int index);
    // Insert a copy of row `index` (name suffixed " · copy", mute NOT
    // copied, routing NOT copied — a new bus starts unrouted); returns the
    // new row's index, -1 on a bad index. Same convention as
    // OutputListModel::duplicateOutput.
    Q_INVOKABLE int duplicateBus(int index);
    Q_INVOKABLE void renameBus(int index, const QString &name);
    Q_INVOKABLE void setType(int index, const QString &type);
    Q_INVOKABLE void setLevel(int index, qreal level);
    Q_INVOKABLE void setMuted(int index, bool muted);
    Q_INVOKABLE void toggleAudioRoute(int busIndex, int inputIndex);
    Q_INVOKABLE void toggleVideoRoute(int busIndex, int sourceIndex);

    // Snapshot for the Edit Bus dialog — routedAudioInputs/routedVideoSources
    // surfaced as QVariantList so QML can read them directly.
    Q_INVOKABLE QVariantMap getBus(int index) const;

private:
    QList<BusItem> m_buses;
};
