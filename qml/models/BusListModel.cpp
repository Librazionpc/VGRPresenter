#include "BusListModel.h"

#include "AudioInputListModel.h"
#include "services/EngineBridge.h"
#include "VideoSourceListModel.h"

#include <QDebug>
#include <QSet>
#include <cstdio>
#include <utility>

#include "modules/production/ProductionEngine.hpp"

#include <QJSEngine>

#include <algorithm>
#include <cstring>
#include <set>

// The one instance, for C++ AND QML — see the header for why this is eager.
BusListModel *BusListModel::Instance()
{
    static BusListModel s;
    return &s;
}

BusListModel *BusListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    BusListModel *s = Instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

// The engine owns the buses. Every row here is a live view of bus nodes in
// the kernel's production graph (docs/specs/27 — ProductionEngine owns the
// graph; this model is a row-ordered relay for QML, per the app's standard
// "kernel decides, bridge/model relays, UI renders" layering):
//
//   - A bus is a PAIR of graph nodes — an Audio plane and a Video plane —
//     the same video-bus+audio-bus pairing the engine already gives every
//     output. The UI's `type` ("audio" | "video" | "both") is routing
//     POLICY on top (which rosters the dialog offers), not the nodes'
//     existence.
//   - Level 0-100 maps linearly onto the plane nodes' gainDb -60..0 dB;
//     mute is the node's VolumeControl::mute.
//   - A route is a graph EDGE from a stable per-source node ("asrc:<id>"/
//     "vsrc:<id>" — the source row's STABLE id, not its roster row) into
//     the matching plane node. Source nodes are created on demand at first
//     route; deleting a roster row cuts only its own edges (the roster
//     model calls back), so no other row's routes ever shift. Audio routes
//     are additive (mixing); video routes are strictly 1:1 (a bus renders
//     exactly one frame source) — enforced here, exactly as before.
//
// The model starts EMPTY: no buses exist until the user adds one (+ Add)
// or the engine creates them. Nothing is seeded app-side — the graph is the
// only truth, and a preset roster the user didn't build is just hardcoded
// state wearing an engine costume.
namespace {

bps::production::ProductionGraph &graph()
{
    return bps::production::ProductionEngine::Instance().Graph();
}

// UI 0-100  <->  engine gainDb -60..0 dB, linear. 0 reads as silence,
// 100 as unity — matching the board's "buses open silent, user raises".
constexpr double kSilentDb = -60.0;

// Stable per-source graph node ids: a source ROUTE is an edge from a node
// named after the source row's stable id (never its roster row), so routes
// survive every roster reorder/removal.
constexpr const char *kAudioSourcePrefix = "asrc:";
constexpr const char *kVideoSourcePrefix = "vsrc:";

qreal levelFromDb(double db)
{
    return std::clamp((db - kSilentDb) / (0.0 - kSilentDb) * 100.0, 0.0, 100.0);
}

double dbFromLevel(qreal level)
{
    return kSilentDb + (level / 100.0) * (0.0 - kSilentDb);
}

QVariantList toVariantList(const QList<int> &indices)
{
    QVariantList list;
    for (int i : indices)
        list.append(i);
    return list;
}

// Routes of one plane: the stable "asrc:<id>"/"vsrc:<id>" source nodes
// feeding it. Reported as ROSTER ROWS (the QML contract): each upstream id
// is translated through the owning roster model — a source whose row was
// removed drops out (its edges get cut on removal anyway; this is the
// belt to those braces). Ordering is irrelevant to consumers (membership
// tests), but keep upstream order for stable output.
QList<int> audioRoutesFromGraph(const std::string &planeNodeId)
{
    QList<int> out;
    constexpr const char *needle = "asrc:";
    for (const auto &up : graph().Upstream(planeNodeId)) {
        if (up.starts_with(needle)) {
            const int row = AudioInputListModel::rowForStableId(
                QString::fromStdString(up.substr(std::strlen(needle))));
            if (row >= 0 && !out.contains(row))
                out.append(row);
        }
    }
    return out;
}

QList<int> videoRoutesFromGraph(const std::string &planeNodeId)
{
    QList<int> out;
    constexpr const char *needle = "vsrc:";
    for (const auto &up : graph().Upstream(planeNodeId)) {
        if (up.starts_with(needle)) {
            const int row = VideoSourceListModel::rowForStableId(
                QString::fromStdString(up.substr(std::strlen(needle))));
            if (row >= 0 && !out.contains(row))
                out.append(row);
        }
    }
    return out;
}

bool feedsFrom(const std::string &planeNodeId, const std::string &sourceId)
{
    const auto ups = graph().Upstream(planeNodeId);
    return std::find(ups.begin(), ups.end(), sourceId) != ups.end();
}

void logIfFailed(const char *op, const auto &r)
{
    if (!r.ok())
        qWarning() << "BusListModel:" << op << "failed:" << r.error().message.c_str();
}

// One atomic edit window around a multi-step graph mutation. Every graph
// mutator serializes the WHOLE graph and flushes the database synchronously
// (QueuePersist) — on the GUI thread, once per call — so a bus creation
// (6 mutators) or a video re-route (N+1) would otherwise flush that many
// times back to back. Inside an edit window the graph saves once, at commit.
// A no-op when a window is already open (nesting), so helpers compose.
class GraphEdit
{
public:
    GraphEdit() : owns_(graph().BeginEdit().ok()) {}
    ~GraphEdit()
    {
        if (owns_)
            logIfFailed("CommitEdit", graph().CommitEdit());
    }
    GraphEdit(const GraphEdit &) = delete;
    GraphEdit &operator=(const GraphEdit &) = delete;

private:
    bool owns_;
};

} // namespace

