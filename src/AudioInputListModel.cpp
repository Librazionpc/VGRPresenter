#include "AudioInputListModel.h"

#include <algorithm>

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
    // Seeded roster so the routing board has something to route from on
    // first run (same rationale as OutputListModel's seeded outputs —
    // StyleListModel/SlideListModel start empty because those are pure user
    // content, but a mixer with nothing plugged in doesn't demonstrate
    // anything).
    //
    // Row ORDER is a stored contract: BusListModel seeds its routing by row
    // index (Master routes {0,1,3}, etc.), so inserting or reordering these
    // rows silently rewires every bus.
    AudioInputItem media;
    media.name = QStringLiteral("Media Player");
    media.kind = QStringLiteral("media");
    media.sublabel = QStringLiteral("Playlists & tracks");
    media.level = 60;
    media.effects = defaultEffects();
    m_inputs.append(media);

    AudioInputItem worship;
    worship.name = QStringLiteral("Worship Mix");
    worship.kind = QStringLiteral("media");
    worship.level = 82;
    worship.effects = defaultEffects();
    m_inputs.append(worship);

    AudioInputItem preService;
    preService.name = QStringLiteral("Pre-service");
    preService.kind = QStringLiteral("media");
    preService.level = 45;
    preService.muted = true;
    preService.effects = defaultEffects();
    m_inputs.append(preService);

    AudioInputItem mic1;
    mic1.name = QStringLiteral("Mic 1 · Lavalier");
    mic1.kind = QStringLiteral("device");
    mic1.level = 70;
    mic1.effects = defaultEffects();
    m_inputs.append(mic1);

    AudioInputItem mic2;
    mic2.name = QStringLiteral("Mic 2 · Handheld");
    mic2.kind = QStringLiteral("device");
    mic2.level = 55;
    mic2.muted = true;
    mic2.effects = defaultEffects();
    m_inputs.append(mic2);

    AudioInputItem lineIn;
    lineIn.name = QStringLiteral("Line In · Pulpit");
    lineIn.kind = QStringLiteral("device");
    lineIn.level = 50;
    lineIn.effects = defaultEffects();
    m_inputs.append(lineIn);

    AudioInputItem desktop;
    desktop.name = QStringLiteral("Desktop Audio");
    desktop.kind = QStringLiteral("device");
    desktop.sublabel = QStringLiteral("System sounds");
    desktop.level = 65;
    desktop.effects = defaultEffects();
    m_inputs.append(desktop);
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
        { EffectsRole, "effects" },
    };
}

void AudioInputListModel::addInput()
{
    const int row = m_inputs.size();
    beginInsertRows(QModelIndex(), row, row);
    AudioInputItem added;
    added.name = QStringLiteral("New Input %1").arg(row + 1);
    added.effects = defaultEffects();
    m_inputs.append(added);
    endInsertRows();
}

void AudioInputListModel::removeInput(int index)
{
    if (index < 0 || index >= m_inputs.size())
        return;

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
        { "effects", effects },
    };
}
