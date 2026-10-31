#include "BusListModel.h"

#include "AudioInputListModel.h"
#include "ChannelTrim.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/DesignLibraryService.h"
#include "VideoSourceListModel.h"

#include <QDebug>
#include <QPair>
#include <QTimer>
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
//     are additive (mixing); video sources may feed multiple buses, and each
//     bus accepts one frame source.
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

// How many video sources feed one bus's video plane. Read from the GRAPH, never
// from the cached row list: the cache is a convenience, the edges are the truth,
// and the one-video-provider rule must not be bypassable by a stale cache.
int videoProviders(const std::string &planeNodeId)
{
    int n = 0;
    for (const auto &up : graph().Upstream(planeNodeId))
        if (up.starts_with(kVideoSourcePrefix))
            ++n;
    return n;
}

// The audio half of videoProviders — read by a TEMPLATE bus's one-sound rule
// (connectRefusal / connectAudioRoute): however many pictures it carries, a
// template bus has one audio input, so a second input is refused where the
// operator can read why instead of being summed in with no way to tell.
int audioProviders(const std::string &planeNodeId)
{
    int n = 0;
    for (const auto &up : graph().Upstream(planeNodeId))
        if (up.starts_with(kAudioSourcePrefix))
            ++n;
    return n;
}

// THE WINDOWS A TEMPLATE BUS ACTUALLY HAS. A window is a `bus` block on the
// template design, and the routing rule follows from what each one can show:
//
//   total — every `bus` block on the template (the design's windows);
//   open  — the ones that name NO bus (meta busId empty/0). Those are the ones
//           that take the template bus's routed video sources, in route order
//           (SceneBuilder::RecordTemplateWindows / LiveOutputService's
//           addTemplateWindows: TemplateWindowPlaneKey per position). A window
//           that NAMES a bus shows that bus and no route can land in it.
//
// So a template with no bus items at all has nowhere to put a picture: routing a
// camera into it is a wire that can never show anything, and that is refused at
// the source rather than drawn and left black.
//
// `known` is what keeps the rule from acting on ignorance: the template library
// is a lazily-built QML singleton, so at boot (and for a moment after) the
// design map is empty for a reason that has nothing to do with the template —
// "not read yet" must not be mistaken for "no windows" and sweep away routes
// that are still valid.
struct TemplateWindows
{
    bool known = false;
    int total = 0;
    int open = 0;
};

