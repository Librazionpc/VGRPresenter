#include "AudioInputListModel.h"

#include "BusListModel.h"

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
    // Row ORDER is a stored contract: BusListModel routes reference rows by
    // index, so removing a row shifts them (no remap exists yet — routes can
    // go stale after a removal; a known limitation, see KNOWN_ISSUES.md).
}

AudioInputListModel::~AudioInputListModel()
{
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
    case ModeRole: return item.mode;
    case DelayMsRole: return item.delayMs;
    case ChannelsRole: return item.channels;
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
        { ModeRole, "mode" },
        { DelayMsRole, "delayMs" },
        { ChannelsRole, "channels" },
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
}

void AudioInputListModel::removeInput(int index)
{
    if (index < 0 || index >= m_inputs.size())
        return;

    // Cut this source's routing edges BEFORE the row disappears: routes key
    // on the row's stable id, so nothing shifts or goes stale — other rows'
    // routes are untouched by construction.
    const QString id = m_inputs.at(index).id;
    if (BusListModel *buses = BusListModel::Instance())
        buses->cutAudioSourceEdges(id);

    beginRemoveRows(QModelIndex(), index, index);
    m_inputs.removeAt(index);
    endRemoveRows();
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
    m_inputs.append(copy);
    endInsertRows();
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
}

void AudioInputListModel::setMode(int index, int mode)
{
    if (index < 0 || index >= m_inputs.size())
        return;
    const int clamped = std::clamp(mode, 0, 3);
    if (m_inputs[index].mode == clamped)
        return;

    m_inputs[index].mode = clamped;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ModeRole });
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

    return {
        { "name", item.name },
        { "kind", item.kind },
        { "sublabel", item.sublabel },
        { "level", item.level },
        { "muted", item.muted },
        { "mode", item.mode },
        { "delayMs", item.delayMs },
        { "channels", item.channels },
        { "routingAuto", item.routingAuto },
        { "routing", routingVariant(item.channelRoutes) },
        { "effects", effects },
    };
}
