#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVariantList>
#include <optional>
#include <qqml.h>

#include "modules/production/ProductionTypes.hpp"

class AudioInputListModel;
class VideoSourceListModel;

struct BusItem
{
    // Stable identity = the engine node number ("bus:<id>#a"/"bus:<id>#v")
    // — survives row removals; the routing matrix stores THESE, not rows.
    int id = 0;
    QString name;
    QString type = QStringLiteral("audio");
    QString templateId;
    bool onBoard = false;
    qreal level = 75;  // practical headroom by default, mapped to gainDb (-60..0 dB)
    bool muted = false;
    // PER-SIDE trim of the bus's own output, index 0 = L, 1 = R (1.0 = open,
    // 0.0 = that side closed). The same convention an audio INPUT row's
    QList<qreal> channelGains;
    QList<qreal> channelTrims;
    QList<int> routedAudioInputs;
    QList<int> routedVideoSources;
    // Engine graph node ids: the Audio plane and the Video plane of this
    // bus. Always both set.
    QString engineId;
    QString engineIdVideo;
};

// QML singleton backing Settings · Audio & Video's Buses & Routing column.
class BusListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static BusListModel *Instance();
    static BusListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);
    void adoptRoutingModels(AudioInputListModel *audioRoster,
                            VideoSourceListModel *videoRoster);

    // Bus row <-> stable bus number (BusItem::id). rowForBusNumber returns
    // -1 when the bus was removed — callers treat that as "no such route".
    static int busNumberAt(int row);
    static int rowForBusNumber(int busNumber);

    enum Role {
        NameRole = Qt::UserRole + 1,
        TypeRole,
        LevelRole,
        MutedRole,
        ChannelGainsRole,
        RoutedAudioInputsRole,
        RoutedVideoSourcesRole,
        // Whether this bus has a card on the Routing board (see BusItem::onBoard).
        OnBoardRole,
    };
    Q_ENUM(Role)

    explicit BusListModel(QObject *parent = nullptr);
    ~BusListModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addBus(const QString &name, const QString &type);
    Q_INVOKABLE int addBusOnBoard(const QString &name, const QString &type);
    // Add / take this bus off the Routing board. Never touches the bus itself.
    Q_INVOKABLE void setBusOnBoard(int index, bool onBoard);
    Q_INVOKABLE int addTemplateBus(const QString &name, const QString &templateId);
    Q_INVOKABLE bool usesTemplate(const QString &templateId) const;
    Q_INVOKABLE void removeBus(int index);
    Q_INVOKABLE bool deleteBus(int index);
    Q_INVOKABLE bool restoreRemovedBus();
    // The name the last delete removed ("" when there is none).
    Q_INVOKABLE QString removedBusName() const;
    Q_INVOKABLE int duplicateBus(int index);
    Q_INVOKABLE void renameBus(int index, const QString &name);
    Q_INVOKABLE void setType(int index, const QString &type);
    // A TEMPLATE bus's design: re-point it at another Buses-category template. Ignored for any
    // other kind of bus and for an empty id (a template bus with no template composes nothing).
    Q_INVOKABLE void setTemplateId(int index, const QString &templateId);
    Q_INVOKABLE void setLevel(int index, qreal level);
    Q_INVOKABLE void setMuted(int index, bool muted);
    Q_INVOKABLE qreal channelGain(int index, int channel) const;
    Q_INVOKABLE void setChannelGain(int index, int channel, qreal gain);
    Q_INVOKABLE void toggleChannelGain(int index, int channel);
    // The gain this side should come back at (never the 0 that "closed" stores).
    Q_INVOKABLE qreal channelTrim(int index, int channel) const;
    Q_INVOKABLE void setChannelTrim(int index, int channel, qreal trim);
    Q_INVOKABLE void toggleAudioRoute(int busIndex, int inputIndex);
    Q_INVOKABLE void toggleVideoRoute(int busIndex, int sourceIndex);
    Q_INVOKABLE bool disconnectAudioRoute(int busIndex, int inputIndex);
    Q_INVOKABLE bool disconnectVideoRoute(int busIndex, int sourceIndex);
    Q_INVOKABLE bool connectAudioRoute(int busIndex, int inputIndex);
    Q_INVOKABLE bool connectVideoRoute(int busIndex, int sourceIndex);
    Q_INVOKABLE bool canConnect(const QString &kind, int sourceRow, int busIndex) const;
    Q_INVOKABLE QString connectRefusal(const QString &kind, int sourceRow, int busIndex) const;

    // Snapshot for the Edit Bus dialog — routedAudioInputs/routedVideoSources
    // surfaced as QVariantList so QML can read them directly.
    Q_INVOKABLE QVariantMap getBus(int index) const;

private:
    static inline BusListModel *s_instance = nullptr;

    // The single write of the trim list (the public faces above only decide WHEN
    // a trim is worth remembering). true when it changed anything.
    bool writeChannelTrim(int index, int channel, qreal trim);

    struct RemovedBus {
        int row = -1;
        BusItem item;
        QStringList audioSources;
        QStringList videoSources;
    };
    // One deep, like every undo an operator takes immediately: a second delete
    // replaces the offer, because the notice on screen is about that second bus.
    std::optional<RemovedBus> m_removedBus;

    int createBusRow(const QString &name, const QString &type,
                     qreal level, bool muted, const QString &templateId = QString(),
                     bool onBoard = false);
    void pullRow(int row);
    void rebuildFromGraph();
    void logBoardState() const;

public:
    void refreshRoutes();
    void refreshRoutesIfStale();
    Q_INVOKABLE void refresh();

    void cutAudioSourceEdges(const QString &stableId);
    void cutVideoSourceEdges(const QString &stableId);

    Q_INVOKABLE void pruneOrphanSourceEdges();
    Q_INVOKABLE void trimExtraVideoProviders();
    static QString audioSourceNodeId(int rosterRow);
    static QString videoSourceNodeId(int rosterRow);

private:
    bool m_active = true;
    void deactivate();
    void watchRosters();
    QList<QMetaObject::Connection> m_rosterConnections;

    QList<BusItem> m_buses;
    int m_nextBus = 1;   // stable bus number / engine node id counter
};