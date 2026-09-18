#include "BusListModel.h"

#include <algorithm>

namespace {
QVariantList toVariantList(const QList<int> &indices)
{
    QVariantList list;
    for (int i : indices)
        list.append(i);
    return list;
}
}

BusListModel::BusListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // Seeded roster (audio inputs 0=Mic 1..., video sources 0=Cam 1...) —
    // but NO seeded routing. Lines on the board must be drawn by the user,
    // not hardcoded demo mappings: pre-wired routes made real drops look
    // like no-ops ("can't map two sources to one bus"). Rosters are data;
    // mappings are user state.
    BusItem master;
    master.name = QStringLiteral("Master");
    master.type = QStringLiteral("audio");
    master.level = 80;
    m_buses.append(master);

    BusItem worshipGroup;
    worshipGroup.name = QStringLiteral("Worship Group");
    worshipGroup.type = QStringLiteral("audio");
    worshipGroup.level = 70;
    m_buses.append(worshipGroup);

    BusItem stage;
    stage.name = QStringLiteral("Stage");
    stage.type = QStringLiteral("audio");
    stage.level = 60;
    stage.muted = true;
    m_buses.append(stage);

    BusItem program;
    program.name = QStringLiteral("Program");
    program.type = QStringLiteral("video");
    program.level = 85;
    m_buses.append(program);

    BusItem liveStream;
    liveStream.name = QStringLiteral("Live Stream");
    liveStream.type = QStringLiteral("video");
    liveStream.level = 75;
    m_buses.append(liveStream);
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
    BusItem item;
    item.name = trimmed.isEmpty() ? QStringLiteral("New Bus %1").arg(row + 1) : trimmed;
    item.type = type.isEmpty() ? QStringLiteral("audio") : type;
    m_buses.append(item);
    endInsertRows();
}

void BusListModel::removeBus(int index)
{
    if (index < 0 || index >= m_buses.size())
        return;

    beginRemoveRows(QModelIndex(), index, index);
    m_buses.removeAt(index);
    endRemoveRows();
}

int BusListModel::duplicateBus(int index)
{
    if (index < 0 || index >= m_buses.size())
        return -1;

    const int row = m_buses.size();
    beginInsertRows(QModelIndex(), row, row);
    BusItem copy = m_buses.at(index);
    copy.name = QStringLiteral("%1 · copy").arg(copy.name);
    // Mute is a state, not a property; routing is this bus's own wiring —
    // a copy starts live and unrouted.
    copy.muted = false;
    copy.routedAudioInputs.clear();
    copy.routedVideoSources.clear();
    m_buses.append(copy);
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
    // drop it rather than leave an invisible, unreachable routing entry
    // (setType is the only place type narrows, so this is the only place
    // that needs to enforce it). Audio sources fit EVERY bus (a video bus
    // carries the feed with its embedded audio — matches QML's
    // sourceFitsBus, the one compat rule), so only video routes are ever
    // dropped, when the type narrows to pure audio.
    QList<int> roles = { TypeRole };
    if (type != QStringLiteral("video") && type != QStringLiteral("both") && !m_buses[index].routedVideoSources.isEmpty()) {
        m_buses[index].routedVideoSources.clear();
        roles.append(RoutedVideoSourcesRole);
    }

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

    m_buses[index].muted = muted;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { MutedRole });
}

void BusListModel::toggleAudioRoute(int busIndex, int inputIndex)
{
    if (busIndex < 0 || busIndex >= m_buses.size())
        return;

    QList<int> &routes = m_buses[busIndex].routedAudioInputs;
    const int pos = routes.indexOf(inputIndex);
    if (pos >= 0)
        routes.removeAt(pos);
    else
        routes.append(inputIndex);

    const QModelIndex changed = this->index(busIndex);
    emit dataChanged(changed, changed, { RoutedAudioInputsRole });
}

void BusListModel::toggleVideoRoute(int busIndex, int sourceIndex)
{
    if (busIndex < 0 || busIndex >= m_buses.size())
        return;

    QList<int> &routes = m_buses[busIndex].routedVideoSources;
    const int pos = routes.indexOf(sourceIndex);
    if (pos >= 0) {
        routes.removeAt(pos);
    } else {
        // VIDEO IS STRICTLY 1:1 (the one contract, with QML's dropAllowed):
        // a bus renders exactly ONE source (one output frame) and a source
        // renders on exactly ONE bus. The drag path pre-checks dropAllowed
        // and refuses busy buses (red); this toggle is plain ASSIGNMENT —
        // the new source replaces whatever this bus held and is pulled from
        // any other bus holding it (the Edit dialog's checkboxes rely on
        // that). Audio toggles stay additive — mixing one input into
        // several buses is normal and frame-free.
        QList<int> pulledFrom;
        for (int b = 0; b < m_buses.size(); ++b) {
            if (b == busIndex)
                continue;
            const int p = m_buses[b].routedVideoSources.indexOf(sourceIndex);
            if (p >= 0) {
                m_buses[b].routedVideoSources.removeAt(p);
                pulledFrom.append(b);
            }
        }
        routes.clear();
        routes.append(sourceIndex);
        for (int b : pulledFrom)
            emit dataChanged(this->index(b), this->index(b), { RoutedVideoSourcesRole });
    }

    const QModelIndex changed = this->index(busIndex);
    emit dataChanged(changed, changed, { RoutedVideoSourcesRole });
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
