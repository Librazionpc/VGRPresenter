#include "AudioInputListModel.h"

#include "BusListModel.h"
#include "ChannelTrim.h"
#include "services/EngineBridge.h"
#include "services/SettingsService.h"
#include "platform/PlatformAccessor.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/production/ProductionTypes.hpp"

#include <QJsonArray>
#include <QJSEngine>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cstring>
#include <string>

// The one instance, for C++ AND QML — see the header for why this is eager.
AudioInputListModel *AudioInputListModel::Instance()
{
    static AudioInputListModel s;
    return &s;
}

AudioInputListModel *AudioInputListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    AudioInputListModel *s = Instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

// "bus:<n>#a"/"bus:<n>#v" → the bus NUMBER, the stable identity the routing
// matrix stores (rows shift; numbers don't). -1 for anything else, so a caller
// can tell "not one of our planes" from "bus 0".
static int busNumberFromPlaneNode(const std::string &nodeId)
{
    constexpr const char *prefix = "bus:";
    if (!nodeId.starts_with(prefix))
        return -1;
    const size_t hash = nodeId.find('#');
    const size_t from = std::strlen(prefix);
    if (hash == std::string::npos || hash <= from)
        return -1;
    try {
        return std::stoi(nodeId.substr(from, hash - from));
    } catch (const std::exception &) {
        return -1;
    }
}

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
    Q_ASSERT_X(!s_instance || s_instance == this, "AudioInputListModel",
               "a second AudioInputListModel was constructed — Instance() "
               "would silently rebind and every C++ reader would hold the "
               "wrong one");
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
    // session.audioRoster; stable ids are restored so the production graph's
    // saved routes can reconnect to the same rows after launch.
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

void AudioInputListModel::adoptInstance(AudioInputListModel *roster)
{
    if (!roster || roster == s_instance)
        return;
    if (s_instance)
        s_instance->deactivate();
    s_instance = roster;
}

void AudioInputListModel::deactivate()
{
    m_active = false;
    if (saveTimer_) {
        saveTimer_->stop();
        delete saveTimer_;
        saveTimer_ = nullptr;
    }
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

QString AudioInputListModel::idForRow(int row)
{
    AudioInputListModel *self = s_instance ? s_instance : Instance();
    if (row < 0 || row >= self->m_inputs.size())
        return {};
    QString &id = self->m_inputs[row].id;
    if (id.isEmpty()) {
        id = QStringLiteral("a%1").arg(self->m_nextId++);
        self->scheduleSave();   // the healed id has to survive the session
    }
    return id;
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
    //
    // Read through the HEALING accessor, never the raw member: a row whose id was
    // never assigned has NO id, and an empty id made the whole cut below a silent
    // no-op — the row left the board while its graph edges (and the lines drawn
    // for them) stayed, and the next row added was handed that freed id and
    // inherited them.
    const QString id = idForRow(index);
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
        // Audio containers only — the input is opened as audio and refuses
        // anything else, so the picker is where that decision belongs.
        { "Audio files (*.mp3 *.wav *.flac *.m4a *.aac *.ogg)" });
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
    if (!m_active)
        return;   // never push a displaced roster's view onto the engine
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

    const bool ownsEdit = graph.BeginEdit().ok();
    const AudioInputItem &item = m_inputs.at(index);

    // ---- VolumeControl: the row's fader + mute -----------------------------
    bps::production::VolumeControl vol;
    if (auto cur = graph.Volume(node); cur.ok())
        vol = cur.value();          // keep solo/pan; this model owns gain/mute
    // UI 0..100 → engine gain, the SAME mapping BusListModel's fader uses
    // (0 = −60 dB silence, 100 = unity). Local copy of the mapping — the
    // helper lives in BusListModel.cpp's anonymous namespace.
    vol.gainDb = -60.0 + (item.level / 100.0) * 60.0;
    vol.mute = item.muted;
    (void)graph.SetVolume(node, vol);

    // Store the independent channel trims on the source node so the actual
    // AudioMixer path (and its program meter) applies the same L/R state as
    // the Media and Settings meters. Older rosters with no saved trims use
    // unity, matching channelGain().
    for (int channel = 0; channel < 8; ++channel) {
        const qreal gain = channel < item.channelGains.size()
            ? item.channelGains.at(channel) : 1.0;
        (void)graph.SetMeta(node, "uiChannelGain" + std::to_string(channel),
                            std::to_string(gain));
    }

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
    if (ownsEdit)
        (void)graph.CommitEdit();
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
    // Remember the level as this side's trim on the way past (see
    // models/ChannelTrim.h). It rides EVERY write of an open level, so a knob
    // dragged here leaves a restorable trim behind without the caller knowing
    // anything about it — and a close (gain 0) deliberately does not, or the
    // next reopen would arm silence.
    const bool remember = !qFuzzyCompare(gains[channel], clamped)
                          && channeltrim::remembersTrim(clamped);
    if (qFuzzyCompare(gains[channel], clamped))
        return;
    gains[channel] = clamped;
    if (remember)
        writeChannelTrim(index, channel, clamped);
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ChannelGainsRole });
    scheduleSave();   // debounced — a dragged knob coalesces into one write
    pushEffectsToGraph(index);   // per-channel trim rides the node volume
}

