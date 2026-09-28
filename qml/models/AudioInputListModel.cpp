#include "AudioInputListModel.h"

#include "BusListModel.h"
#include "services/EngineBridge.h"
#include "services/SettingsService.h"
#include "platform/PlatformAccessor.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/production/ProductionTypes.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

static QVariantList routingVariant(const QList<QList<int>> &routes)
{
    QVariantList perChannel;
    for (const QList<int> &route : routes) {
        QVariantList cell;
        for (int bus : route) cell.append(bus);
        perChannel.append(cell);
    }
    return perChannel;
}

QList<AudioEffect> AudioInputListModel::defaultEffects()
{
    // Defaults matching the reference rack
    // (VGRPresenter_Settings_Audio_Video_Add_Compressor.qml): Gain / EQ /
    // Compressor / Reverb enabled, Limiter / Noise Gate / Delay off.
    // Order is the rack's visual order.
    return {
        { QStringLiteral("gain"),        QStringLiteral("Gain"),        true,  6,   -24, 24,  QStringLiteral(" dB") },
        { QStringLiteral("eq"),          QStringLiteral("Equalizer"),   true,  3,   1,   5,   QStringLiteral("-band") },
        { QStringLiteral("compressor"),  QStringLiteral("Compressor"),  true,  3,   1,   10,  QStringLiteral(":1") },
        { QStringLiteral("reverb"),      QStringLiteral("Reverb"),      true,  24,  0,   100, QStringLiteral("%") },
        { QStringLiteral("limiter"),     QStringLiteral("Limiter"),     false, -1,  -24, 0,   QStringLiteral(" dB") },
        { QStringLiteral("noiseGate"),   QStringLiteral("Noise Gate"),  false, -50, -80, 0,   QStringLiteral(" dB") },
        { QStringLiteral("delay"),       QStringLiteral("Delay"),       false, 120, 0,   500, QStringLiteral(" ms") },
    };
}

static QVariantMap effectToVariant(const AudioEffect &effect)
{
    return {
        { "key", effect.key },
        { "label", effect.label },
        { "enabled", effect.enabled },
        { "value", effect.value },
        { "minValue", effect.minValue },
        { "maxValue", effect.maxValue },
        { "suffix", effect.suffix },
    };
}

AudioInputListModel::AudioInputListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    s_instance = this;
    // Starts EMPTY: the board shows the machine's real audio devices (via
    // EngineBridge.audioDevices), not a hardcoded demo roster — every source
    // on the board is one the user added. (The old seed rows were mock data;
    // with the engine PAL now enumerating real hardware they were removed.)
    //
    // Routes key on each row's STABLE id (asrc:<id> graph nodes), never its row.
    // BusListModel does CACHE routes as roster rows for QML, so a removal
    // (which shifts rows) ends with BusListModel::refreshRoutes() — see
    // removeInput. The roster (rows + settings + matrix) persists via
    // session.audioRoster; board routing is still re-pruned at startup
    // (BusListModel prunes every asrc:/vsrc: node — unchanged by design).
    //
    // Restore must wait for the OTHER QML singletons: singleton construction
    // order follows QML import order, so SettingsService may not exist when
    // this model is built — restoreRoster would no-op and the board would
    // start empty for the whole session. Retry on the event loop (0ms, then
    // every 100ms up to ~2s) until the store is reachable; every attempt is
    // a no-op once the roster is non-empty, so a late retry can never
    // clobber rows the user already added.
    for (int attempt = 0; attempt < 20; ++attempt) {
        QTimer::singleShot(attempt == 0 ? 0 : 100, this, [this]() {
            if (m_inputs.isEmpty())
                restoreRoster();
        });
    }
}

AudioInputListModel::~AudioInputListModel()
{
    // Flush any debounced save before dying — the last fader move must not
    // be lost to the 400 ms window (writes are cheap; losing them isn't).
    if (saveTimer_ && saveTimer_->isActive()) {
        saveTimer_->stop();
        queueRosterWrite();
    }
    if (s_instance == this)
        s_instance = nullptr;
}

QString AudioInputListModel::stableIdForRow(int row)
{
    if (!s_instance || row < 0 || row >= s_instance->m_inputs.size())
        return {};
    return s_instance->m_inputs.at(row).id;
}