TemplateWindows templateWindows(const QString &templateId)
{
    TemplateWindows w;
    if (templateId.isEmpty())
        return w;
    // The library answers with the DESIGN map (DesignLibraryService::toMap):
    // blocks as QVariantMaps, each with `kind` and `meta` (a map — see
    // ShowConverter::blockToVariant).
    const QVariantMap design = TemplateLibraryService::templates().design(templateId);
    if (design.isEmpty())
        return w;   // not readable yet (or gone) — nothing to judge
    w.known = true;
    const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
    for (const QVariant &v : blocks) {
        const QVariantMap block = v.toMap();
        if (block.value(QStringLiteral("kind")).toString() != QStringLiteral("bus"))
            continue;
        ++w.total;
        const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
        if (meta.value(QStringLiteral("busId")).toInt() <= 0)
            ++w.open;
    }
    return w;
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
    rebuildFromGraph();

    // The board CACHES each bus's routes as ROSTER ROWS (the graph's edges are
    // keyed by each row's stable id; QML wants row numbers). Every roster change
    // can therefore invalidate those cached lists, and a stale list is not
    // cosmetic: it decides what the drag gesture believes, so a route that was
    // already in the graph read as "not connected" and the next drop turned it
    // off. Re-derive the cache on every roster STRUCTURAL change instead of
    // trusting each caller to remember — the two restores do call this, but a
    // restores-while-you-watch (data-protection restore, duplicate, remove)
    // used to leave the board's lines and "n src" counts behind.
    //
    // Only structural signals: a dataChanged from a fader drag does not move a
    // row, and re-pulling every bus on every fader tick would rebuild the
    // connector layer mid-drag for nothing.
    watchRosters();

    // The ghost sweep (see pruneOrphanSourceEdges). Deferred well past the
    // rosters' own restore attempts (they retry over ~2s from their
    // constructors, which this one runs after), and retried in case the engine
    // — and therefore the settings store the rows come from — booted late. The
    // guard inside makes every run after the first a no-op.
    for (int attempt = 0; attempt < 12; ++attempt)
        QTimer::singleShot(2200 + attempt * 400, this, [this, attempt]() {
            pruneOrphanSourceEdges();
            trimExtraVideoProviders();
            // ...and re-pull the cached routes if the rosters only just reported.
            // After the sweeps: a swept ghost must not be re-read as a route.
            refreshRoutesIfStale();
            // Once, after the first repair pass: what the board now believes it is
            // routing (see logBoardState).
            if (attempt == 0)
                logBoardState();
        });

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

// (Re-)aim the roster watch at the CURRENT roster singletons. The constructor
// calls this; adoption calls it again (see adoptRoutingModels), because the
// rosters to watch are not necessarily the instances the constructor saw. A
// board that watches a roster nobody edits never re-derives its cached rows,
// and a cached row is what a connection LINE is drawn from — which is exactly
// how a line came to be drawn to a row that no longer exists.
void BusListModel::watchRosters()
{
    for (const QMetaObject::Connection &c : std::as_const(m_rosterConnections))
        disconnect(c);
    m_rosterConnections.clear();
    const auto rederive = [this]() { refreshRoutes(); };
    const auto watch = [&](QAbstractItemModel *roster) {
        if (!roster)
            return;
        m_rosterConnections.append(connect(roster, &QAbstractItemModel::rowsInserted, this,
                [rederive](const QModelIndex &, int, int) { rederive(); }));
        m_rosterConnections.append(connect(roster, &QAbstractItemModel::rowsRemoved, this,
                [rederive](const QModelIndex &, int, int) { rederive(); }));
        m_rosterConnections.append(connect(roster, &QAbstractItemModel::modelReset, this, rederive));
    };
    watch(AudioInputListModel::Instance());
    watch(VideoSourceListModel::Instance());
}

void BusListModel::adoptRoutingModels(AudioInputListModel *audioRoster,
                                      VideoSourceListModel *videoRoster)
{
    if (s_instance && s_instance != this) {
        // The instance C++ built for itself: the board never shows it, so it
        // stops being a writer of this session's state.
        s_instance->deactivate();
    }
    s_instance = this;   // QML called this on the board's own model
    if (audioRoster)
        AudioInputListModel::adoptInstance(audioRoster);
    if (videoRoster)
        VideoSourceListModel::adoptInstance(videoRoster);
    watchRosters();      // re-aim at the adopted rosters
    refresh();           // and read the truth back in, so no cached row can survive
}

void BusListModel::deactivate()
{
    m_active = false;
    for (const QMetaObject::Connection &c : std::as_const(m_rosterConnections))
        disconnect(c);
    m_rosterConnections.clear();
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
// ("asrc:a3" / "vsrc:v7"), so it survives every row shift. Empty only for a
// row that does not exist.
//
// idForRow(), NOT stableIdForRow(): it HEALS a row that somehow has no id yet
// instead of answering empty for it. An empty answer here is read by the whole
// connect path (canConnect, connectAudioRoute, pushRoutingOffThread) as "this row
// has nothing to route", so a row without an id was unroutable, permanently and
// silently — a drop onto a bus just refused, which is exactly how "a new audio
// source refuses to connect" reads from the board.
QString BusListModel::audioSourceNodeId(int rosterRow)
{
    AudioInputListModel *roster = AudioInputListModel::Instance();
    if (!roster)
        return {};
    const QString id = roster->idForRow(rosterRow);
    return id.isEmpty() ? QString()
                        : QString::fromLatin1(kAudioSourcePrefix) + id;
}

QString BusListModel::videoSourceNodeId(int rosterRow)
{
    VideoSourceListModel *roster = VideoSourceListModel::Instance();
    if (!roster)
        return {};
    const QString id = roster->idForRow(rosterRow);
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

// THE GHOST SWEEP. A source node is live only while a roster row owns its id
// (the id is what a route keys on), so any other "asrc:"/"vsrc:" node in the
// graph is a leftover — and leftovers are not harmless. The id counter restarts
// from the RESTORED rows, so the next row added is handed a freed id and
// inherits the ghost's edges: a line appears for a route nobody made, and the
// line the deleted row was drawn with never goes away, because the edge it
// stands for is still in the graph. This is the sweep the constructor comment
// claimed ("any asrc:/vsrc: node found at startup is a leftover… Drop them
// all") but which no code did.
//
// Each kind is swept only once its roster has ANSWERED (restoreDone): before
// that a roster is empty because it has not loaded, not because it has no rows,
// and sweeping then would cut live routes.
void BusListModel::pruneOrphanSourceEdges()
{
    if (!m_active)
        return;   // a displaced model is not the board's; it does not edit the graph
    const bool audioReady = AudioInputListModel::restoreDone();
    const bool videoReady = VideoSourceListModel::restoreDone();
    if (!audioReady && !videoReady)
        return;

    QList<std::string> ghosts;
    int audioGhosts = 0;
    int videoGhosts = 0;
    for (const std::string &id : graph().NodeIds()) {
        const bool audio = id.starts_with(kAudioSourcePrefix);
        const bool video = !audio && id.starts_with(kVideoSourcePrefix);
        if (!audio && !video)
            continue;
        if (audio ? !audioReady : !videoReady)
            continue;   // that roster has not reported yet
        const char *prefix = audio ? kAudioSourcePrefix : kVideoSourcePrefix;
        const QString stable = QString::fromStdString(id.substr(std::strlen(prefix)));
        const int row = audio ? AudioInputListModel::rowForStableId(stable)
                              : VideoSourceListModel::rowForStableId(stable);
        if (row >= 0)
            continue;   // a live row owns it
        ghosts.append(id);
        if (audio)
            ++audioGhosts;
        else
            ++videoGhosts;
    }
    if (ghosts.isEmpty())
        return;

    {
        GraphEdit edit;   // every cut + removal below is one save
        for (const std::string &ghost : ghosts) {
            const bool audio = ghost.starts_with(kAudioSourcePrefix);
            for (int b = 0; b < m_buses.size(); ++b) {
                const std::string plane = (audio ? m_buses.at(b).engineId
                                                 : m_buses.at(b).engineIdVideo).toStdString();
                if (feedsFrom(plane, ghost))
                    logIfFailed("Disconnect(ghost)", graph().Disconnect(ghost, plane));
            }
            logIfFailed("RemoveNode(ghost)", graph().RemoveNode(ghost));
        }
    }
    qInfo() << "BusListModel: dropped" << int(ghosts.size())
            << "orphan source node(s) left by a roster that no longer owns them"
            << "(audio" << audioGhosts << ", video" << videoGhosts << ")";
    refreshRoutes();   // whatever they fed is no longer fed — redraw the board
}

void BusListModel::trimExtraVideoProviders()
{
    if (!m_active)
        return;
    QList<QPair<std::string, std::string>> cuts;   // source node -> plane node
    for (int row = 0; row < m_buses.size(); ++row) {
        const bool templateBus = m_buses.at(row).type == QStringLiteral("template");
        const std::string plane = m_buses.at(row).engineIdVideo.toStdString();
        QList<std::string> providers;
        for (const auto &up : graph().Upstream(plane)) {
            if (up.starts_with(kVideoSourcePrefix))
                providers.append(up);
        }
        // A TEMPLATE bus is the one bus that is SUPPOSED to carry several video
        // sources, so the one-provider sweep leaves every route it has alone. It is
        // not trimmed to its windows either: the windows name their own buses, and
        // the feeds routed into the template bus are exactly the ones the operator
        // chose to give it.
        if (templateBus)
            continue;
        if (providers.size() < 2)
            continue;
        // The first is the one the engine reads (the preview and the mixer take
        // the first route); everything after it is unreachable in the app.
        for (int i = 1; i < providers.size(); ++i)
            cuts.append({providers.at(i), plane});
    }
    if (cuts.isEmpty())
        return;

    {
        GraphEdit edit;
        for (const auto &cut : cuts) {
            if (graph().Disconnect(cut.first, cut.second).ok()) {
                qInfo() << "BusListModel:" << QString::fromStdString(cut.second)
                        << "dropped an extra video provider"
                        << QString::fromStdString(cut.first)
                        << "— a bus shows one video source, a template bus one per window";
            }
        }
    }
    refreshRoutes();
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
        if (auto r = graph().GetNode(a); r.ok()) {
            item.type = QString::fromStdString(
                r.value().meta.count("uiBusType") ? r.value().meta.at("uiBusType")
                                                  : "audio");
            // ON THE ROUTING BOARD? Absent meta = no (see BusItem::onBoard):
            // every bus saved before the board had a membership reads as off it,
            // which is the rule — only what the operator adds shows.
            const auto shown = r.value().meta.find("uiOnBoard");
            item.onBoard = shown != r.value().meta.end() && shown->second == "1";
        }
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
// Re-reads every row's routes (and level/mute/name) from the graph. Routes
// are cached as roster ROWS, so ANY roster row shift (a removal) leaves every
// bus's cached lists stale — the roster models call this after a removal.
void BusListModel::refreshRoutes()
{
    if (!m_active || m_buses.isEmpty())
        return;
    for (int row = 0; row < m_buses.size(); ++row)
        pullRow(row);
    emit dataChanged(index(0), index(int(m_buses.size()) - 1),
                     { RoutedAudioInputsRole, RoutedVideoSourcesRole });
}

// The deferral-time repair (see the header): while any bus's cache and the
// graph disagree about how many routes that bus has, re-pull. Counted, not
// compared row by row: both sides come from the same translation, so the case
// this exists for is a cache built before the rosters had rows (every edge
// translating to "no such row": 0 against the graph's N). A roster ROW SHIFT
// keeps the counts and is already covered by the roster's own structural
// signals (watchRosters), which is why this does not drag every bus through a
// full re-pull on every tick.
void BusListModel::logBoardState() const
{
    for (int row = 0; row < m_buses.size(); ++row) {
        const BusItem &item = m_buses[row];
        // Level and per-side trims are on the line because they are the OTHER
        // reason a bus card's L/R pair can sit dark: the pair reads the bus's
        // POST-FADER mix, so a bus at volume 0 (where buses are created) is flat
        // however healthy its routes are. "Not glowing" and "not connected" then
        // look identical from outside, and only the numbers tell them apart.
        qInfo().noquote()
            << QStringLiteral("BusListModel: bus %1 \u201C%2\u201D (%3) routes audio %4 video %5 "
                              "level %6 muted %7 gains %8/%9")
                   .arg(QString::number(row), item.name, item.type,
                        QString::number(item.routedAudioInputs.size()),
                        QString::number(item.routedVideoSources.size()),
                        QString::number(item.level, 'f', 1),
                        item.muted ? QStringLiteral("yes") : QStringLiteral("no"),
                        QString::number(item.channelGains.value(0, 1.0), 'f', 2),
                        QString::number(item.channelGains.value(1, 1.0), 'f', 2));
    }
}

void BusListModel::refreshRoutesIfStale()
{
    if (!m_active || m_buses.isEmpty())
        return;
    for (const BusItem &item : m_buses) {
        if (audioRoutesFromGraph(item.engineId.toStdString()).size()
                != item.routedAudioInputs.size()
            || videoRoutesFromGraph(item.engineIdVideo.toStdString()).size()
                != item.routedVideoSources.size()) {
            qInfo() << "BusListModel: re-pulling routes — the cached rows disagreed with the graph"
                    << "(a roster that reported after this board was built)";
            refreshRoutes();
            return;
        }
    }
}

// Full re-sync from the graph — rows added/removed/renamed behind our back.
void BusListModel::refresh()
{
    if (!m_active)
        return;
    beginResetModel();
    m_buses.clear();
    rebuildFromGraph();
    endResetModel();
    // A document that just (re)arrived is held to the running rules in the same
    // session, not on the next launch: an older graph can carry two video
    // providers on one bus or an "asrc:"/"vsrc:" node its roster no longer
    // lists, and until this, only the constructor's deferred sweep ever looked —
    // so a doc restored late kept the extra route (and its line) all session.
    // Both calls are cheap no-ops on a clean graph, and each guards its own
    // readiness (a roster that has not reported is not "empty", it is unknown).
    pruneOrphanSourceEdges();
    trimExtraVideoProviders();
}

int BusListModel::createBusRow(const QString &name, const QString &type,
                               qreal level, bool muted, const QString &templateId,
                               bool onBoard)
{
    BusItem item;
    item.name = name;
    item.type = type;
    item.level = level;
    item.muted = muted;
    item.templateId = templateId;
    item.onBoard = onBoard;

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
    if (!templateId.isEmpty()) {
        logIfFailed("SetMeta(templateId audio)",
                    graph().SetMeta(item.engineId.toStdString(), "templateId", templateId.toStdString()));
        logIfFailed("SetMeta(templateId video)",
                    graph().SetMeta(item.engineIdVideo.toStdString(), "templateId", templateId.toStdString()));
    }
    // Board membership, written only when it is ON: an absent key reads as "off"
    // everywhere (see rebuildFromGraph), so a bus made off the board carries no
    // meta at all until the operator adds it.
    if (onBoard) {
        logIfFailed("SetMeta(uiOnBoard audio)",
                    graph().SetMeta(item.engineId.toStdString(), "uiOnBoard", "1"));
        logIfFailed("SetMeta(uiOnBoard video)",
                    graph().SetMeta(item.engineIdVideo.toStdString(), "uiOnBoard", "1"));
    }
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
    if (auto r = graph().GetNode(item.engineId.toStdString()); r.ok()) {
        item.name = QString::fromStdString(r.value().displayName);
        // The per-side trim comes back off the AUDIO plane's meta — the same
        // key the mixer reads (see channelGain/setChannelGain), so a restart
        // restores which sides were closed. A key that is absent means unity
        // (an older graph, or a bus nobody has trimmed), never 0.
        item.channelGains.clear();
        // ...and the trim each side was closed at, off its OWN key. A graph
        // saved before the trims existed has none: channelTrim() then falls back
        // to the level the side holds open (unity beyond that), so a restored
        // bus can never reopen a side at 0.
        item.channelTrims.clear();
        for (int channel = 0; channel < 2; ++channel) {
            auto readMeta = [&r](const std::string &key, qreal fallback) {
                const auto it = r.value().meta.find(key);
                if (it == r.value().meta.end())
                    return fallback;
                bool ok = false;
                const double parsed = QString::fromStdString(it->second).toDouble(&ok);
                return ok ? std::clamp(parsed, 0.0, 1.0) : fallback;
            };
            const qreal gain = readMeta("uiChannelGain" + std::to_string(channel), 1.0);
            item.channelGains.append(gain);
            item.channelTrims.append(readMeta("uiChannelTrim" + std::to_string(channel), 0.0));
        }
        // The TEMPLATE this bus was created from ("" = an ordinary bus), off the
        // templateId meta a template bus carries. Read here as well as at rebuild
        // so a bus that comes back from an undo or a late document restore knows
        // its template without a restart.
        const auto templateId = r.value().meta.find("templateId");
        item.templateId = templateId == r.value().meta.end()
            ? QString() : QString::fromStdString(templateId->second);
        // ...and whether it is on the Routing board (absent = off, see
        // BusItem::onBoard). Re-read here so adding it there takes effect without
        // a restart, and so a document restored late carries its membership.
        const auto shown = r.value().meta.find("uiOnBoard");
        item.onBoard = shown != r.value().meta.end() && shown->second == "1";
    }
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
    case ChannelGainsRole: {
        QVariantList gains;
        for (qreal g : item.channelGains)
            gains.append(g);
        return gains;
    }
    case RoutedAudioInputsRole: return toVariantList(item.routedAudioInputs);
    case RoutedVideoSourcesRole: return toVariantList(item.routedVideoSources);
    case OnBoardRole: return item.onBoard;
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
        { ChannelGainsRole, "channelGains" },
        { RoutedAudioInputsRole, "routedAudioInputs" },
        { RoutedVideoSourcesRole, "routedVideoSources" },
        { OnBoardRole, "onBoard" },
    };
}

void BusListModel::addBus(const QString &name, const QString &type)
{
    const QString trimmed = name.trimmed();
    const int row = m_buses.size();

    beginInsertRows(QModelIndex(), row, row);
    createBusRow(trimmed.isEmpty() ? QStringLiteral("New Bus %1").arg(row + 1) : trimmed,
                 type.isEmpty() ? QStringLiteral("audio") : type, 75, false);
    endInsertRows();
}

int BusListModel::addBusOnBoard(const QString &name, const QString &type)
{
    const QString trimmed = name.trimmed();
    const int row = m_buses.size();
    beginInsertRows(QModelIndex(), row, row);
    createBusRow(trimmed.isEmpty() ? QStringLiteral("New Bus %1").arg(row + 1) : trimmed,
                 type.isEmpty() ? QStringLiteral("audio") : type, 75, false, QString(),
                 true);
    endInsertRows();
    return row;
}

void BusListModel::setBusOnBoard(int index, bool onBoard)
{
    if (index < 0 || index >= m_buses.size())
        return;
    BusItem &item = m_buses[index];
    if (item.onBoard == onBoard)
        return;
    item.onBoard = onBoard;
    GraphEdit edit;
    for (const QString &id : { item.engineId, item.engineIdVideo })
        logIfFailed("SetMeta(uiOnBoard)",
                    graph().SetMeta(id.toStdString(), "uiOnBoard", onBoard ? "1" : "0"));
    const QModelIndex changed = this->index(index, 0);
    emit dataChanged(changed, changed, { OnBoardRole });
}

int BusListModel::addTemplateBus(const QString &name, const QString &templateId)
{
    const QString id = templateId.trimmed();
    if (id.isEmpty() || TemplateLibraryService::templates().design(id).isEmpty())
        return -1;
    const int row = m_buses.size();
    beginInsertRows(QModelIndex(), row, row);
    const QString busName = name.trimmed().isEmpty()
        ? QStringLiteral("Template Bus %1").arg(row + 1) : name.trimmed();
    createBusRow(busName, QStringLiteral("template"), 75, false, id);
    endInsertRows();
    return row;
}

bool BusListModel::usesTemplate(const QString &templateId) const
{
    if (templateId.isEmpty())
        return false;
    return std::any_of(m_buses.cbegin(), m_buses.cend(), [&](const BusItem &bus) {
        return bus.templateId == templateId;
    });
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

bool BusListModel::deleteBus(int index)
{
    if (index < 0 || index >= m_buses.size())
        return false;

    // CAPTURE BEFORE THE ROWS GO: the item's own routing lists are ROSTER ROWS,
    // and a row is a position in a list that can move. The node ids behind them
    // are what an edge is really keyed on, so those are what a restore reconnects.
    RemovedBus back;
    back.row = index;
    back.item = m_buses.at(index);
    for (const int row : std::as_const(back.item.routedAudioInputs)) {
        const QString node = audioSourceNodeId(row);
        if (!node.isEmpty())
            back.audioSources.append(node);
    }
    for (const int row : std::as_const(back.item.routedVideoSources)) {
        const QString node = videoSourceNodeId(row);
        if (!node.isEmpty())
            back.videoSources.append(node);
    }

    const int before = m_buses.size();
    removeBus(index);
    if (m_buses.size() == before)
        return false;   // the graph still held it — there is nothing to undo
    m_removedBus = back;

    // The notice is raised HERE, not at each call site: every door onto "delete
    // this bus" (the pane's menu, the board's own Delete, the bus dialog's) then
    // offers the same one click back, and a new door cannot forget to.
    EventBus::instance().notify(tr("\u201c%1\u201d was removed \u2014 Undo puts it back.").arg(back.item.name),
                                QStringLiteral("info"), tr("Buses"),
                                QStringLiteral("ui.buses.removed"), QVariant(),
                                QVariantMap{{ QStringLiteral("id"), QStringLiteral("buses.undoRemove") },
                                            { QStringLiteral("label"), tr("Undo") }});
    return true;
}

QString BusListModel::removedBusName() const
{
    return m_removedBus.has_value() ? m_removedBus->item.name : QString();
}

bool BusListModel::restoreRemovedBus()
{
    if (!m_removedBus.has_value())
        return false;
    const RemovedBus back = std::move(*m_removedBus);
    m_removedBus.reset();

    // THE IDENTITY IS THE POINT: a restore that minted a new bus number would put
    // back a STRANGER — a slide, a scene or a saved route that names the deleted
    // bus would not find what came back. So the same node ids are reused, and if
    // something else already answers to them this refuses instead of duplicating.
    const std::string audio = back.item.engineId.toStdString();
    const std::string video = back.item.engineIdVideo.toStdString();
    if (graph().HasNode(audio) || graph().HasNode(video)) {
        qWarning() << "BusListModel: cannot put" << back.item.name
                   << "back \u2014 another node holds its identity now";
        return false;
    }

    GraphEdit edit;   // the nodes, their meta, their volume AND their routes: one save
    logIfFailed("AddBus(audio)", graph().AddBus(audio, back.item.name.toStdString(),
                                                 bps::production::BusRole::Custom,
                                                 bps::production::SignalType::Audio));
    logIfFailed("AddBus(video)", graph().AddBus(video, back.item.name.toStdString(),
                                                 bps::production::BusRole::Custom,
                                                 bps::production::SignalType::Video));
    logIfFailed("SetMeta(audio)",
                graph().SetMeta(audio, "uiBusType", back.item.type.toStdString()));
    logIfFailed("SetMeta(video)",
                graph().SetMeta(video, "uiBusType", back.item.type.toStdString()));
    // ...and the template it came from, without which a restored template bus
    // would come back as an ordinary one that accepts many video sources and
    // composes none of them.
    if (!back.item.templateId.isEmpty()) {
        logIfFailed("SetMeta(templateId)",
                    graph().SetMeta(audio, "templateId", back.item.templateId.toStdString()));
        logIfFailed("SetMeta(templateId)",
                    graph().SetMeta(video, "templateId", back.item.templateId.toStdString()));
    }
    // ...and its place on the Routing board. The membership lives in the same
    // node meta as the template, so a restore that skips it puts the bus back
    // into the roster and NOT onto the board it was deleted from: Undo would read
    // as "it came back somewhere else", and every wire the operator had drawn to
    // its card would point at a node the canvas no longer draws.
    if (back.item.onBoard) {
        logIfFailed("SetMeta(uiOnBoard audio)",
                    graph().SetMeta(audio, "uiOnBoard", "1"));
        logIfFailed("SetMeta(uiOnBoard video)",
                    graph().SetMeta(video, "uiOnBoard", "1"));
    }
    const bps::production::VolumeControl v{ dbFromLevel(back.item.level), back.item.muted,
                                            false, 0.0, 0.0 };
    logIfFailed("SetVolume(audio)", graph().SetVolume(audio, v));
    logIfFailed("SetVolume(video)", graph().SetVolume(video, v));
    // The edges the removal took with it, re-made from the stable source nodes.
    for (const QString &src : std::as_const(back.audioSources)) {
        const std::string node = src.toStdString();
        if (graph().HasNode(node))
            logIfFailed("Connect(audio)", graph().Connect(node, audio,
                                                          bps::production::SignalType::Audio));
    }
    for (const QString &src : std::as_const(back.videoSources)) {
        const std::string node = src.toStdString();
        // A video plane takes one provider: a source that has since been routed to
        // this bus's replacement, or a bus that has become audio-only, is skipped
        // rather than forced — the bus still comes back, just not that route.
        if (!graph().HasNode(node) || !graph().CanConnect(node, video,
                                                          bps::production::SignalType::Video))
            continue;
        logIfFailed("Connect(video)", graph().Connect(node, video,
                                                      bps::production::SignalType::Video));
    }

    const int row = std::clamp(back.row, 0, int(m_buses.size()));
    beginInsertRows(QModelIndex(), row, row);
    m_buses.insert(row, back.item);
    endInsertRows();

    // The per-side trims ride the plane node's meta, which went with the node.
    for (int ch = 0; ch < back.item.channelGains.size(); ++ch)
        setChannelGain(row, ch, back.item.channelGains[ch]);
    for (int ch = 0; ch < back.item.channelTrims.size(); ++ch)
        setChannelTrim(row, ch, back.item.channelTrims[ch]);

    // The cached route rows come from the graph, so this is where the restored
    // edges turn back into the card's line list.
    pullRow(row);
    const QModelIndex changed = index(row);
    emit dataChanged(changed, changed);
    return true;
}

int BusListModel::duplicateBus(int index)
{
    if (index < 0 || index >= m_buses.size())
        return -1;

    // COPIED BY VALUE, not held by reference: createBusRow() APPENDS to m_buses,
    // which reallocates it — a `const BusItem &` into the vector would dangle for
    // every read after that call (the template id below was read from freed
    // memory, and "duplicate" of a template bus then copied garbage).
    const BusItem src = m_buses.at(index);
    beginInsertRows(QModelIndex(), m_buses.size(), m_buses.size());
    // A copy lands wherever the original was: duplicating a card on the board
    // makes another card, duplicating a roster bus keeps it off the board.
    const int row = createBusRow(QStringLiteral("%1 · copy").arg(src.name), src.type,
                                 src.level, false, QString(), src.onBoard);
    // A COPY OF A TEMPLATE BUS IS ONE TOO: the type alone would make it a
    // template bus with no template — it would take many video sources and then
    // have nothing to compose them into.
    if (!src.templateId.isEmpty()) {
        GraphEdit edit;
        m_buses[row].templateId = src.templateId;
        for (const QString &id : { m_buses[row].engineId, m_buses[row].engineIdVideo })
            logIfFailed("SetMeta(templateId)",
                        graph().SetMeta(id.toStdString(), "templateId",
                                        src.templateId.toStdString()));
    }
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
    // A TEMPLATE BUS KEEPS ITS KIND. Its many video inputs, its one audio input
    // and the design it composes all follow from "template", and the dialogs
    // that offer the three ordinary kinds know nothing about it — a save there
    // would silently turn a layout back into a plain bus and lose its windows.
    if (m_buses[index].type == QStringLiteral("template"))
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
    // "template" accepts video (its windows are fed by it), so it is on the
    // video-accepting side of this rule and keeps whatever is routed.
    if (type != QStringLiteral("video") && type != QStringLiteral("both")
        && type != QStringLiteral("template")
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

qreal BusListModel::channelGain(int index, int channel) const
{
    if (index < 0 || index >= m_buses.size() || channel < 0)
        return 1.0;
    const QList<qreal> &gains = m_buses[index].channelGains;
    return channel < gains.size() ? gains.at(channel) : 1.0;
}

// One side of the bus's own output trim — the bus-card pill's write, and the
// same operation AudioInputListModel::setChannelGain performs on a source
// row: gain 0 IS "this side is closed" (there is no separate enable bit
// anywhere in the app), and the value is carried to the engine as the node
// meta the mixer already understands.
void BusListModel::setChannelGain(int index, int channel, qreal gain)
{
    if (index < 0 || index >= m_buses.size() || channel < 0 || channel >= 2)
        return;
    const qreal clamped = std::clamp(gain, 0.0, 1.0);
    QList<qreal> &gains = m_buses[index].channelGains;
    while (gains.size() <= channel)
        gains.append(1.0);   // absent = unity
    if (qFuzzyCompare(gains[channel], clamped))
        return;
    gains[channel] = clamped;

    // Written on the bus's AUDIO plane — the node the mixer actually sums,
    // and the one whose meta is read back into channelGains. The video plane
    // carries no audio signal type, so it needs no trim.
    GraphEdit edit;
    logIfFailed("SetMeta(channelGain)",
                graph().SetMeta(m_buses[index].engineId.toStdString(),
                                "uiChannelGain" + std::to_string(channel),
                                std::to_string(clamped)));

    // ...and remember an OPEN level as that side's trim while we are here, so a
    // bus card reopened from its L/R pill gives back the level it was closed at
    // (models/ChannelTrim.h). A close (gain 0) deliberately records nothing.
    if (channeltrim::remembersTrim(clamped) && writeChannelTrim(index, channel, clamped)) {
        logIfFailed("SetMeta(channelTrim)",
                    graph().SetMeta(m_buses[index].engineId.toStdString(),
                                    "uiChannelTrim" + std::to_string(channel),
                                    std::to_string(clamped)));
    }

    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ChannelGainsRole });
}

void BusListModel::toggleChannelGain(int index, int channel)
{
    // The bus card's L/R pill, through the same one policy as a source row's.
    if (channeltrim::isOff(channelGain(index, channel)))
        setChannelGain(index, channel, channelTrim(index, channel));
    else
        setChannelGain(index, channel, channeltrim::closed());
}

qreal BusListModel::channelTrim(int index, int channel) const
{
    if (index < 0 || index >= m_buses.size() || channel < 0)
        return channeltrim::unity();
    const BusItem &item = m_buses.at(index);
    const qreal remembered = channel < item.channelTrims.size()
        ? item.channelTrims.at(channel) : channeltrim::closed();
    return channeltrim::reopenGain(remembered, channelGain(index, channel));
}

void BusListModel::setChannelTrim(int index, int channel, qreal trim)
{
    if (index < 0 || index >= m_buses.size() || channel < 0 || channel >= 2)
        return;
    const qreal clamped = std::clamp(trim, 0.0, 1.0);
    if (!writeChannelTrim(index, channel, clamped))
        return;
    GraphEdit edit;
    logIfFailed("SetMeta(channelTrim)",
                graph().SetMeta(m_buses[index].engineId.toStdString(),
                                "uiChannelTrim" + std::to_string(channel),
                                std::to_string(clamped)));
}

bool BusListModel::writeChannelTrim(int index, int channel, qreal trim)
{
    if (index < 0 || index >= m_buses.size() || channel < 0 || channel >= 2)
        return false;
    QList<qreal> &trims = m_buses[index].channelTrims;
    while (trims.size() <= channel)
        trims.append(channeltrim::closed());   // absent = nothing remembered
    if (qFuzzyCompare(trims[channel], trim))
        return false;
    trims[channel] = trim;
    return true;
}

// Add-only (idempotent). The graph edge is the truth: a route that is already
// there is left alone and reported as connected, so a drag can never remove one
// (see the header — this is the gesture's own call).
bool BusListModel::connectAudioRoute(int busIndex, int inputIndex)
{
    if (busIndex < 0 || busIndex >= m_buses.size())
        return false;
    // QML passes a roster ROW; the edge keys on the row's STABLE id node —
    // a bad/vanished row has no node and nothing to route.
    const QString srcQ = audioSourceNodeId(inputIndex);
    if (srcQ.isEmpty())
        return false;
    BusItem &item = m_buses[busIndex];
    const std::string src = srcQ.toStdString();
    const std::string plane = item.engineId.toStdString();
    const QModelIndex changed = index(busIndex);

    if (feedsFrom(plane, src)) {
        pullRow(busIndex);          // cache the truth, but add nothing
        emit dataChanged(changed, changed, { RoutedAudioInputsRole });
        return true;
    }
    // A TEMPLATE BUS HAS ONE SOUND (see connectRefusal): the board's drop is
    // refused before it gets here, this catches the bus dialog's toggle list and
    // anything scripted.
    if (item.type == QStringLiteral("template") && audioProviders(plane) > 0) {
        qWarning() << "BusListModel: audio route refused —" << item.name
                   << "is a template bus and already carries its one audio input";
        return false;
    }

    GraphEdit edit;   // (AddSource + Connect), one save
    if (!graph().HasNode(src)) {
        logIfFailed("AddSource(audio)",
                    graph().AddSource(src, "Audio Source",
                                      bps::production::SignalType::Audio));
    }
    const auto connected = graph().Connect(src, plane,
                                           bps::production::SignalType::Audio);
    if (!connected.ok()) {
        qWarning() << "BusListModel: Connect(audio) failed:"
                   << connected.error().message.c_str() << "— route not added";
        pullRow(busIndex);
        emit dataChanged(changed, changed, { RoutedAudioInputsRole });
        return false;
    }
    // addInput() and its fader/effects setters run before a route creates
    // this graph node. Push the saved row state now that the node exists;
    // otherwise it keeps ProductionGraph's unity defaults and the live
    // bus path disagrees with the source controls (including a 0 fader).
    if (AudioInputListModel *inputs = AudioInputListModel::Instance())
        inputs->pushEffectsOffThread(inputIndex);

    pullRow(busIndex);
    emit dataChanged(changed, changed, { RoutedAudioInputsRole });
    return true;
}

void BusListModel::toggleAudioRoute(int busIndex, int inputIndex)
{
    // Existing edge -> the REMOVE half (the board's right-click on a line, the
    // bus dialog's toggle list). Anything else is the add half, which must never
    // remove — one implementation, see connectAudioRoute. The remove half is
    // shared with the source ROW's own delete (disconnectAudioRoute).
    if (disconnectAudioRoute(busIndex, inputIndex))
        return;
    (void)connectAudioRoute(busIndex, inputIndex);
}

bool BusListModel::disconnectAudioRoute(int busIndex, int inputIndex)
{
    if (busIndex < 0 || busIndex >= m_buses.size())
        return false;
    const QString srcQ = audioSourceNodeId(inputIndex);
    if (srcQ.isEmpty())
        return false;   // a bad/vanished roster row has no node to cut
    const std::string plane = m_buses.at(busIndex).engineId.toStdString();
    if (!feedsFrom(plane, srcQ.toStdString()))
        return false;   // nothing routed — never the ADD half
    GraphEdit edit;
    logIfFailed("Disconnect(audio)",
                graph().Disconnect(srcQ.toStdString(), plane));
    pullRow(busIndex);
    const QModelIndex changed = index(busIndex);
    emit dataChanged(changed, changed, { RoutedAudioInputsRole });
    return true;
}

// Add-only (idempotent), the video half of connectAudioRoute — the drag calls
// this, so dropping a source onto a bus it already feeds cannot unroute it.
// Video sources fan out to multiple buses AND a bus takes several sources; the
// graph's CanConnect() is the one policy answer (it was a one-provider-per-bus
// rule, which refused a second video source with no explanation on screen).
bool BusListModel::connectVideoRoute(int busIndex, int sourceIndex)
{
    if (busIndex < 0 || busIndex >= m_buses.size())
        return false;
    // QML passes a roster ROW; the edge keys on the row's STABLE id node —
    // a bad/vanished row has no node and nothing to route.
    const QString srcQ = videoSourceNodeId(sourceIndex);
    if (srcQ.isEmpty())
        return false;
    if (m_buses[busIndex].type == QStringLiteral("audio"))
        return false;
    const std::string src = srcQ.toStdString();
    const std::string plane = m_buses[busIndex].engineIdVideo.toStdString();
    const QModelIndex changed = index(busIndex);

    if (feedsFrom(plane, src)) {
        pullRow(busIndex);
        emit dataChanged(changed, changed, { RoutedVideoSourcesRole });
        return true;
    }
    // ONE VIDEO PROVIDER PER BUS (the header's rule, see connectRefusal). The
    // board's drag is refused before it gets here; this catches the other doors
    // into a route — the bus dialog's toggle list and anything scripted — and
    // says which bus is already spoken for. A route comes OFF through that same
    // toggle, a line's right-click, or deleting the source row, never by
    // stacking a second provider on top.
    //
    // A TEMPLATE bus is exempt from the ONE-video-provider rule: it takes many video
    // sources, with no cap by window — the windows name their own buses, and routing
    // those feeds into the template bus is how it gets them. Only a template with no
    // bus items at all is refused (nothing to show). The board refuses that before it
    // gets here; this catches the bus dialog's toggle list and anything scripted.
    if (m_buses[busIndex].type == QStringLiteral("template")) {
        const TemplateWindows windows = templateWindows(m_buses.at(busIndex).templateId);
        if (windows.known && windows.total == 0) {
            qWarning() << "BusListModel: video route refused —" << m_buses.at(busIndex).name
                       << "is a template bus and its template has no bus items";
            return false;
        }
    } else if (videoProviders(plane) > 0) {
        qWarning() << "BusListModel: video route refused —" << m_buses.at(busIndex).name
                   << "already carries a video source; remove that route first";
        return false;
    }
    if (!graph().CanConnect(src, plane, bps::production::SignalType::Video)) {
        qWarning() << "BusListModel: video route refused — audio-only bus,"
                   << "signal mismatch, or it would create a cycle";
        return false;
    }

    GraphEdit edit;
    if (!graph().HasNode(src)) {
        logIfFailed("AddSource(video)",
                    graph().AddSource(src, "Video Source",
                                      bps::production::SignalType::Video));
    }
    const auto connected = graph().Connect(src, plane,
                                           bps::production::SignalType::Video);
    if (!connected.ok()) {
        qWarning() << "BusListModel: Connect(video) failed:"
                   << connected.error().message.c_str() << "— route not added";
        pullRow(busIndex);
        emit dataChanged(changed, changed, { RoutedVideoSourcesRole });
        return false;
    }

    pullRow(busIndex);
    emit dataChanged(changed, changed, { RoutedVideoSourcesRole });
    return true;
}

void BusListModel::toggleVideoRoute(int busIndex, int sourceIndex)
{
    // Existing edge -> the REMOVE half; anything else is the add half (see the
    // audio half above for why the cut lives in disconnectVideoRoute).
    if (disconnectVideoRoute(busIndex, sourceIndex))
        return;
    (void)connectVideoRoute(busIndex, sourceIndex);
}

bool BusListModel::disconnectVideoRoute(int busIndex, int sourceIndex)
{
    if (busIndex < 0 || busIndex >= m_buses.size())
        return false;
    const QString srcQ = videoSourceNodeId(sourceIndex);
    if (srcQ.isEmpty())
        return false;   // a bad/vanished roster row has no node to cut
    const std::string plane = m_buses.at(busIndex).engineIdVideo.toStdString();
    if (!feedsFrom(plane, srcQ.toStdString()))
        return false;   // nothing routed — never the ADD half
    GraphEdit edit;
    logIfFailed("Disconnect(video)",
                graph().Disconnect(srcQ.toStdString(), plane));
    pullRow(busIndex);
    const QModelIndex changed = index(busIndex);
    emit dataChanged(changed, changed, { RoutedVideoSourcesRole });
    return true;
}

// THE board's policy, in one place (the header's contract). Order matters: the
// answer names the FIRST reason, so "you already did this" reads differently
// from "this bus is taken" and neither is reported as a generic refusal.
QString BusListModel::connectRefusal(const QString &kind, int sourceRow, int busIndex) const
{
    if (busIndex < 0 || busIndex >= m_buses.size() || sourceRow < 0)
        return QStringLiteral("no-source-row");
    if (kind != QStringLiteral("audio") && kind != QStringLiteral("video"))
        return QStringLiteral("no-source-row");
    const bool audio = (kind == QStringLiteral("audio"));
    const QString srcQ = audio ? audioSourceNodeId(sourceRow) : videoSourceNodeId(sourceRow);
    if (srcQ.isEmpty())
        return QStringLiteral("no-source-row");   // a bad/vanished roster row has no node
    const BusItem &bus = m_buses.at(busIndex);
    const bool templateBus = bus.type == QStringLiteral("template");
    if (!audio && bus.type == QStringLiteral("audio"))
        return QStringLiteral("audio-only-bus");
    const std::string plane = (audio ? bus.engineId : bus.engineIdVideo).toStdString();
    if (feedsFrom(plane, srcQ.toStdString()))
        return QStringLiteral("already-connected");
    // A TEMPLATE BUS HAS ONE SOUND. Its pictures may be many (one per window),
    // its audio is single: a second input would be summed with the first with
    // nothing on screen saying which one the operator is hearing.
    if (audio && templateBus && audioProviders(plane) > 0)
        return QStringLiteral("template-bus-has-audio");
    // A TEMPLATE BUS ACCEPTS ONLY WHAT ITS TEMPLATE HAS A WINDOW FOR. Its windows
    // ARE the template's bus items (see templateWindows), so a feed routed into
    // it lands in one — and a template with no free window has nowhere to put the
    // picture. Refusing here is the board's red card AND its toast; the same rule
    // is enforced again in connectVideoRoute for the doors that skip the board.
    if (!audio && templateBus) {
        const TemplateWindows windows = templateWindows(bus.templateId);
        // Only a template with NO bus items is refused: there is nothing for the feed to
        // show in. A template whose windows already name their buses still takes routes —
        // those are the feeds the template bus carries — so there is no cap by window.
        if (windows.known && windows.total == 0)
            return QStringLiteral("template-has-no-windows");
    }
    if (!audio && !templateBus && videoProviders(plane) > 0) {
        // ONE VIDEO PROVIDER PER BUS — the rule the header has always stated. A
        // bus's video plane carries one picture (the preview and the mixer read
        // the first route and ignore the rest), so a second video source is a
        // route the operator can neither see nor use. It used to be refused by
        // the graph with nothing on screen saying why; it is refused here now,
        // where the board's red card and its toast can both read the answer.
        return QStringLiteral("bus-has-video");
    }
    if (!graph().CanConnect(srcQ.toStdString(), plane,
                            audio ? bps::production::SignalType::Audio
                                  : bps::production::SignalType::Video))
        return QStringLiteral("engine-refused");
    return {};
}

bool BusListModel::canConnect(const QString &kind, int sourceRow, int busIndex) const
{
    return connectRefusal(kind, sourceRow, busIndex).isEmpty();
}

QVariantMap BusListModel::getBus(int index) const
{
    if (index < 0 || index >= m_buses.size())
        return {};

    const BusItem &item = m_buses.at(index);
    // `id` is the bus's STABLE number, and callers pick/compare by it (a bus
    // block on a slide carries it, the picker selects by it, a canvas item
    // resolves its row from it). It was NOT in this map, so every reader got
    // `undefined` — which is why every card in the Add Bus picker read as
    // picked (all ids "equal" the default) and why a canvas bus item could
    // never find its row. The engine node ids go with it for the same reason:
    // they are the bus's identity in the graph, not its row here.
    return {
        { "id", item.id },
        { "name", item.name },
        { "type", item.type },
        { "level", item.level },
        { "muted", item.muted },
        { "routedAudioInputs", toVariantList(item.routedAudioInputs) },
        { "routedVideoSources", toVariantList(item.routedVideoSources) },
        { "engineId", item.engineId },
        { "engineIdVideo", item.engineIdVideo },
        // "" for an ordinary bus; a template DESIGN id for a bus created from
        // one (the Routing card's Template · N in subtitle and the output's
        // composed picture both read it).
        { "templateId", item.templateId },
        // Whether this bus has a card on the Routing board (see BusItem::onBoard).
        { "onBoard", item.onBoard },
    };
}
