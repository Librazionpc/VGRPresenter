#include "VideoSourceListModel.h"

#include "BusListModel.h"

#include <algorithm>

VideoSourceListModel::VideoSourceListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // Starts EMPTY — no seeded cards. Cameras/screens come from the ENGINE's
    // video PAL (Media Foundation enumeration; the dialogs' Device selects
    // read EngineBridge.videoDevices), so a hardcoded mock roster would lie
    // about the hardware. Media rows are user-added via Add Source.
    // (AudioInputListModel keeps its seeded roster by explicit decision.)
}

VideoSourceListModel::~VideoSourceListModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

QString VideoSourceListModel::stableIdForRow(int row)
{
    if (!s_instance || row < 0 || row >= s_instance->m_sources.size())
        return {};
    return s_instance->m_sources.at(row).id;
}

int VideoSourceListModel::rowForStableId(const QString &id)
{
    if (!s_instance || id.isEmpty())
        return -1;
    for (int i = 0; i < s_instance->m_sources.size(); ++i)
        if (s_instance->m_sources.at(i).id == id)
            return i;
    return -1;   // removed row — callers treat as "no such route"
}

int VideoSourceListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_sources.size();
}

QVariant VideoSourceListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_sources.size())
        return {};

    const VideoSourceItem &item = m_sources.at(index.row());
    switch (role) {
    case NameRole: return item.name;
    case KindRole: return item.kind;
    case SublabelRole: return item.sublabel;
    case ModeRole: return item.mode;
    case MutedRole: return item.muted;
    case LevelRole: return item.level;
    default: return {};
    }
}

QHash<int, QByteArray> VideoSourceListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { KindRole, "kind" },
        { SublabelRole, "sublabel" },
        { ModeRole, "mode" },
        { MutedRole, "muted" },
        { LevelRole, "level" },
    };
}

void VideoSourceListModel::addSource()
{
    addSourceWith(QStringLiteral(""), QStringLiteral("camera"), QStringLiteral(""), 75, false);
}

void VideoSourceListModel::addSourceWith(const QString &name, const QString &kind,
                                         const QString &sublabel, qreal level, bool muted)
{
    const int row = m_sources.size();
    beginInsertRows(QModelIndex(), row, row);
    VideoSourceItem added;
    added.id = QStringLiteral("v%1").arg(m_nextId++);
    added.name = name.isEmpty() ? QStringLiteral("New Source %1").arg(row + 1) : name;
    added.kind = kind;
    added.sublabel = sublabel;
    added.mode = QString();
    added.muted = muted;
    added.level = level;
    m_sources.append(added);
    endInsertRows();
}

int VideoSourceListModel::duplicateSource(int index)
{
    if (index < 0 || index >= m_sources.size())
        return -1;

    const int row = m_sources.size();
    beginInsertRows(QModelIndex(), row, row);
    VideoSourceItem copy = m_sources.at(index);
    copy.id = QStringLiteral("v%1").arg(m_nextId++);   // fresh identity — routing does NOT copy
    copy.name = QStringLiteral("%1 · copy").arg(copy.name);
    // Mute is a state, not a property of the source — a copy starts live
    // (same rule as OutputListModel's duplicates).
    copy.muted = false;
    m_sources.append(copy);
    endInsertRows();
    return row;
}

void VideoSourceListModel::removeSource(int index)
{
    if (index < 0 || index >= m_sources.size())
        return;

    // Cut this source's routing edges BEFORE the row disappears: routes key
    // on the row's stable id, so nothing shifts or goes stale — other rows'
    // routes are untouched by construction.
    const QString id = m_sources.at(index).id;
    if (BusListModel *buses = BusListModel::Instance())
        buses->cutVideoSourceEdges(id);

    beginRemoveRows(QModelIndex(), index, index);
    m_sources.removeAt(index);
    endRemoveRows();
}

void VideoSourceListModel::renameSource(int index, const QString &name)
{
    if (index < 0 || index >= m_sources.size())
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_sources[index].name == trimmed)
        return;

    m_sources[index].name = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole });
}

void VideoSourceListModel::setKind(int index, const QString &kind)
{
    if (index < 0 || index >= m_sources.size())
        return;
    if (m_sources[index].kind == kind)
        return;

    m_sources[index].kind = kind;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { KindRole });
}

void VideoSourceListModel::setSublabel(int index, const QString &sublabel)
{
    if (index < 0 || index >= m_sources.size())
        return;
    if (m_sources[index].sublabel == sublabel)
        return;

    m_sources[index].sublabel = sublabel;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { SublabelRole });
}

void VideoSourceListModel::setMode(int index, const QString &mode)
{
    if (index < 0 || index >= m_sources.size())
        return;
    if (m_sources[index].mode == mode)
        return;

    m_sources[index].mode = mode;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ModeRole });
}

void VideoSourceListModel::setMuted(int index, bool muted)
{
    if (index < 0 || index >= m_sources.size())
        return;
    if (m_sources[index].muted == muted)
        return;

    m_sources[index].muted = muted;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { MutedRole });
}

void VideoSourceListModel::setLevel(int index, qreal level)
{
    if (index < 0 || index >= m_sources.size())
        return;
    // Meaningful only for media rows (they carry audio) — but storing the
    // value anyway is harmless and keeps the field honest if a row's kind
    // is switched to media later.
    const qreal clamped = std::clamp(level, 0.0, 100.0);
    if (qFuzzyCompare(m_sources[index].level, clamped))
        return;

    m_sources[index].level = clamped;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { LevelRole });
}

QVariantMap VideoSourceListModel::getSource(int index) const
{
    if (index < 0 || index >= m_sources.size())
        return {};

    const VideoSourceItem &item = m_sources.at(index);
    return {
        { "name", item.name },
        { "kind", item.kind },
        { "sublabel", item.sublabel },
        { "mode", item.mode },
        { "muted", item.muted },
        { "level", item.level },
    };
}