int AudioInputListModel::rowForStableId(const QString &id)
{
    if (!s_instance || id.isEmpty())
        return -1;
    for (int i = 0; i < s_instance->m_inputs.size(); ++i)
        if (s_instance->m_inputs.at(i).id == id)
            return i;
    return -1;   // removed row — callers treat as "no such route"
}

int AudioInputListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_inputs.size();
}

QVariant AudioInputListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_inputs.size())
        return {};

    const AudioInputItem &item = m_inputs.at(index.row());
    switch (role) {
    case NameRole: return item.name;
    case KindRole: return item.kind;
    case SublabelRole: return item.sublabel;
    case LevelRole: return item.level;
    case MutedRole: return item.muted;
    case DelayMsRole: return item.delayMs;
    case ChannelsRole: return item.channels;
    case ChannelGainsRole: {
        QVariantList gains;
        for (qreal g : item.channelGains) gains.append(g);
        return gains;
    }
    case RoutingAutoRole: return item.routingAuto;
    case RoutingRole: {
        QVariantList perChannel;
        for (const QList<int> &route : item.channelRoutes) {
            QVariantList cell;
            for (int bus : route) cell.append(bus);
            perChannel.append(cell);
        }
        return perChannel;
    }
    case EffectsRole: {
        QVariantList list;
        for (const AudioEffect &effect : item.effects)
            list.append(effectToVariant(effect));
        return list;
    }
    default: return {};
    }
}

QHash<int, QByteArray> AudioInputListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { KindRole, "kind" },
        { SublabelRole, "sublabel" },
        { LevelRole, "level" },
        { MutedRole, "muted" },
        { DelayMsRole, "delayMs" },
        { ChannelsRole, "channels" },
        { ChannelGainsRole, "channelGains" },
        { RoutingAutoRole, "routingAuto" },
        { RoutingRole, "routing" },
        { EffectsRole, "effects" },
    };
}

void AudioInputListModel::addInput()
{
    const int row = m_inputs.size();
    beginInsertRows(QModelIndex(), row, row);
    AudioInputItem added;
    added.id = QStringLiteral("a%1").arg(m_nextId++);
    added.name = QStringLiteral("New Input %1").arg(row + 1);
    added.effects = defaultEffects();
    m_inputs.append(added);
    endInsertRows();
    scheduleSave();
}

void AudioInputListModel::removeInput(int index)
{
    if (index < 0 || index >= m_inputs.size())
        return;

    // Cut this source's routing edges BEFORE the row disappears: routes key
    // on the row's stable id, so no OTHER row's route is touched.
    const QString id = m_inputs.at(index).id;
    BusListModel *buses = BusListModel::Instance();
    if (buses)
        buses->cutAudioSourceEdges(id);

    beginRemoveRows(QModelIndex(), index, index);
    m_inputs.removeAt(index);
    endRemoveRows();
    scheduleSave();

    // Rows after `index` just shifted up; the buses cache routes as ROWS, so
    // re-derive them (the graph edges themselves are untouched).
    if (buses)
        buses->refreshRoutes();
}

int AudioInputListModel::duplicateInput(int index)
{
    if (index < 0 || index >= m_inputs.size())
        return -1;

    const int row = m_inputs.size();
    beginInsertRows(QModelIndex(), row, row);
    AudioInputItem copy = m_inputs.at(index);
    copy.id = QStringLiteral("a%1").arg(m_nextId++);   // fresh identity — routing does NOT copy
    copy.name = QStringLiteral("%1 · copy").arg(copy.name);
    // Mute is a state, not a property of the source — a copy starts live
    // (same rule as OutputListModel's duplicates).
    copy.muted = false;
    // Per-channel bus routing is NOT copied either — the copy's graph routes start
    // empty, so a copied matrix would claim routes the board doesn't have.
    copy.channelRoutes.clear();
    m_inputs.append(copy);
    endInsertRows();
    scheduleSave();
    return row;
}

void AudioInputListModel::renameInput(int index, const QString &name)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_inputs[index].name == trimmed)
        return;

    m_inputs[index].name = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole });
    scheduleSave();
}

void AudioInputListModel::setKind(int index, const QString &kind)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    if (m_inputs[index].kind == kind)
        return;

    m_inputs[index].kind = kind;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { KindRole });
    scheduleSave();
}