BusListModel::BusListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    Q_ASSERT_X(!s_instance || s_instance == this, "BusListModel",
               "a second BusListModel was constructed — Instance() would "
               "silently rebind and every C++ reader would hold the wrong one");
    s_instance = this;
    pruneOrphanSourceNodes();
    rebuildFromGraph();

    // Engine-side changes made without this model (production.restored, or a
    // bus edited through the engine facade / a future IPC client) re-sync the
    // rows. The model is otherwise a cached view refreshed only after its own
    // mutations. Queued: the relay already marshals to the GUI thread, this
    // just keeps a reset from happening inside a delegate's own event.
    connect(&EngineBridge::instance(), &EngineBridge::engineEvent, this,
            [this](const QString &topic, const QVariantMap &) {
                if (topic == QLatin1String("production.restored")
                    || topic == QLatin1String("production.bus_changed"))
                    refresh();
            }, Qt::QueuedConnection);
}

BusListModel::~BusListModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

int BusListModel::busNumberAt(int row)
{
    if (!s_instance || row < 0 || row >= s_instance->m_buses.size())
        return -1;
    return s_instance->m_buses.at(row).id;
}

int BusListModel::rowForBusNumber(int busNumber)
{
    if (!s_instance)
        return -1;
    for (int i = 0; i < s_instance->m_buses.size(); ++i)
        if (s_instance->m_buses.at(i).id == busNumber)
            return i;
    return -1;   // removed bus — callers treat as "no such route"
}

// Stable graph node id for a roster row — derived from the row's stable id
// ("asrc:a3" / "vsrc:v7"), so it survives every row shift. Empty on a bad
// or not-yet-constructed roster (routing runs only after the screen exists).
QString BusListModel::audioSourceNodeId(int rosterRow)
{
    const QString id = AudioInputListModel::stableIdForRow(rosterRow);
    return id.isEmpty() ? QString()
                        : QString::fromLatin1(kAudioSourcePrefix) + id;
}