void AudioInputListModel::toggleChannelGain(int index, int channel)
{
    // THE reopen policy, asked of one place: ChannelTrim.h. Closing writes 0;
    // reopening hands back the trim the side was closed with, and only unity if
    // there never was one.
    if (channeltrim::isOff(channelGain(index, channel)))
        setChannelGain(index, channel, channelTrim(index, channel));
    else
        setChannelGain(index, channel, channeltrim::closed());
}

qreal AudioInputListModel::channelTrim(int index, int channel) const
{
    if (index < 0 || index >= m_inputs.size() || channel < 0)
        return channeltrim::unity();
    const AudioInputItem &item = m_inputs.at(index);
    const qreal remembered = channel < item.channelTrims.size()
        ? item.channelTrims.at(channel) : channeltrim::closed();
    return channeltrim::reopenGain(remembered, channelGain(index, channel));
}

void AudioInputListModel::setChannelTrim(int index, int channel, qreal trim)
{
    if (index < 0 || index >= m_inputs.size() || channel < 0 || channel >= 8)
        return;
    if (!writeChannelTrim(index, channel, std::clamp(trim, 0.0, 1.0)))
        return;
    scheduleSave();
}

// The one write of the trim list (kept private-ish: the public faces above only
// decide WHEN a trim is worth remembering). Returns true when it changed.
bool AudioInputListModel::writeChannelTrim(int index, int channel, qreal trim)
{
    if (index < 0 || index >= m_inputs.size() || channel < 0 || channel >= 8)
        return false;
    QList<qreal> &trims = m_inputs[index].channelTrims;
    while (trims.size() <= channel)
        trims.append(channeltrim::closed());   // absent = nothing remembered
    if (qFuzzyCompare(trims[channel], trim))
        return false;
    trims[channel] = trim;
    return true;
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

QString AudioInputListModel::serializeRoster() const
{
    QJsonArray rows;
    for (const AudioInputItem &it : m_inputs) {
        QJsonArray gains;
        for (qreal g : it.channelGains) gains.append(g);
        // The trims each side was closed at, beside the gains they belong to: a
        // side closed at -6 dB comes back at -6 dB in the next session too (see
        // models/ChannelTrim.h). Written even when empty, so the saved row
        // always says what it knows.
        QJsonArray trims;
        for (qreal t : it.channelTrims) trims.append(t);
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
            { "trims", trims },
            { "routingAuto", it.routingAuto },
            { "routes", routes },
            { "effects", effects },
        });
    }
    return QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact));
}