void AudioInputListModel::setSublabel(int index, const QString &sublabel)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    if (m_inputs[index].sublabel == sublabel)
        return;

    m_inputs[index].sublabel = sublabel;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { SublabelRole });
    scheduleSave();
}

QString AudioInputListModel::pickAudioFile() const
{
    if (!EngineBridge::instance().booted())
        return {};
    auto r = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        "Pick an audio file",
        { "Audio files (*.mp3 *.wav *.flac *.m4a *.aac *.ogg)", "All files (*.*)" });
    return r.ok() && r.value() ? QString::fromStdString(*r.value()) : QString();
}

void AudioInputListModel::setLevel(int index, qreal level)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    const qreal clamped = std::clamp(level, 0.0, 100.0);
    if (qFuzzyCompare(m_inputs[index].level, clamped))
        return;

    m_inputs[index].level = clamped;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { LevelRole });
    scheduleSave();
    pushEffectsToGraph(index);   // the fader IS the graph's gainDb
}

void AudioInputListModel::setMuted(int index, bool muted)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    if (m_inputs[index].muted == muted)
        return;

    m_inputs[index].muted = muted;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { MutedRole });
    scheduleSave();
    pushEffectsToGraph(index);   // mute is graph VolumeControl::mute
}

// ---------------------------------------------------------------------------
// REAL DSP PUSH — the effects rack onto the production graph (docs/specs/27
// §Processing chains). The rack stops being decoration: each row's enabled
// effects become ProcessingStages on its "asrc:<id>" SOURCE NODE and its
// level/mute land in the node's VolumeControl — the SAME nodes the routing
// board's buses patch from, so anything downstream of the source hears the
// rack. No-op when the row is unrouted (no node = nothing to process) or the
// engine is not booted. Values map: UI dB stay dB, ratios (:1) and percents
// fold into the stage's 0..1 amount, delay ms into seconds.
void AudioInputListModel::pushEffectsToGraph(int index)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    if (!EngineBridge::instance().booted())
        return;   // graph not up yet — the next write after boot pushes again

    const QString nodeQ = BusListModel::audioSourceNodeId(index);
    if (nodeQ.isEmpty())
        return;
    const std::string node = nodeQ.toStdString();

    auto &graph = bps::production::ProductionEngine::Instance().Graph();
    // Unrouted source: its node is never created (nodes appear with the first
    // route, BusListModel::toggleAudioRoute) — nothing to push.
    if (!graph.HasNode(node))
        return;

    const AudioInputItem &item = m_inputs.at(index);

    // ---- VolumeControl: the row's fader + mute + per-channel gains ---------
    bps::production::VolumeControl vol;
    if (auto cur = graph.Volume(node); cur.ok())
        vol = cur.value();          // keep solo/pan; this model owns gain/mute
    // UI 0..100 → engine gain, the SAME mapping BusListModel's fader uses
    // (0 = −60 dB silence, 100 = unity). Local copy of the mapping — the
    // helper lives in BusListModel.cpp's anonymous namespace.
    vol.gainDb = -60.0 + (item.level / 100.0) * 60.0;
    vol.mute = item.muted;
    (void)graph.SetVolume(node, vol);

    // ---- Processing chain: rebuilt from the rack on every write ------------
    (void)graph.ClearProcessing(node);
    using PK = bps::production::ProcessKind;
    for (const AudioEffect &e : item.effects) {
        if (!e.enabled)
            continue;
        bps::production::ProcessingStage stage;
        if (e.key == QLatin1String("gain")) {
            // The rack's Gain is a trim in dB — the SAME domain as the fader.
            stage.kind = PK::Gain;
            stage.amount = e.value;
        } else if (e.key == QLatin1String("eq")) {
            // Bands 1..5 — the amount carries the band count; tone shaping
            // per band is a future engine refinement.
            stage.kind = PK::Eq;
            stage.amount = e.value / 5.0;
        } else if (e.key == QLatin1String("compressor")) {
            // Ratio 1..10:1 — intensity as (ratio-1)/9 so 1:1 = 0, 10:1 = 1.
            stage.kind = PK::Compressor;
            stage.amount = std::clamp((e.value - 1.0) / 9.0, 0.0, 1.0);
        } else if (e.key == QLatin1String("limiter")) {
            // Ceiling -24..0 dBFS.
            stage.kind = PK::Limiter;
            stage.amount = std::clamp((e.value + 24.0) / 24.0, 0.0, 1.0);
        } else if (e.key == QLatin1String("noiseGate")) {
            // Threshold -80..0 dBFS.
            stage.kind = PK::Gate;
            stage.amount = std::clamp((e.value + 80.0) / 80.0, 0.0, 1.0);
        } else if (e.key == QLatin1String("delay")) {
            // 0..500 ms → the stage's seconds domain.
            stage.kind = PK::Delay;
            stage.amount = e.value / 1000.0;
        } else if (e.key == QLatin1String("reverb")) {
            // No reverb stage in the engine's ProcessKind set — carried as a
            // named Custom stage so the intent survives the graph (a future
            // reverb stage claims it without a model change).
            stage.kind = PK::Custom;
            stage.custom = "reverb";
            stage.amount = std::clamp(e.value / 100.0, 0.0, 1.0);
        } else {
            continue;
        }
        (void)graph.AddProcessing(node, stage);
    }
}