QString BusListModel::videoSourceNodeId(int rosterRow)
{
    const QString id = VideoSourceListModel::stableIdForRow(rosterRow);
    return id.isEmpty() ? QString()
                        : QString::fromLatin1(kVideoSourcePrefix) + id;
}

void BusListModel::cutAudioSourceEdges(const QString &stableId)
{
    if (stableId.isEmpty())
        return;
    const std::string node = std::string(kAudioSourcePrefix) + stableId.toStdString();
    if (!graph().HasNode(node))
        return;   // never routed — nothing to cut

    GraphEdit edit;
    QList<int> touched;
    for (int row = 0; row < m_buses.size(); ++row) {
        if (graph().Disconnect(node, m_buses.at(row).engineId.toStdString()).ok())
            touched.append(row);
    }
    // The row is gone for good — its source node goes with it (else every
    // removed source leaves an orphan node in the persisted graph).
    logIfFailed("RemoveNode(asrc)", graph().RemoveNode(node));
    for (const int row : std::as_const(touched)) {
        pullRow(row);
        const QModelIndex changed = index(row);
        emit dataChanged(changed, changed, { RoutedAudioInputsRole });
    }
}

void BusListModel::cutVideoSourceEdges(const QString &stableId)
{
    if (stableId.isEmpty())
        return;
    const std::string node = std::string(kVideoSourcePrefix) + stableId.toStdString();
    if (!graph().HasNode(node))
        return;   // never routed — nothing to cut

    GraphEdit edit;
    QList<int> touched;
    for (int row = 0; row < m_buses.size(); ++row) {
        if (graph().Disconnect(node, m_buses.at(row).engineIdVideo.toStdString()).ok())
            touched.append(row);
    }
    logIfFailed("RemoveNode(vsrc)", graph().RemoveNode(node));
    for (const int row : std::as_const(touched)) {
        pullRow(row);
        const QModelIndex changed = index(row);
        emit dataChanged(changed, changed, { RoutedVideoSourcesRole });
    }
}

// The graph (already restored from the kernel's persisted production doc by
// the time QML constructs this singleton) is rebuilt into rows here. Row
// order follows the paired-plane node ids' numeric suffix so a restored
// board looks exactly like the one the user left. Only planes whose twin
// still exists form a row — a crashed remove can't strand a half-bus.
void BusListModel::rebuildFromGraph()
{
    std::set<int> seen;
    const auto collect = [&](const char *planeSuffix) {
        for (const auto &id : graph().NodeIds()) {
            const std::string s = id;
            if (!s.starts_with("bus:") || !s.ends_with(planeSuffix))
                continue;
            const size_t numStart = 4;   // after "bus:"
            const size_t numEnd = s.find('#');
            if (numEnd == std::string::npos || numEnd <= numStart)
                continue;
            int n = 0;
            try {
                n = std::stoi(s.substr(numStart, numEnd - numStart));
            } catch (const std::exception &) {
                continue;   // not one of ours
            }
            seen.insert(n);
        }
    };
    collect("#a");
    collect("#v");

    for (int n : seen) {
        const std::string a = "bus:" + std::to_string(n) + "#a";
        const std::string v = "bus:" + std::to_string(n) + "#v";
        // Count EVERY number seen, half-buses included: the next AddBus must
        // never reuse an id that still names a stray plane node.
        m_nextBus = std::max(m_nextBus, n + 1);
        if (!graph().HasNode(a) || !graph().HasNode(v)) {
            // Half-bus from an interrupted remove — not a row; clean the
            // stray plane up so it can't collide or linger in the document.
            GraphEdit edit;
            for (const std::string &stray : {a, v})
                if (graph().HasNode(stray))
                    logIfFailed("RemoveNode(stray)", graph().RemoveNode(stray));
            continue;
        }

        BusItem item;
        item.id = n;   // stable identity = the engine node number
        item.engineId = QString::fromStdString(a);
        item.engineIdVideo = QString::fromStdString(v);

        // The UI's routing-policy type lives engine-side as node meta
        // (survives restarts with the rest of the graph).
        if (auto r = graph().GetNode(a); r.ok())
            item.type = QString::fromStdString(
                r.value().meta.count("uiBusType") ? r.value().meta.at("uiBusType")
                                                  : "audio");
        m_buses.append(item);
    }

    // Second pass: pull live values (level/mute/routes) per row.
    for (int row = 0; row < m_buses.size(); ++row)
        pullRow(row);
}

