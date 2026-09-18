#include "VideoSourceListModel.h"

#include <algorithm>

VideoSourceListModel::VideoSourceListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // Seeded roster matching the reference image — see
    // AudioInputListModel's constructor for why this list starts non-empty.
    m_sources.append({ QStringLiteral("Cam 1"), QStringLiteral("camera"), QStringLiteral("PTZ · Wide"), false, 75 });
    m_sources.append({ QStringLiteral("Cam 2"), QStringLiteral("camera"), QStringLiteral("Fixed · Close-up"), false, 75 });
    m_sources.append({ QStringLiteral("Audience Cam"), QStringLiteral("camera"), QStringLiteral("Fixed · Wide"), false, 75 });
    m_sources.append({ QStringLiteral("Screen Capture"), QStringLiteral("screen"), QStringLiteral("HDMI in"), false, 75 });
    m_sources.append({ QStringLiteral("Video File"), QStringLiteral("media"), QStringLiteral("MP4 · 4K"), true, 60 });
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
    m_sources.append({ name.isEmpty() ? QStringLiteral("New Source %1").arg(row + 1) : name,
                       kind, sublabel, muted, level });
    endInsertRows();
}

int VideoSourceListModel::duplicateSource(int index)
{
    if (index < 0 || index >= m_sources.size())
        return -1;

    const int row = m_sources.size();
    beginInsertRows(QModelIndex(), row, row);
    VideoSourceItem copy = m_sources.at(index);
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
        { "muted", item.muted },
        { "level", item.level },
    };
}