void AudioInputListModel::setDelayMs(int index, int delayMs)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    const int clamped = std::clamp(delayMs, 0, 1000);
    if (m_inputs[index].delayMs == clamped)
        return;

    m_inputs[index].delayMs = clamped;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { DelayMsRole });
    scheduleSave();
}

void AudioInputListModel::setChannels(int index, int channels)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    const int clamped = std::clamp(channels, 1, 8);
    if (m_inputs[index].channels == clamped)
        return;

    m_inputs[index].channels = clamped;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ChannelsRole });
    scheduleSave();
}

void AudioInputListModel::setChannelGain(int index, int channel, qreal gain)
{
    if (index < 0 || index >= m_inputs.size() || channel < 0 || channel >= 8)
        return;
    const qreal clamped = std::clamp(gain, 0.0, 1.0);
    QList<qreal> &gains = m_inputs[index].channelGains;
    while (gains.size() <= channel)
        gains.append(1.0);   // absent = unity
    if (qFuzzyCompare(gains[channel], clamped))
        return;
    gains[channel] = clamped;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ChannelGainsRole });
    scheduleSave();   // debounced — a dragged knob coalesces into one write
    pushEffectsToGraph(index);   // per-channel trim rides the node volume
}

qreal AudioInputListModel::channelGain(int index, int channel) const
{
    if (index < 0 || index >= m_inputs.size() || channel < 0)
        return 1.0;
    const QList<qreal> &gains = m_inputs[index].channelGains;
    return channel < gains.size() ? gains.at(channel) : 1.0;
}

// ---- Roster persistence (session.audioRoster via the settings store) ------

void AudioInputListModel::scheduleSave()
{
    if (!saveTimer_) {
        saveTimer_ = new QTimer(this);
        saveTimer_->setSingleShot(true);
        saveTimer_->setInterval(400);
        connect(saveTimer_, &QTimer::timeout, this, [this]() { queueRosterWrite(); });
    }
    saveTimer_->start();
}

QString AudioInputListModel::serializeRoster() const
{
    QJsonArray rows;
    for (const AudioInputItem &it : m_inputs) {
        QJsonArray gains;
        for (qreal g : it.channelGains) gains.append(g);
        QJsonArray routes;
        for (const QList<int> &cell : it.channelRoutes) {
            QJsonArray buses;
            for (int b : cell) buses.append(b);
            routes.append(buses);
        }
        QJsonArray effects;
        for (const AudioEffect &e : it.effects) {
            effects.append(QJsonObject{
                { "key", e.key }, { "enabled", e.enabled }, { "value", e.value },
            });
        }
        rows.append(QJsonObject{
            { "id", it.id },
            { "name", it.name },
            { "kind", it.kind },
            { "sublabel", it.sublabel },
            { "level", it.level },
            { "muted", it.muted },
            { "delayMs", it.delayMs },
            { "channels", it.channels },
            { "gains", gains },
            { "routingAuto", it.routingAuto },
            { "routes", routes },
            { "effects", effects },
        });
    }
    return QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact));
}