// Roster rows are not persisted (they restart empty each launch, with fresh
// stable ids a1/v1...), but the graph is — so any "asrc:"/"vsrc:" node found at
// startup is a leftover from a PREVIOUS session. Left alone, a new source
// "a1" would inherit last session's routes for the old "a1" (its first toggle
// would DISconnect) and the orphans would pile up forever. Drop them all.
// (When rosters are persisted with their ids, this must go — then only nodes
// whose id is absent from the restored roster are orphans.)
void BusListModel::pruneOrphanSourceNodes()
{
    std::vector<std::string> stale;
    for (const auto &id : graph().NodeIds()) {
        const std::string s = id;
        if (s.starts_with(kAudioSourcePrefix) || s.starts_with(kVideoSourcePrefix))
            stale.push_back(s);
    }
    if (stale.empty())
        return;
    GraphEdit edit;
    for (const std::string &id : stale)
        logIfFailed("RemoveNode(orphan source)", graph().RemoveNode(id));
}

// Re-reads every row's routes (and level/mute/name) from the graph. Routes
// are cached as roster ROWS, so ANY roster row shift (a removal) leaves every
// bus's cached lists stale — the roster models call this after a removal.
void BusListModel::refreshRoutes()
{
    if (m_buses.isEmpty())
        return;
    for (int row = 0; row < m_buses.size(); ++row)
        pullRow(row);
    emit dataChanged(index(0), index(int(m_buses.size()) - 1),
                     { RoutedAudioInputsRole, RoutedVideoSourcesRole });
}

// Full re-sync from the graph — rows added/removed/renamed behind our back.
void BusListModel::refresh()
{
    beginResetModel();
    m_buses.clear();
    rebuildFromGraph();
    endResetModel();
}

int BusListModel::createBusRow(const QString &name, const QString &type,
                               qreal level, bool muted)
{
    BusItem item;
    item.name = name;
    item.type = type;
    item.level = level;
    item.muted = muted;

    GraphEdit edit;   // 6 graph mutators, one save

    const int busNo = m_nextBus++;
    const std::string base = "bus:" + std::to_string(busNo);
    item.id = busNo;   // stable identity — the routing matrix stores this
    item.engineId = QString::fromStdString(base + "#a");
    item.engineIdVideo = QString::fromStdString(base + "#v");

    // User-created buses are BusRole::Custom (the engine preserves roles in
    // bus scenes/snapshots).
    logIfFailed("AddBus(audio)", graph().AddBus(item.engineId.toStdString(),
                                                name.toStdString(),
                                                bps::production::BusRole::Custom,
                                                bps::production::SignalType::Audio));
    logIfFailed("AddBus(video)", graph().AddBus(item.engineIdVideo.toStdString(),
                                                name.toStdString(),
                                                bps::production::BusRole::Custom,
                                                bps::production::SignalType::Video));
    // The routing-policy type rides engine-side as node meta so it comes
    // back with the graph on the next boot. (After AddBus — SetMeta needs
    // the node to exist.)
    logIfFailed("SetMeta(audio)",
                graph().SetMeta(item.engineId.toStdString(), "uiBusType", type.toStdString()));
    logIfFailed("SetMeta(video)",
                graph().SetMeta(item.engineIdVideo.toStdString(), "uiBusType", type.toStdString()));
    // Buses open silent (level 0 = -60 dB) unless the caller says otherwise.
    const bps::production::VolumeControl v{ dbFromLevel(level), muted, false, 0.0, 0.0 };
    logIfFailed("SetVolume(audio)", graph().SetVolume(item.engineId.toStdString(), v));
    logIfFailed("SetVolume(video)", graph().SetVolume(item.engineIdVideo.toStdString(), v));

    m_buses.append(item);
    return m_buses.size() - 1;
}

