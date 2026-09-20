#include "BusListModel.h"

#include <QDebug>
#include <QSet>

#include "modules/production/ProductionEngine.hpp"

#include <algorithm>

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
//   - A route is a graph EDGE from a roster source node ("ain:<row>" for
//     AudioInputListModel rows, "vin:<row>" for VideoSourceListModel rows)
//     into the matching plane node. Source nodes are created on demand at
//     first route. Audio routes are additive (mixing); video routes are
//     strictly 1:1 (a bus renders exactly one frame source) — enforced
//     here, exactly as before.
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

// Routes of one plane: the "ain:<i>"/"vin:<i>" nodes feeding it, numerically
// sorted (the graph stores ids alphabetically — "ain:10" would precede
// "ain:2").
QList<int> routesFromGraph(const std::string &planeNodeId, const char *prefix)
{
    QList<int> out;
    const std::string needle = std::string(prefix) + ":";
    for (const auto &up : graph().Upstream(planeNodeId)) {
        if (up.starts_with(needle)) {
            try {
                out.append(std::stoi(up.substr(needle.size())));
            } catch (const std::exception &) {
                // Malformed id — ignore; a route row that isn't ours.
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The roster source node for row `i` — created at first route (idempotent).
void ensureRosterSource(const char *prefix, int i, bps::production::SignalType type)
{
    const std::string id = std::string(prefix) + ":" + std::to_string(i);
    if (!graph().HasNode(id)) {
        const char *kind = std::string(prefix) == "ain" ? "Audio Input " : "Video Source ";
        auto r = graph().AddSource(id, kind + std::to_string(i + 1), type);
        if (!r.ok())
            qWarning() << "BusListModel: AddSource" << id.c_str() << "failed:"
                       << r.error().message.c_str();
    }
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

} // namespace

BusListModel::BusListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // Empty by design — see the header note. Buses appear only through
    // addBus/duplicateBus (or a future engine-side creator).
}

int BusListModel::createBusRow(const QString &name, const QString &type,
                               qreal level, bool muted)
{
    BusItem item;
    item.name = name;
    item.type = type;
    item.level = level;
    item.muted = muted;

    const std::string base = "bus:" + std::to_string(m_nextBus++);
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
    item.routedAudioInputs = routesFromGraph(item.engineId.toStdString(), "ain");
    item.routedVideoSources = routesFromGraph(item.engineIdVideo.toStdString(), "vin");
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
    logIfFailed("RemoveNode(audio)", graph().RemoveNode(item.engineId.toStdString()));
    logIfFailed("RemoveNode(video)", graph().RemoveNode(item.engineIdVideo.toStdString()));

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

    m_buses[index].type = type;
    // A route to a roster the bus no longer accepts from is dead weight —
    // drop the EDGE (the graph, not just the cache). Audio sources fit
    // EVERY bus (a video bus carries the feed with its embedded audio —
    // matches QML's sourceFitsBus, the one compat rule), so only video
    // routes are ever dropped, when the type narrows to pure audio.
    QList<int> roles = { TypeRole };
    if (type != QStringLiteral("video") && type != QStringLiteral("both")
        && !m_buses[index].routedVideoSources.isEmpty()) {
        for (int i : std::as_const(m_buses[index].routedVideoSources))
            logIfFailed("Disconnect(policy)",
                        graph().Disconnect("vin:" + std::to_string(i),
                                           m_buses[index].engineIdVideo.toStdString()));
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
    BusItem &item = m_buses[busIndex];
    const std::string src = "ain:" + std::to_string(inputIndex);
    const std::string plane = item.engineId.toStdString();

    if (feedsFrom(plane, src))
        logIfFailed("Disconnect(audio)", graph().Disconnect(src, plane));
    else {
        ensureRosterSource("ain", inputIndex, bps::production::SignalType::Audio);
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
    const std::string src = "vin:" + std::to_string(sourceIndex);

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
        ensureRosterSource("vin", sourceIndex, bps::production::SignalType::Video);
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