void AudioInputListModel::queueRosterWrite()
{
    SettingsService *settings = SettingsService::instancePtr();
    if (!settings || !EngineBridge::instance().booted())
        return;   // engine gone (shutdown) or settings not ready — nothing to write to
    settings->setValue(QStringLiteral("session.audioRoster"), serializeRoster());
}

void AudioInputListModel::saveRoster()
{
    if (saveTimer_)
        saveTimer_->stop();
    queueRosterWrite();
}

int AudioInputListModel::restoreRoster()
{
    SettingsService *settings = SettingsService::instancePtr();
    if (!settings || !EngineBridge::instance().booted())
        return 0;
    const QString blob = settings->value(QStringLiteral("session.audioRoster")).toString();
    if (blob.isEmpty())
        return 0;
    const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8());
    if (!doc.isArray())
        return 0;

    const QJsonArray rows = doc.array();
    if (rows.isEmpty())
        return 0;
    beginResetModel();
    m_inputs.clear();
    int nextId = 1;
    for (const QJsonValue &rv : rows) {
        const QJsonObject o = rv.toObject();
        AudioInputItem it;
        it.id = o.value(QStringLiteral("id")).toString();
        it.name = o.value(QStringLiteral("name")).toString();
        it.kind = o.value(QStringLiteral("kind")).toString(QStringLiteral("device"));
        it.sublabel = o.value(QStringLiteral("sublabel")).toString();
        it.level = o.value(QStringLiteral("level")).toDouble();
        it.muted = o.value(QStringLiteral("muted")).toBool();
        it.delayMs = std::clamp(o.value(QStringLiteral("delayMs")).toInt(), 0, 1000);
        it.channels = std::clamp(o.value(QStringLiteral("channels")).toInt(2), 1, 8);
        for (const QJsonValue &g : o.value(QStringLiteral("gains")).toArray())
            it.channelGains.append(std::clamp(g.toDouble(1.0), 0.0, 1.0));
        it.routingAuto = o.value(QStringLiteral("routingAuto")).toBool();
        for (const QJsonValue &cell : o.value(QStringLiteral("routes")).toArray()) {
            QList<int> buses;
            for (const QJsonValue &b : cell.toArray()) buses.append(b.toInt());
            it.channelRoutes.append(buses);
        }
        // Effects: enable/value restored over the DEFAULT template, so a
        // future effect added in code keeps its range/label metadata.
        it.effects = defaultEffects();
        const QJsonObject savedFx = o.value(QStringLiteral("effects")).toObject();
        for (AudioEffect &e : it.effects) {
            if (savedFx.contains(e.key)) {
                const QJsonObject se = savedFx.value(e.key).toObject();
                e.enabled = se.value(QStringLiteral("enabled")).toBool(e.enabled);
                e.value = std::clamp(se.value(QStringLiteral("value")).toDouble(e.value),
                                     e.minValue, e.maxValue);
            }
        }
            // Ids: keep the saved one; renumber only broken/missing entries.
        if (it.id.isEmpty())
            it.id = QStringLiteral("a%1").arg(m_nextId);
        bool okNum = false;
        const int num = it.id.mid(1).toInt(&okNum);
        if (okNum)
            nextId = std::max(nextId, num + 1);
        m_inputs.append(it);
    }
    m_nextId = std::max(m_nextId, nextId);
    endResetModel();

    // The buses' cached routes may reference restored rows by id — re-derive.
    if (BusListModel *buses = BusListModel::Instance())
        buses->refreshRoutes();
    return m_inputs.size();
}

QVariantMap AudioInputListModel::getRouting(int index) const
{
    if (index < 0 || index >= m_inputs.size())
        return {};

    const AudioInputItem &item = m_inputs.at(index);
    QVariantList perChannel;
    for (const QList<int> &route : item.channelRoutes) {
        QVariantList cell;
        // Stored values are stable BUS NUMBERS; QML consumes rows —
        // translate here. A number whose bus row is gone drops out.
        for (int busNo : route) {
            const int busRow = BusListModel::rowForBusNumber(busNo);
            if (busRow >= 0)
                cell.append(busRow);
        }
        perChannel.append(cell);
    }
    return {
        { "auto", item.routingAuto },
        { "channels", item.channels },
        { "routes", perChannel },
    };
}

