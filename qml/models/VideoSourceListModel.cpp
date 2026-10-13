#include "VideoSourceListModel.h"

#include "BusListModel.h"
#include "ChannelTrim.h"
#include "EngineBridge.h"
#include "SettingsService.h"
#include "modules/production/ProductionEngine.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJSEngine>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

#include <algorithm>

// The one instance, for C++ AND QML — see the header for why this is eager.
VideoSourceListModel *VideoSourceListModel::Instance()
{
    static VideoSourceListModel s;
    return &s;
}

VideoSourceListModel *VideoSourceListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    VideoSourceListModel *s = Instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

VideoSourceListModel::VideoSourceListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    Q_ASSERT_X(!s_instance || s_instance == this, "VideoSourceListModel",
               "a second VideoSourceListModel was constructed — Instance() "
               "would silently rebind and every C++ reader would hold the "
               "wrong one");
    s_instance = this;

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
    if (!m_active)
        return;   // a displaced instance is not the roster anyone is editing
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
        QJsonArray gains;
        for (qreal g : it.channelGains)
            gains.append(g);
        // The trims each side was closed at, beside the gains they belong to, so
        // reopening a side gives the trim back in the next session too.
        QJsonArray trims;
        for (qreal t : it.channelTrims)
            trims.append(t);
        rows.append(QJsonObject{
            { "id", it.id },
            { "name", it.name },
            { "kind", it.kind },
            { "sublabel", it.sublabel },
            { "mode", it.mode },
            { "muted", it.muted },
            { "level", it.level },
            { "channelGains", gains },
            { "channelTrims", trims },
        });
    }
    return QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact));
}

