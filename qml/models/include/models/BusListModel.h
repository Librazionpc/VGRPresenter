#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <qqml.h>

#include "modules/production/ProductionTypes.hpp"

// One mix bus — a named routing target that audio inputs and/or video
// sources can be sent to (Settings · Audio & Video's middle "BUSES &
// ROUTING" column). THE ENGINE OWNS THE BUSES: every row is a live view of
// bus nodes in the kernel's production graph (docs/specs/27 —
// ProductionEngine/ProductionGraph), created through CreateBus-level graph
// primitives; this model only keeps the row ORDER and mirrors node state.
// A bus is a PAIR of graph nodes — one Audio plane + one Video plane — the
// same video-bus+audio-bus pairing the engine gives every output; the UI's
// `type` ("audio" | "video" | "both") is the routing POLICY on top (which
// rosters may route in), not the nodes' existence.
//
//   - Routing is stored as graph EDGES from stable per-source nodes
//     ("asrc:<AudioInputItem::id>" for audio rows, "vsrc:<VideoSourceItem::id>"
//     for video rows) into the bus's plane node — created on demand at first
//     route. Edges key on the source's STABLE ID, not its roster row, so
//     deleting a roster row can never shift another row's routes (the
//     removed row's own edges are cut by its model on removal).
struct BusItem
{
    // Stable identity = the engine node number ("bus:<id>#a"/"bus:<id>#v")
    // — survives row removals; the routing matrix stores THESE, not rows.
    int id = 0;
    QString name;
    // "audio" | "video" | "both" — which input roster(s) this bus ACCEPTS
    // routing from. The reference image only shows audio-only and
    // video-only buses; "both" is a real third option the Edit dialog
    // exposes. Policy only — both plane nodes always exist in the graph.
    QString type = QStringLiteral("audio");
    qreal level = 0;   // mapped to the node's gainDb (-60..0 dB linear)
    bool muted = false;
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
    // C++ access to the QML-created singleton (null until QML constructs
    // it) — the cross-model translation entry points below are only ever
    // called from routing flows, which run after the screen exists.
    static BusListModel *Instance() { return s_instance; }

    // Bus row <-> stable bus number (BusItem::id). rowForBusNumber returns
    // -1 when the bus was removed — callers treat that as "no such route".
    static int busNumberAt(int row);
    static int rowForBusNumber(int busNumber);

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
    ~BusListModel() override;

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
    static inline BusListModel *s_instance = nullptr;

    // Engine-backed plumbing: create the graph nodes + cache row (returns
    // the new row); pull one row's name/level/muted/routes back from the
    // graph. The model starts EMPTY app-side; the constructor then rebuilds
    // rows from whatever the engine's graph holds (restored from the
    // kernel's persisted production document) — the graph is the only
    // truth, across restarts too.
    int createBusRow(const QString &name, const QString &type,
                     qreal level, bool muted);
    void pullRow(int row);
    void rebuildFromGraph();
    void pruneOrphanSourceNodes();

public:
    // Re-read every row's routes from the graph. Routes are cached as roster
    // ROWS, so a roster removal (which shifts rows) must call this or every
    // bus keeps pointing at the old row numbers.
    void refreshRoutes();
    // Full re-sync from the graph (rows added/removed behind this model's
    // back — engine facade, restore). Also fired by production.restored /
    // production.bus_changed engine events.
    Q_INVOKABLE void refresh();

    // Edge cleanup invoked by the roster models when a source row is
    // REMOVED: cuts every graph edge from that source's stable node(s)
    // (asrc:<id> / vsrc:<id>) and refreshes the affected rows. No-ops when
    // the source was never routed.
    void cutAudioSourceEdges(const QString &stableId);
    void cutVideoSourceEdges(const QString &stableId);

private:
    // Stable-id graph node id for a roster row; empty on a bad row. Video
    // additionally gets the node pulled out of any other bus by the 1:1
    // rule at connect time.
    static QString audioSourceNodeId(int rosterRow);
    static QString videoSourceNodeId(int rosterRow);

    QList<BusItem> m_buses;
    int m_nextBus = 1;   // stable bus number / engine node id counter
};