void AudioInputListModel::setRoutingAuto(int index, bool auto_)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    if (m_inputs[index].routingAuto == auto_)
        return;

    m_inputs[index].routingAuto = auto_;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { RoutingAutoRole });
    scheduleSave();
}

void AudioInputListModel::toggleChannelRoute(int index, int channel, int busIndex)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    AudioInputItem &item = m_inputs[index];
    if (channel < 0 || channel >= item.channels || busIndex < 0)
        return;
    // QML passes a bus ROW; store the bus's stable NUMBER (rows shift on
    // removal, numbers don't).
    if (BusListModel *buses = BusListModel::Instance(); !buses)
        return;
    const int busNo = BusListModel::busNumberAt(busIndex);
    if (busNo < 0)
        return;

    // Route lists are sparse to the channel count — extend on first touch.
    while (item.channelRoutes.size() <= channel)
        item.channelRoutes.append(QList<int>());

    QList<int> &route = item.channelRoutes[channel];
    const int at = route.indexOf(busNo);
    if (at >= 0)
        route.removeAt(at);
    else
        route.append(busNo);

    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { RoutingRole });
    scheduleSave();
}

void AudioInputListModel::setChannelRoutes(int index, const QVariantList &perChannel)
{
    if (index < 0 || index >= m_inputs.size())
        return;

    AudioInputItem &item = m_inputs[index];
    QList<QList<int>> parsed;
    if (BusListModel *buses = BusListModel::Instance(); !buses)
        return;
    for (const QVariant &entry : perChannel) {
        QList<int> route;
        const QVariantList cell = entry.toList();
        for (const QVariant &v : cell) {
            bool ok = false;
            const int busRow = v.toInt(&ok);
            // QML passes rows — store stable bus numbers.
            const int busNo = ok ? BusListModel::busNumberAt(busRow) : -1;
            if (busNo >= 0)
                route.append(busNo);
        }
        parsed.append(route);
    }

    if (parsed == item.channelRoutes)
        return;
    item.channelRoutes = parsed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { RoutingRole });
    scheduleSave();
}

static AudioEffect *findEffect(QList<AudioEffect> &effects, const QString &key)
{
    for (AudioEffect &effect : effects) {
        if (effect.key == key)
            return &effect;
    }
    return nullptr;
}

void AudioInputListModel::setEffectEnabled(int index, const QString &key, bool enabled)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    AudioEffect *effect = findEffect(m_inputs[index].effects, key);
    if (!effect || effect->enabled == enabled)
        return;

    effect->enabled = enabled;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { EffectsRole });
    scheduleSave();
    pushEffectsToGraph(index);   // the rack IS the graph's processing chain
}

void AudioInputListModel::setEffectValue(int index, const QString &key, qreal value)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    AudioEffect *effect = findEffect(m_inputs[index].effects, key);
    if (!effect)
        return;

    const qreal clamped = std::clamp(value, effect->minValue, effect->maxValue);
    if (qFuzzyCompare(effect->value, clamped))
        return;

    effect->value = clamped;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { EffectsRole });
    scheduleSave();
    pushEffectsToGraph(index);   // re-translate the whole chain (cheap)
}

QVariantList AudioInputListModel::defaultEffectsTemplate() const
{
    QVariantList list;
    for (const AudioEffect &effect : defaultEffects())
        list.append(effectToVariant(effect));
    return list;
}

QVariantMap AudioInputListModel::getInput(int index) const
{
    if (index < 0 || index >= m_inputs.size())
        return {};

    const AudioInputItem &item = m_inputs.at(index);
    QVariantList effects;
    for (const AudioEffect &effect : item.effects)
        effects.append(effectToVariant(effect));
    QVariantList gains;
    for (qreal g : item.channelGains) gains.append(g);

    return {
        { "name", item.name },
        { "kind", item.kind },
        { "sublabel", item.sublabel },
        { "level", item.level },
        { "muted", item.muted },
        { "delayMs", item.delayMs },
        { "channels", item.channels },
        { "channelGains", gains },
        { "routingAuto", item.routingAuto },
        { "routing", routingVariant(item.channelRoutes) },
        { "effects", effects },
    };
}