void AudioInputListModel::queueRosterWrite()
{
    if (!m_active)
        return;
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

bool AudioInputListModel::restoreDone()
{
    return s_instance && s_instance->m_restoreDone;
}

int AudioInputListModel::restoreRoster()
{
    SettingsService *settings = SettingsService::instancePtr();
    if (!settings || !EngineBridge::instance().booted())
        return 0;
    // From here on the store has ANSWERED — empty means empty, so the graph's
    // orphan sweep may treat these rows as the authority for which source nodes
    // are still live (see BusListModel::pruneOrphanSourceEdges). Set before the
    // blob checks on purpose: a roster that answers "nothing saved" is an answer.
    m_restoreDone = true;
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
    QSet<QString> assigned;
    QList<int> migratedSilentDefaults;
    for (const QJsonValue &rv : rows) {
        const QJsonObject o = rv.toObject();
        AudioInputItem it;
        it.id = o.value(QStringLiteral("id")).toString();
        it.name = o.value(QStringLiteral("name")).toString();
        it.kind = o.value(QStringLiteral("kind")).toString(QStringLiteral("device"));
        it.sublabel = o.value(QStringLiteral("sublabel")).toString();
        it.level = std::clamp(o.value(QStringLiteral("level")).toDouble(75.0), 0.0, 100.0);
        it.muted = o.value(QStringLiteral("muted")).toBool();
        // Older builds created configured, unmuted inputs with a 0 fader by
        // default. That made a newly routed live source silent until users
        // discovered and raised this control. Migrate that legacy default;
        // a deliberately muted input stays untouched.
        if (it.level <= 0.0001 && !it.muted && !it.sublabel.trimmed().isEmpty()) {
            it.level = 75.0;
            migratedSilentDefaults.append(m_inputs.size());
        }
        it.delayMs = std::clamp(o.value(QStringLiteral("delayMs")).toInt(), 0, 1000);
        it.channels = std::clamp(o.value(QStringLiteral("channels")).toInt(2), 1, 8);
        for (const QJsonValue &g : o.value(QStringLiteral("gains")).toArray())
            it.channelGains.append(std::clamp(g.toDouble(1.0), 0.0, 1.0));
        // Rows saved before the trims existed simply have none: channelTrim()
        // then falls back to whatever level the side holds open (and unity
        // beyond that), so an older roster cannot reopen a side at 0.
        for (const QJsonValue &t : o.value(QStringLiteral("trims")).toArray())
            it.channelTrims.append(std::clamp(t.toDouble(0.0), 0.0, 1.0));
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
        //
        // Against the ids already TAKEN in this restore, not against a counter
        // that only moves when a row carries a parseable number: two rows with a
        // missing id were both handed "a<m_nextId>" — the SAME id — and the
        // second row's node was the first row's node. Routes key on that id, so
        // the board drew one row's lines under another card and, worse, a
        // removal cut the wrong row's edge (or none): the line stayed behind a
        // card that was gone.
        if (it.id.isEmpty() || assigned.contains(it.id)) {
            while (assigned.contains(QStringLiteral("a%1").arg(nextId)))
                ++nextId;
            it.id = QStringLiteral("a%1").arg(nextId);
        }
        assigned.insert(it.id);
        bool okNum = false;
        const int num = it.id.mid(1).toInt(&okNum);
        if (okNum)
            nextId = std::max(nextId, num + 1);
        m_inputs.append(it);
    }
    m_nextId = std::max(m_nextId, nextId);
    endResetModel();

    for (int row = 0; row < m_inputs.size(); ++row) {
        pushEffectsToGraph(row);
        // A matrix saved in an earlier session must reach the graph again —
        // otherwise a restored routing shows checked in the dialog and is dead
        // in the engine (the "it isn't saving my connections" report).
        pushRoutingToGraph(row);
    }

    if (!migratedSilentDefaults.isEmpty())
        scheduleSave();

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

// ---------------------------------------------------------------------------
// ROUTING MATRIX → GRAPH — see the header. Called from every matrix write and
// from the roster restore, so a matrix the user set in an earlier session lands
// in the graph again on the next boot.
void AudioInputListModel::pushRoutingToGraph(int index)
{
    if (!m_active)
        return;   // a displaced roster must not re-add an edge the board removed
    if (index < 0 || index >= m_inputs.size())
        return;
    if (!EngineBridge::instance().booted())
        return;   // graph not up yet — the next write after boot pushes again

    const QString nodeQ = BusListModel::audioSourceNodeId(index);
    if (nodeQ.isEmpty())
        return;
    const std::string node = nodeQ.toStdString();

    // bus number → the channels routed into it.
    QMap<int, unsigned> masks;
    const AudioInputItem &item = m_inputs.at(index);
    for (int c = 0; c < item.channelRoutes.size() && c < 8; ++c) {
        for (const int busNo : item.channelRoutes.at(c)) {
            if (busNo < 0)
                continue;
            masks[busNo] = masks.value(busNo, 0u) | (1u << c);
        }
    }
    auto &graph = bps::production::ProductionEngine::Instance().Graph();
    const bool ownsEdit = graph.BeginEdit().ok();
    // ADD half — one edge (+ its channel mask) per bus the matrix names. Skipped
    // when the matrix names nothing; the REMOVE half below still runs, because
    // "the matrix names no bus at all" is exactly when its old edges must go.
    for (auto it = masks.constBegin(); it != masks.constEnd(); ++it) {
        const std::string busNode = "bus:" + std::to_string(it.key()) + "#a";
        if (!graph.HasNode(busNode))
            continue;   // that bus row is gone; the matrix entry is stale
        // The row's node is created on its FIRST route (the same lazy creation
        // the bus dialog's toggle performs) — a matrix-only route is still a
        // route, and without the node there is nothing to feed the bus.
        if (!graph.HasNode(node)) {
            if (!graph.AddSource(node, "Audio Source",
                                 bps::production::SignalType::Audio).ok())
                continue;
        }
        bool connected = false;
        for (const auto &up : graph.Upstream(busNode))
            if (up == node) { connected = true; break; }
        if (!connected
            && !graph.Connect(node, busNode, bps::production::SignalType::Audio).ok()) {
            qWarning() << "AudioInputListModel: routing-matrix Connect failed for"
                       << QString::fromStdString(busNode);
            continue;
        }
        (void)graph.SetMeta(node, "uiRouteMask:" + busNode, std::to_string(it.value()));
    }
    // REMOVE half — the edges THIS ROW'S MATRIX made and no longer names. The
    // marker is the "uiRouteMask:<busNode>" meta the add half just wrote: a route
    // made on the board's drag or in the bus dialog's toggle carries no mask, so
    // those surfaces keep their own edges and nothing fights over one edge. What
    // this fixes: unticking the LAST channel of a bus left the edge and its stale
    // channel mask in the graph, so the row stayed wired to (and audible on) a
    // bus its matrix no longer routed to.
    if (graph.HasNode(node)) {
        const auto meta = graph.GetNode(node);
        if (meta.ok()) {
            for (const std::string &busNode : graph.Downstream(node)) {
                const QString key = QStringLiteral("uiRouteMask:") + QString::fromStdString(busNode);
                if (!meta.value().meta.count(key.toStdString()))
                    continue;   // not this matrix's edge
                const int busNo = busNumberFromPlaneNode(busNode);
                if (busNo < 0 || masks.contains(busNo))
                    continue;   // still routed there (or not one of ours)
                if (graph.Disconnect(node, busNode).ok()) {
                    qInfo() << "AudioInputListModel: routing-matrix unrouted" << nodeQ
                            << "from" << QString::fromStdString(busNode);
                } else {
                    qWarning() << "AudioInputListModel: routing-matrix Disconnect failed for"
                               << nodeQ << "->" << QString::fromStdString(busNode);
                }
            }
        }
    }
    if (ownsEdit)
        (void)graph.CommitEdit();
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
    pushRoutingToGraph(index);   // the cell IS a route into that bus
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
    pushRoutingToGraph(index);   // the whole matrix, in one push
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
    QVariantList trims;
    for (qreal t : item.channelTrims) trims.append(t);

    return {
        { "name", item.name },
        { "kind", item.kind },
        { "sublabel", item.sublabel },
        { "level", item.level },
        { "muted", item.muted },
        { "delayMs", item.delayMs },
        { "channels", item.channels },
        { "channelGains", gains },
        { "channelTrims", trims },
        { "routingAuto", item.routingAuto },
        { "routing", routingVariant(item.channelRoutes) },
        { "effects", effects },
    };
}
