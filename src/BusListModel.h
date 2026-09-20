#pragma once

#include <QAbstractListModel>
#include <QList>
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
// Routing is stored as graph EDGES from roster source nodes ("ain:<row>"
// for AudioInputListModel rows, "vin:<row>" for VideoSourceListModel rows)
// into the bus's plane node — created on demand at first route. Same
// index-into-roster convention as before (and its same staleness caveat:
// rows shift, edges don't follow).
struct BusItem
{
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
    // Engine-backed plumbing: create the graph nodes + cache row (returns
    // the new row); pull one row's name/level/muted/routes back from the
    // graph. The model starts EMPTY — buses exist only once created (user
    // + Add, or a future engine-side creator); nothing is seeded app-side.
    int createBusRow(const QString &name, const QString &type,
                     qreal level, bool muted);
    void pullRow(int row);

    QList<BusItem> m_buses;
    int m_nextBus = 1;   // engine node id counter ("bus:<n>#a" / "bus:<n>#v")
};