void VideoSourceListModel::queueRosterWrite()
{
    if (!m_active)
        return;
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

bool VideoSourceListModel::restoreDone()
{
    return s_instance && s_instance->m_restoreDone;
}

int VideoSourceListModel::restoreRoster()
{
    SettingsService *settings = SettingsService::instancePtr();
    if (!settings || !EngineBridge::instance().booted())
        return 0;
    // The store has ANSWERED (see AudioInputListModel's twin): BusListModel's
    // orphan sweep may now treat these rows as the authority for which
    // "vsrc:<id>" nodes are still live. Set before the blob checks on purpose —
    // "nothing saved" is an answer too.
    m_restoreDone = true;
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
    QSet<QString> assigned;
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
        // Per-side trims, same rule as AudioInputListModel's restore: a missing
        // or short list means the sides it doesn't cover were never trimmed
        // (unity) — so a roster from before these existed restores open.
        for (const QJsonValue &gv : o.value(QStringLiteral("channelGains")).toArray())
            it.channelGains.append(std::clamp(gv.toDouble(1.0), 0.0, 1.0));
        // Rows saved before the trims existed have none: channelTrim() then
        // falls back to the level the side holds open (unity beyond that), so an
        // older roster cannot reopen a side at 0.
        for (const QJsonValue &tv : o.value(QStringLiteral("channelTrims")).toArray())
            it.channelTrims.append(std::clamp(tv.toDouble(0.0), 0.0, 1.0));
        // Ids: keep the saved one (bus routing keys on it); renumber only
        // broken/missing entries — picking from the ids is not already taken in
        // this restore, the same rule the audio roster follows, so two rows can
        // never end up sharing one node. A shared node draws one row's lines
        // under the other's card and cuts the wrong edge when a row goes.
        if (it.id.isEmpty() || assigned.contains(it.id)) {
            while (assigned.contains(QStringLiteral("v%1").arg(nextId)))
                ++nextId;
            it.id = QStringLiteral("v%1").arg(nextId);
        }
        assigned.insert(it.id);
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
    // Restored rows that are already routed must carry their saved fader into
    // the graph again (the node survives a restart inside the production doc;
    // its VolumeControl is whatever the last push left — or unity).
    for (int row = 0; row < m_sources.size(); ++row)
        pushAudioToGraph(row);
    return m_sources.size();
}

void VideoSourceListModel::adoptInstance(VideoSourceListModel *roster)
{
    if (!roster || roster == s_instance)
        return;
    if (s_instance)
        s_instance->deactivate();
    s_instance = roster;
}

void VideoSourceListModel::deactivate()
{
    m_active = false;
    if (saveTimer_) {
        saveTimer_->stop();
        delete saveTimer_;
        saveTimer_ = nullptr;
    }
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

QString VideoSourceListModel::idForRow(int row)
{
    VideoSourceListModel *self = s_instance ? s_instance : Instance();
    if (row < 0 || row >= self->m_sources.size())
        return {};
    QString &id = self->m_sources[row].id;
    if (id.isEmpty()) {
        id = QStringLiteral("v%1").arg(self->m_nextId++);
        self->scheduleSave();   // the healed id has to survive the session
    }
    return id;
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
    case ChannelGainsRole: {
        QVariantList gains;
        for (qreal g : item.channelGains)
            gains.append(g);
        return gains;
    }
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
        { ChannelGainsRole, "channelGains" },
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
    //
    // Through the HEALING accessor, like the audio roster: an id-less row would
    // make the cut a silent no-op, leaving its edges (and the bus's line to it)
    // in the graph behind a row that is gone.
    const QString id = idForRow(index);
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

QString VideoSourceListModel::pickVideoFile() const
{
    if (!EngineBridge::instance().booted())
        return {};
    auto r = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        "Pick a video file",
        // Video containers only — the source is opened as a video and refuses
        // anything else, so the picker is where that decision belongs.
        { "Video files (*.mp4 *.mov *.mkv *.avi *.webm *.m4v)" });
    return r.ok() && r.value() ? QString::fromStdString(*r.value()) : QString();
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
    pushAudioToGraph(index);   // mute IS the node's VolumeControl::mute
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
    pushAudioToGraph(index);   // the fader IS the graph's gainDb
}

// One side of the row's audio trim — the video card's L/R pill, and the same
// operation AudioInputListModel::setChannelGain performs on an input row:
// gain 0 IS "this side is closed" (there is no separate enable bit anywhere
// in the app). Deliberately NOT the row's `muted`: that flag closes the whole
// row, which is why clicking one pill used to darken both.
void VideoSourceListModel::setChannelGain(int index, int channel, qreal gain)
{
    if (index < 0 || index >= m_sources.size() || channel < 0 || channel >= 2)
        return;
    const qreal clamped = std::clamp(gain, 0.0, 1.0);
    QList<qreal> &gains = m_sources[index].channelGains;
    while (gains.size() <= channel)
        gains.append(1.0);   // absent = unity
    if (qFuzzyCompare(gains[channel], clamped))
        return;
    gains[channel] = clamped;
    // Remember an OPEN level as this side's trim on the way past, so the pill
    // that closed it can hand it back (models/ChannelTrim.h). A close (gain 0)
    // records nothing — that would arm the next reopen with silence.
    if (channeltrim::remembersTrim(clamped))
        writeChannelTrim(index, channel, clamped);
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ChannelGainsRole });
    scheduleSave();
    pushAudioToGraph(index);   // the per-side trim rides the node's meta
}

void VideoSourceListModel::toggleChannelGain(int index, int channel)
{
    // One policy for the app, one call here (see BusListModel's twin).
    if (channeltrim::isOff(channelGain(index, channel)))
        setChannelGain(index, channel, channelTrim(index, channel));
    else
        setChannelGain(index, channel, channeltrim::closed());
}

qreal VideoSourceListModel::channelTrim(int index, int channel) const
{
    if (index < 0 || index >= m_sources.size() || channel < 0)
        return channeltrim::unity();
    const VideoSourceItem &item = m_sources.at(index);
    const qreal remembered = channel < item.channelTrims.size()
        ? item.channelTrims.at(channel) : channeltrim::closed();
    return channeltrim::reopenGain(remembered, channelGain(index, channel));
}