void BusListModel::pullRow(int row)
{
    if (row < 0 || row >= m_buses.size())
        return;
    BusItem &item = m_buses[row];
    if (auto r = graph().Volume(item.engineId.toStdString()); r.ok()) {
        item.level = levelFromDb(r.value().gainDb);
        item.muted = r.value().mute;
    }
    if (auto r = graph().GetNode(item.engineId.toStdString()); r.ok())
        item.name = QString::fromStdString(r.value().displayName);
    // `type` stays cached: it's this model's routing policy, mirrored onto
    // both plane nodes (which always exist).
    item.routedAudioInputs = audioRoutesFromGraph(item.engineId.toStdString());
    item.routedVideoSources = videoRoutesFromGraph(item.engineIdVideo.toStdString());
}

int BusListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_buses.size();
}

QVariant BusListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_buses.size())
        return {};

    const BusItem &item = m_buses.at(index.row());
    switch (role) {
    case NameRole: return item.name;
    case TypeRole: return item.type;
    case LevelRole: return item.level;
    case MutedRole: return item.muted;
    case RoutedAudioInputsRole: return toVariantList(item.routedAudioInputs);
    case RoutedVideoSourcesRole: return toVariantList(item.routedVideoSources);
    default: return {};
    }
}

QHash<int, QByteArray> BusListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { TypeRole, "type" },
        { LevelRole, "level" },
        { MutedRole, "muted" },
        { RoutedAudioInputsRole, "routedAudioInputs" },
        { RoutedVideoSourcesRole, "routedVideoSources" },
    };
}

void BusListModel::addBus(const QString &name, const QString &type)
{
    const QString trimmed = name.trimmed();
    const int row = m_buses.size();

    beginInsertRows(QModelIndex(), row, row);
    createBusRow(trimmed.isEmpty() ? QStringLiteral("New Bus %1").arg(row + 1) : trimmed,
                 type.isEmpty() ? QStringLiteral("audio") : type, 0, false);
    endInsertRows();
}

void BusListModel::removeBus(int index)
{
    if (index < 0 || index >= m_buses.size())
        return;
    const BusItem item = m_buses.at(index);

    // Engine first: node removal takes its edges with it; the view row goes
    // only when the graph no longer holds the bus.
    {
        GraphEdit edit;
        logIfFailed("RemoveNode(audio)", graph().RemoveNode(item.engineId.toStdString()));
        logIfFailed("RemoveNode(video)", graph().RemoveNode(item.engineIdVideo.toStdString()));
    }
    if (graph().HasNode(item.engineId.toStdString())
        || graph().HasNode(item.engineIdVideo.toStdString())) {
        qWarning() << "BusListModel: removeBus kept the row — the graph still holds it";
        return;
    }

    beginRemoveRows(QModelIndex(), index, index);
    m_buses.removeAt(index);
    endRemoveRows();
}

int BusListModel::duplicateBus(int index)
{
    if (index < 0 || index >= m_buses.size())
        return -1;

    const BusItem &src = m_buses.at(index);
    beginInsertRows(QModelIndex(), m_buses.size(), m_buses.size());
    const int row = createBusRow(QStringLiteral("%1 · copy").arg(src.name), src.type,
                                 src.level, false);
    endInsertRows();
    return row;
}

void BusListModel::renameBus(int index, const QString &name)
{
    if (index < 0 || index >= m_buses.size())
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_buses[index].name == trimmed)
        return;

    // Rename flows through the graph (one truth for every view of the node).
    GraphEdit edit;
    logIfFailed("RenameNode(audio)", graph().RenameNode(m_buses[index].engineId.toStdString(),
                                                        trimmed.toStdString()));
    logIfFailed("RenameNode(video)", graph().RenameNode(m_buses[index].engineIdVideo.toStdString(),
                                                        trimmed.toStdString()));
    m_buses[index].name = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole });
}

