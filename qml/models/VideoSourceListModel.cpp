#include "VideoSourceListModel.h"

#include "BusListModel.h"
#include "EngineBridge.h"
#include "SettingsService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <algorithm>

VideoSourceListModel::VideoSourceListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // Starts EMPTY — no seeded cards. Cameras/screens come from the ENGINE's
    // video PAL (Media Foundation enumeration; the dialogs' Device selects
    // read EngineBridge.videoDevices), so a hardcoded mock roster would lie
    // about the hardware. Rows are user-added via Add Source and PERSISTED
    // (session.videoRoster — the audio board's contract; a camera the user
    // set up must not vanish on every launch).
    //
    // Restore must wait for the OTHER QML singletons (SettingsService may
    // not exist when this model is built — singleton order follows QML
    // import order), so the restore retries on the event loop until the
    // store is reachable. Every attempt no-ops once rows exist, so a late
    // retry can never clobber rows the user already added.
    for (int attempt = 0; attempt < 20; ++attempt) {
        QTimer::singleShot(attempt == 0 ? 0 : 100, this, [this]() {
            if (m_sources.isEmpty())
                restoreRoster();
        });
    }
}

VideoSourceListModel::~VideoSourceListModel()
{
    // Flush any debounced save before dying — the last edit must not be
    // lost to the 400 ms window (writes are cheap; losing them isn't).
    if (saveTimer_ && saveTimer_->isActive()) {
        saveTimer_->stop();
        queueRosterWrite();
    }
    if (s_instance == this)
        s_instance = nullptr;
}

// ---- Roster persistence (session.videoRoster via the settings store) ------

void VideoSourceListModel::scheduleSave()
{
    if (!saveTimer_) {
        saveTimer_ = new QTimer(this);
        saveTimer_->setSingleShot(true);
        saveTimer_->setInterval(400);
        connect(saveTimer_, &QTimer::timeout, this, [this]() { queueRosterWrite(); });
    }
    saveTimer_->start();
}

QString VideoSourceListModel::serializeRoster() const
{
    QJsonArray rows;
    for (const VideoSourceItem &it : m_sources) {
        rows.append(QJsonObject{
            { "id", it.id },
            { "name", it.name },
            { "kind", it.kind },
            { "sublabel", it.sublabel },
            { "mode", it.mode },
            { "muted", it.muted },
            { "level", it.level },
        });
    }
    return QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact));
}

void VideoSourceListModel::queueRosterWrite()
{
    SettingsService *settings = SettingsService::instancePtr();
    if (!settings || !EngineBridge::instance().booted())
        return;   // engine gone (shutdown) or settings not ready — nothing to write to
    settings->setValue(QStringLiteral("session.videoRoster"), serializeRoster());
}

void VideoSourceListModel::saveRosterNow()
{
    if (saveTimer_)
        saveTimer_->stop();
    queueRosterWrite();
}

int VideoSourceListModel::restoreRoster()
{
    SettingsService *settings = SettingsService::instancePtr();
    if (!settings || !EngineBridge::instance().booted())
        return 0;
    const QString blob = settings->value(QStringLiteral("session.videoRoster")).toString();
    if (blob.isEmpty())
        return 0;
    const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8());
    if (!doc.isArray())
        return 0;

    const QJsonArray rows = doc.array();
    if (rows.isEmpty())
        return 0;
    beginResetModel();
    m_sources.clear();
    int nextId = 1;
    for (const QJsonValue &rv : rows) {
        const QJsonObject o = rv.toObject();
        VideoSourceItem it;
        it.id = o.value(QStringLiteral("id")).toString();
        it.name = o.value(QStringLiteral("name")).toString();
        it.kind = o.value(QStringLiteral("kind")).toString(QStringLiteral("camera"));
        it.sublabel = o.value(QStringLiteral("sublabel")).toString();
        it.mode = o.value(QStringLiteral("mode")).toString();
        it.muted = o.value(QStringLiteral("muted")).toBool();
        it.level = std::clamp(o.value(QStringLiteral("level")).toDouble(0.0), 0.0, 100.0);
        // Ids: keep the saved one (bus routing keys on it); renumber only
        // broken/missing entries.
        if (it.id.isEmpty())
            it.id = QStringLiteral("v%1").arg(nextId);
        bool okNum = false;
        const int num = it.id.mid(1).toInt(&okNum);
        if (okNum)
            nextId = std::max(nextId, num + 1);
        m_sources.append(it);
    }
    m_nextId = std::max(m_nextId, nextId);
    endResetModel();

    // The buses' cached routes may reference restored rows by id — re-derive.
    if (BusListModel *buses = BusListModel::Instance())
        buses->refreshRoutes();
    return m_sources.size();
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
    scheduleSave();
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
    scheduleSave();
    return row;
}

void VideoSourceListModel::removeSource(int index)
{
    if (index < 0 || index >= m_sources.size())
        return;

    // Cut this source's routing edges BEFORE the row disappears: routes key
    // on the row's stable id, so no OTHER row's route is touched.
    const QString id = m_sources.at(index).id;
    BusListModel *buses = BusListModel::Instance();
    if (buses)
        buses->cutVideoSourceEdges(id);

    beginRemoveRows(QModelIndex(), index, index);
    m_sources.removeAt(index);
    endRemoveRows();
    scheduleSave();

    // Rows after `index` just shifted up; the buses cache routes as ROWS, so
    // re-derive them (the graph edges themselves are untouched).
    if (buses)
        buses->refreshRoutes();
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
    scheduleSave();
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
    scheduleSave();
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
    scheduleSave();
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
    scheduleSave();
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
    scheduleSave();
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
    scheduleSave();
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