void VideoSourceListModel::setChannelTrim(int index, int channel, qreal trim)
{
    if (!writeChannelTrim(index, channel, std::clamp(trim, 0.0, 1.0)))
        return;
    scheduleSave();
}

bool VideoSourceListModel::writeChannelTrim(int index, int channel, qreal trim)
{
    if (index < 0 || index >= m_sources.size() || channel < 0 || channel >= 2)
        return false;
    QList<qreal> &trims = m_sources[index].channelTrims;
    while (trims.size() <= channel)
        trims.append(channeltrim::closed());   // absent = nothing remembered
    if (qFuzzyCompare(trims[channel], trim))
        return false;
    trims[channel] = trim;
    return true;
}

// ---------------------------------------------------------------------------
// REAL PUSH — see the header. Mirrors AudioInputListModel::pushEffectsToGraph's
// contract exactly (a no-op when the row is unrouted or the engine is not up)
// so the two roster models can't drift on what "the row's audio state" means to
// the engine.
void VideoSourceListModel::pushAudioToGraph(int index)
{
    if (!m_active)
        return;   // never push a displaced roster's view onto the engine
    if (index < 0 || index >= m_sources.size())
        return;
    if (!EngineBridge::instance().booted())
        return;   // graph not up yet — the next write after boot pushes again

    const QString nodeQ = BusListModel::videoSourceNodeId(index);
    if (nodeQ.isEmpty())
        return;
    const std::string node = nodeQ.toStdString();

    auto &graph = bps::production::ProductionEngine::Instance().Graph();
    // Unrouted row: its node is never created (nodes appear with the first
    // route — BusListModel::toggleVideoRoute) — nothing to push.
    if (!graph.HasNode(node))
        return;

    const VideoSourceItem &item = m_sources.at(index);
    // One edit window around the whole push (VolumeControl + the trims): every
    // graph mutator serializes and flushes the production doc, so without it
    // this wrote the document three-plus times per fader tick.
    const bool ownsEdit = graph.BeginEdit().ok();

    // VolumeControl: the row's fader + mute, keeping anything else the node
    // already carries (the bus plane's own controls are separate nodes).
    bps::production::VolumeControl vol;
    if (auto cur = graph.Volume(node); cur.ok())
        vol = cur.value();
    // UI 0..100 → engine gain, the SAME mapping the audio roster and a bus
    // fader use (0 = −60 dB silence, 100 = unity).
    vol.gainDb = -60.0 + (item.level / 100.0) * 60.0;
    vol.mute = item.muted;
    (void)graph.SetVolume(node, vol);

    // The per-side trims, on the same meta key the mixer's SumSource reads.
    for (int channel = 0; channel < 8; ++channel) {
        const qreal gain = channel < item.channelGains.size()
            ? item.channelGains.at(channel) : 1.0;
        (void)graph.SetMeta(node, "uiChannelGain" + std::to_string(channel),
                            std::to_string(gain));
    }
    if (ownsEdit)
        (void)graph.CommitEdit();
}

qreal VideoSourceListModel::channelGain(int index, int channel) const
{
    if (index < 0 || index >= m_sources.size() || channel < 0)
        return 1.0;
    const QList<qreal> &gains = m_sources[index].channelGains;
    return channel < gains.size() ? gains.at(channel) : 1.0;
}

QVariantMap VideoSourceListModel::getSource(int index) const
{
    if (index < 0 || index >= m_sources.size())
        return {};

    const VideoSourceItem &item = m_sources.at(index);
    QVariantList gainList;
    for (qreal g : item.channelGains)
        gainList.append(g);
    QVariantList trimList;
    for (qreal t : item.channelTrims)
        trimList.append(t);
    return {
        { "name", item.name },
        { "kind", item.kind },
        { "sublabel", item.sublabel },
        { "mode", item.mode },
        { "muted", item.muted },
        { "level", item.level },
        { "channelGains", gainList },
        { "channelTrims", trimList },
    };
}