void BusListModel::setType(int index, const QString &type)
{
    if (index < 0 || index >= m_buses.size())
        return;
    if (m_buses[index].type == type)
        return;

    GraphEdit edit;
    m_buses[index].type = type;
    // Persist the policy engine-side (meta), then drop routes the narrowed
    // type no longer accepts: a route to a roster the bus no longer accepts
    // from is dead weight. Audio sources fit EVERY bus (a video bus carries
    // the feed with its embedded audio — matches QML's sourceFitsBus, the
    // one compat rule), so only video routes are ever dropped, when the
    // type narrows to pure audio.
    logIfFailed("SetMeta(audio)",
                graph().SetMeta(m_buses[index].engineId.toStdString(), "uiBusType",
                                type.toStdString()));
    logIfFailed("SetMeta(video)",
                graph().SetMeta(m_buses[index].engineIdVideo.toStdString(), "uiBusType",
                                type.toStdString()));
    QList<int> roles = { TypeRole };
    if (type != QStringLiteral("video") && type != QStringLiteral("both")
        && !m_buses[index].routedVideoSources.isEmpty()) {
        // Drop routes the narrowed type no longer accepts. routedVideoSources
        // holds ROSTER ROWS — re-derive their stable nodes to disconnect.
        for (int row : std::as_const(m_buses[index].routedVideoSources)) {
            const QString node = videoSourceNodeId(row);
            if (!node.isEmpty())
                logIfFailed("Disconnect(policy)",
                            graph().Disconnect(node.toStdString(),
                                               m_buses[index].engineIdVideo.toStdString()));
        }
        roles.append(RoutedVideoSourcesRole);
    }

    pullRow(index);
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, roles);
}

void BusListModel::setLevel(int index, qreal level)
{
    if (index < 0 || index >= m_buses.size())
        return;
    const qreal clamped = std::clamp(level, 0.0, 100.0);
    if (qFuzzyCompare(m_buses[index].level, clamped))
        return;

    GraphEdit edit;
    // Write gainDb on BOTH plane nodes, preserving each node's mute/solo.
    for (const QString &id : { m_buses[index].engineId, m_buses[index].engineIdVideo }) {
        bps::production::VolumeControl v{};
        if (auto r = graph().Volume(id.toStdString()); r.ok())
            v = r.value();
        v.gainDb = dbFromLevel(clamped);
        logIfFailed("SetVolume", graph().SetVolume(id.toStdString(), v));
    }

    m_buses[index].level = clamped;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { LevelRole });
}

void BusListModel::setMuted(int index, bool muted)
{
    if (index < 0 || index >= m_buses.size())
        return;
    if (m_buses[index].muted == muted)
        return;

    GraphEdit edit;
    for (const QString &id : { m_buses[index].engineId, m_buses[index].engineIdVideo }) {
        bps::production::VolumeControl v{};
        if (auto r = graph().Volume(id.toStdString()); r.ok())
            v = r.value();
        v.mute = muted;
        logIfFailed("SetVolume(mute)", graph().SetVolume(id.toStdString(), v));
    }

    m_buses[index].muted = muted;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { MutedRole });
}

void BusListModel::toggleAudioRoute(int busIndex, int inputIndex)
{
    if (busIndex < 0 || busIndex >= m_buses.size())
        return;
    // QML passes a roster ROW; the edge keys on the row's STABLE id node —
    // a bad/vanished row has no node and nothing to route.
    const QString srcQ = audioSourceNodeId(inputIndex);
    if (srcQ.isEmpty())
        return;
    BusItem &item = m_buses[busIndex];
    const std::string src = srcQ.toStdString();
    const std::string plane = item.engineId.toStdString();
    GraphEdit edit;   // (AddSource + Connect), one save

    if (feedsFrom(plane, src)) {
        logIfFailed("Disconnect(audio)", graph().Disconnect(src, plane));
    } else {
        if (!graph().HasNode(src)) {
            logIfFailed("AddSource(audio)",
                        graph().AddSource(src, "Audio Source",
                                          bps::production::SignalType::Audio));
        }
        logIfFailed("Connect(audio)", graph().Connect(src, plane,
                                                      bps::production::SignalType::Audio));
    }

    pullRow(busIndex);
    const QModelIndex changed = this->index(busIndex);
    emit dataChanged(changed, changed, { RoutedAudioInputsRole });
}

void BusListModel::toggleVideoRoute(int busIndex, int sourceIndex)
{
    if (busIndex < 0 || busIndex >= m_buses.size())
        return;

    // VIDEO IS STRICTLY 1:1 (the one contract, with QML's dropAllowed):
    // a bus renders exactly ONE source (one output frame) and a source
    // renders on exactly ONE bus. Enforced at the GRAPH level: the new
    // source is disconnected from every bus's video plane, then — unless
    // the toggle is a remove — connected into this bus's video plane.
    // QML passes a roster ROW; the edge keys on the row's STABLE id node —
    // a bad/vanished row has no node and nothing to route.
    const QString srcQ = videoSourceNodeId(sourceIndex);
    if (srcQ.isEmpty())
        return;
    const std::string src = srcQ.toStdString();
    GraphEdit edit;   // N disconnects + one connect, one save

    QSet<int> touchedRows;
    bool wasOnThisBus = false;
    for (int b = 0; b < m_buses.size(); ++b) {
        if (graph().Disconnect(src, m_buses[b].engineIdVideo.toStdString()).ok()) {
            touchedRows.insert(b);
            if (b == busIndex)
                wasOnThisBus = true;
        }
    }

    if (!wasOnThisBus) {
        if (!graph().HasNode(src)) {
            logIfFailed("AddSource(video)",
                        graph().AddSource(src, "Video Source",
                                          bps::production::SignalType::Video));
        }
        if (graph().Connect(src, m_buses[busIndex].engineIdVideo.toStdString(),
                            bps::production::SignalType::Video).ok())
            touchedRows.insert(busIndex);
        else
            qWarning() << "BusListModel: Connect(video) failed — refusing";
    }

    for (const int b : std::as_const(touchedRows)) {
        pullRow(b);
        const QModelIndex changed = this->index(b);
        emit dataChanged(changed, changed, { RoutedVideoSourcesRole });
    }
}

bool BusListModel::canConnect(const QString &kind, int sourceRow, int busIndex) const
{
    if (busIndex < 0 || busIndex >= m_buses.size() || sourceRow < 0)
        return false;
    const bool audio = (kind == QStringLiteral("audio"));
    const QString srcQ = audio ? audioSourceNodeId(sourceRow) : videoSourceNodeId(sourceRow);
    if (srcQ.isEmpty())
        return false;   // a bad/vanished roster row has no node to route
    const BusItem &bus = m_buses.at(busIndex);
    const std::string plane = (audio ? bus.engineId : bus.engineIdVideo).toStdString();
    const bool ok = graph().CanConnect(srcQ.toStdString(), plane,
                                       audio ? bps::production::SignalType::Audio
                                             : bps::production::SignalType::Video);
    return ok;
}

QVariantMap BusListModel::getBus(int index) const
{
    if (index < 0 || index >= m_buses.size())
        return {};

    const BusItem &item = m_buses.at(index);
    return {
        { "name", item.name },
        { "type", item.type },
        { "level", item.level },
        { "muted", item.muted },
        { "routedAudioInputs", toVariantList(item.routedAudioInputs) },
        { "routedVideoSources", toVariantList(item.routedVideoSources) },
    };
}
