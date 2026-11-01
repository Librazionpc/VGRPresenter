#include "PresetListModel.h"

#include "OutputListModel.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/NameRules.h"
#include "services/RoutingService.h"
#include "modules/project/PresetStore.hpp"

#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

QPointer<PresetListModel> PresetListModel::s_instance = nullptr;

PresetListModel *PresetListModel::instance()
{
    static PresetListModel s;
    return &s;
}

PresetListModel *PresetListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    PresetListModel *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

namespace {

const QStringList kRecordingKeys = {
    QStringLiteral("container"), QStringLiteral("encoder"), QStringLiteral("videoBitrate"),
    QStringLiteral("audioBitrate"), QStringLiteral("frameRate"), QStringLiteral("resolution"),
    QStringLiteral("outputFolder"),
};
// An AUDIO preset: sound only, to a file — which format, how compressed, and where.
const QStringList kAudioKeys = {
    QStringLiteral("format"), QStringLiteral("audioBitrate"), QStringLiteral("outputFolder"),
};
const QStringList kStreamingKeys = {
    QStringLiteral("platform"), QStringLiteral("serverUrl"), QStringLiteral("streamKey"),
    QStringLiteral("videoBitrate"), QStringLiteral("audioBitrate"), QStringLiteral("frameRate"),
    QStringLiteral("resolution"), QStringLiteral("encoder"),
};

bool isKind(const QString &kind)
{
    return kind == QLatin1String("recording") || kind == QLatin1String("streaming") || kind == QLatin1String("audio");
}

QString platformLabel(const QString &key)
{
    if (key == QLatin1String("youtube")) return QStringLiteral("YouTube");
    if (key == QLatin1String("twitch")) return QStringLiteral("Twitch");
    if (key == QLatin1String("facebook")) return QStringLiteral("Facebook");
    return QStringLiteral("Custom RTMP");
}

void toast(const QString &message, const QString &level = QStringLiteral("warning"))
{
    qWarning("PresetListModel: %s", qUtf8Printable(message));
    EventBus::instance().notify(message, level, QStringLiteral("Recording"),
                                QStringLiteral("recording.preset"));
}

QVariantMap toMap(const bps::project::StoredPreset &s, QString *kind = nullptr)
{
    Q_UNUSED(kind)
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(s.configJson));
    return doc.isObject() ? doc.object().toVariantMap() : QVariantMap();
}

} // namespace

PresetListModel::PresetListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    s_instance = this;
    // The kernel store opens during Boot, after the QML load constructed this —
    // re-hydrate the moment boot completes (see StyleListModel's note on the gap).
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this] {
        if (EngineBridge::instance().booted())
            reloadFromStore();
    });
    reloadFromStore();
}

PresetListModel::~PresetListModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

void PresetListModel::reloadFromStore()
{
    beginResetModel();
    m_presets.clear();
    for (const bps::project::StoredPreset &s : bps::project::PresetStore::Instance().Get()) {
        PresetItem item;
        item.id = QString::fromStdString(s.id);
        item.name = QString::fromStdString(s.name);
        item.kind = QString::fromStdString(s.kind);
        item.config = normalized(item.kind, toMap(s));
        m_presets.append(item);
    }
    endResetModel();
}

void PresetListModel::save()
{
    std::vector<bps::project::StoredPreset> stored;
    stored.reserve(size_t(m_presets.size()));
    for (const PresetItem &p : m_presets) {
        bps::project::StoredPreset s;
        s.id = p.id.toStdString();
        s.name = p.name.toStdString();
        s.kind = p.kind.toStdString();
        s.configJson = QJsonDocument(QJsonObject::fromVariantMap(p.config))
                           .toJson(QJsonDocument::Compact).toStdString();
        stored.push_back(std::move(s));
    }
    if (auto r = bps::project::PresetStore::Instance().Save(stored); !r.ok())
        toast(tr("Could not save the presets: %1").arg(QString::fromStdString(r.error().message)),
              QStringLiteral("error"));
}

// ---------------------------------------------------------------------------
// Model plumbing
// ---------------------------------------------------------------------------

int PresetListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_presets.size());
}

QVariant PresetListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_presets.size())
        return {};
    const PresetItem &p = m_presets.at(index.row());
    switch (role) {
    case IdRole: return p.id;
    case Qt::DisplayRole:
    case NameRole: return p.name;
    case KindRole: return p.kind;
    case SummaryRole: return summaryFor(p.kind, p.config);
    case ConfigRole: return p.config;
    }
    return {};
}

QHash<int, QByteArray> PresetListModel::roleNames() const
{
    return {
        { IdRole, "presetId" },
        { NameRole, "name" },
        { KindRole, "kind" },
        { SummaryRole, "summary" },
        { ConfigRole, "config" },
    };
}

// ---------------------------------------------------------------------------
// Config shape
// ---------------------------------------------------------------------------

QVariantMap PresetListModel::defaultConfig(const QString &kind) const
{
    return defaults(kind);
}

QVariantMap PresetListModel::defaults(const QString &kind)
{
    if (kind == QLatin1String("audio")) {
        return {
            { "format", "M4A" },
            { "audioBitrate", "192" },
            { "outputFolder", "" },
        };
    }
    if (kind == QLatin1String("streaming")) {
        return {
            { "platform", "youtube" },
            { "serverUrl", "rtmp://a.rtmp.youtube.com/live2" },
            { "streamKey", "" },
            { "videoBitrate", "6000" },
            { "audioBitrate", "192" },
            { "frameRate", "60 fps" },
            { "resolution", "1080p" },
            { "encoder", "Software" },
        };
    }
    return {
        { "container", "MP4" },
        { "encoder", "Software" },
        { "videoBitrate", "12000" },
        { "audioBitrate", "192" },
        { "frameRate", "60 fps" },
        { "resolution", "1080p" },
        { "outputFolder", "" },
    };
}

QVariantMap PresetListModel::normalized(const QString &kind, const QVariantMap &config)
{
    QVariantMap out = defaults(kind);
    const QStringList &keys = kind == QLatin1String("streaming") ? kStreamingKeys
                             : kind == QLatin1String("audio")     ? kAudioKeys : kRecordingKeys;
    for (const QString &key : keys)
        if (config.contains(key))
            out.insert(key, config.value(key).toString().trimmed());
    return out;
}

// Per-field refusals, keyed by the config field they are about, so the dialog can print
// each one UNDER its own field. Empty map = acceptable.
QVariantMap PresetListModel::fieldErrors(const QString &kind, const QVariantMap &config) const
{
    QVariantMap errors;
    if (!isKind(kind)) {
        errors.insert(QStringLiteral("kind"), tr("Pick a preset type first."));
        return errors;
    }
    const auto positive = [&config](const char *key) {
        bool ok = false;
        const int n = config.value(QLatin1String(key)).toString().trimmed().toInt(&ok);
        return ok && n > 0;
    };
    if (kind == QLatin1String("audio")) {
        // Sound only: no picture settings. WAV is uncompressed, so it has no bitrate to check.
        const QString format = config.value(QStringLiteral("format")).toString();
        if (format != QLatin1String("WAV") && format != QLatin1String("M4A") && format != QLatin1String("MP3"))
            errors.insert(QStringLiteral("format"), tr("Pick WAV, M4A or MP3."));
        else if (format != QLatin1String("WAV") && !positive("audioBitrate"))
            errors.insert(QStringLiteral("audioBitrate"), tr("Use a number of kbps, like 192."));
        return errors;
    }
    if (!positive("videoBitrate"))
        errors.insert(QStringLiteral("videoBitrate"), tr("Use a number of kbps, like 6000."));
    if (!positive("audioBitrate"))
        errors.insert(QStringLiteral("audioBitrate"), tr("Use a number of kbps, like 192."));
    if (kind == QLatin1String("streaming")) {
        const QString url = config.value(QStringLiteral("serverUrl")).toString().trimmed();
        if (!url.startsWith(QLatin1String("rtmp://")) && !url.startsWith(QLatin1String("rtmps://")))
            errors.insert(QStringLiteral("serverUrl"), tr("The server URL must start with rtmp:// (or rtmps://)."));
        if (config.value(QStringLiteral("streamKey")).toString().trimmed().isEmpty())
            errors.insert(QStringLiteral("streamKey"), tr("Paste your stream key — the platform needs it to accept the feed."));
    }
    return errors;
}

// The same checks as one sentence, for callers that have no field to print under (the
// toast an add/update raises when something other than the dialog calls it).
QString PresetListModel::validate(const QString &kind, const QVariantMap &config) const
{
    const QVariantMap errors = fieldErrors(kind, config);
    for (const char *key : { "kind", "format", "videoBitrate", "audioBitrate", "serverUrl", "streamKey" })
        if (errors.contains(QLatin1String(key)))
            return errors.value(QLatin1String(key)).toString();
    return {};
}

QString PresetListModel::validateName(const QString &name, int exceptRow) const
{
    // The wording lives in NameRules (one place for every roster); only the noun is ours.
    QStringList taken;
    for (int i = 0; i < m_presets.size(); ++i)
        if (i != exceptRow)
            taken.append(m_presets.at(i).name);
    return NameRules::problem(name, taken, tr("preset"));
}

QString PresetListModel::summaryFor(const QString &kind, const QVariantMap &config)
{
    if (kind == QLatin1String("audio")) {
        const QString format = config.value(QStringLiteral("format")).toString();
        const QString kbps = config.value(QStringLiteral("audioBitrate")).toString();
        const QString shown = format == QLatin1String("M4A") ? QStringLiteral("M4A (AAC)") : format;
        return format == QLatin1String("WAV") ? shown + QStringLiteral(" · lossless")
                                              : shown + QStringLiteral(" · ") + kbps + QStringLiteral(" kbps");
    }
    const QString res = config.value(QStringLiteral("resolution")).toString();
    const QString kbps = config.value(QStringLiteral("videoBitrate")).toString();
    const QString rate = kbps.isEmpty() ? QString() : kbps + QStringLiteral(" kbps");
    QStringList parts;
    if (kind == QLatin1String("streaming")) {
        parts << platformLabel(config.value(QStringLiteral("platform")).toString());
    } else {
        parts << config.value(QStringLiteral("container")).toString()
              << config.value(QStringLiteral("encoder")).toString();
    }
    parts << res << rate;
    parts.removeAll(QString());
    return parts.join(QStringLiteral(" · "));
}

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

bool PresetListModel::nameTaken(const QString &name, int exceptRow) const
{
    for (int i = 0; i < m_presets.size(); ++i)
        if (i != exceptRow && m_presets.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QString PresetListModel::uniqueName(const QString &wanted) const
{
    if (!nameTaken(wanted, -1))
        return wanted;
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(wanted).arg(n);
        if (!nameTaken(candidate, -1))
            return candidate;
    }
}

int PresetListModel::nextIdNumber() const
{
    // "p<n>", one past the highest seen — parsed, never counted, so a removed
    // preset's id is not handed to the next one an output may still point at.
    int max = 0;
    for (const PresetItem &item : m_presets) {
        bool ok = false;
        const int n = item.id.mid(1).toInt(&ok);
        if (ok)
            max = std::max(max, n);
    }
    return max + 1;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

bool PresetListModel::addPreset(const QString &kind, const QString &name, const QVariantMap &config)
{
    const QString trimmed = name.trimmed();
    if (!isKind(kind)) {
        toast(tr("Pick a preset type first."));
        return false;
    }
    if (const QString why = validateName(trimmed, -1); !why.isEmpty()) {
        toast(why);
        return false;
    }
    const QVariantMap cfg = normalized(kind, config);
    if (const QString why = validate(kind, cfg); !why.isEmpty()) {
        toast(why);
        return false;
    }

    PresetItem item;
    item.id = QStringLiteral("p%1").arg(nextIdNumber());
    item.name = trimmed;
    item.kind = kind;
    item.config = cfg;
    const int row = int(m_presets.size());
    beginInsertRows(QModelIndex(), row, row);
    m_presets.append(item);
    endInsertRows();
    save();
    return true;
}

bool PresetListModel::updatePreset(int index, const QString &name, const QVariantMap &config)
{
    if (index < 0 || index >= m_presets.size())
        return false;
    const QString trimmed = name.trimmed();
    if (const QString why = validateName(trimmed, index); !why.isEmpty()) {
        toast(why);
        return false;
    }
    PresetItem &item = m_presets[index];
    const QVariantMap cfg = normalized(item.kind, config);
    if (const QString why = validate(item.kind, cfg); !why.isEmpty()) {
        toast(why);
        return false;
    }
    item.name = trimmed;
    item.config = cfg;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed);
    save();
    // The output rows that wear it re-read their name / summary, and a stream
    // preset's target is part of its output node.
    if (OutputListModel *outputs = OutputListModel::instance())
        outputs->presetChanged(item.id);
    refreshRouting();
    return true;
}

void PresetListModel::duplicatePreset(int index)
{
    if (index < 0 || index >= m_presets.size())
        return;
    PresetItem copy = m_presets.at(index);
    copy.id = QStringLiteral("p%1").arg(nextIdNumber());
    copy.name = uniqueName(tr("%1 (copy)").arg(copy.name));
    beginInsertRows(QModelIndex(), index + 1, index + 1);
    m_presets.insert(index + 1, copy);
    endInsertRows();
    save();
}

void PresetListModel::removePreset(int index)
{
    if (index < 0 || index >= m_presets.size())
        return;
    const QString doomed = m_presets.at(index).id;
    if (OutputListModel *outputs = OutputListModel::instance())
        outputs->detachPresetEverywhere(doomed);
    beginRemoveRows(QModelIndex(), index, index);
    m_presets.removeAt(index);
    endRemoveRows();
    save();
    refreshRouting();
}

void PresetListModel::refreshRouting()
{
    if (RoutingService *routing = RoutingService::instance())
        routing->refresh();
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

QVariantMap PresetListModel::getPreset(int index) const
{
    if (index < 0 || index >= m_presets.size())
        return {};
    const PresetItem &p = m_presets.at(index);
    return {
        { "id", p.id }, { "name", p.name }, { "kind", p.kind },
        { "summary", summaryFor(p.kind, p.config) }, { "config", p.config },
    };
}

int PresetListModel::rowOfId(const QString &presetId) const
{
    for (int i = 0; i < m_presets.size(); ++i)
        if (m_presets.at(i).id == presetId)
            return i;
    return -1;
}

QVariantMap PresetListModel::configOf(const QString &presetId) const
{
    const int row = rowOfId(presetId);
    return row < 0 ? QVariantMap() : m_presets.at(row).config;
}

QString PresetListModel::kindOf(const QString &presetId) const
{
    const int row = rowOfId(presetId);
    return row < 0 ? QString() : m_presets.at(row).kind;
}

QString PresetListModel::nameOf(const QString &presetId) const
{
    const int row = rowOfId(presetId);
    return row < 0 ? QString() : m_presets.at(row).name;
}

QString PresetListModel::summaryOf(const QString &presetId) const
{
    const int row = rowOfId(presetId);
    return row < 0 ? QString() : summaryFor(m_presets.at(row).kind, m_presets.at(row).config);
}

QVariantList PresetListModel::presetsOfKind(const QString &kind) const
{
    QVariantList out;
    for (const PresetItem &p : m_presets)
        if (p.kind == kind)
            out.append(QVariantMap{ { "presetId", p.id }, { "name", p.name },
                                    { "summary", summaryFor(p.kind, p.config) } });
    return out;
}

int PresetListModel::usageCount(const QString &presetId) const
{
    if (presetId.isEmpty())
        return 0;
    OutputListModel *outputs = OutputListModel::instance();
    return outputs ? outputs->presetUsage(presetId) : 0;
}

QVariantMap PresetListModel::lookup(const QString &presetId)
{
    if (presetId.isEmpty())
        return {};
    const std::string wanted = presetId.toStdString();
    for (const bps::project::StoredPreset &s : bps::project::PresetStore::Instance().Get()) {
        if (s.id != wanted)
            continue;
        const QString kind = QString::fromStdString(s.kind);
        QVariantMap map = toMap(s);
        // Same shape a row reads: fields the saved blob predates come from the defaults.
        return {
            { "id", presetId }, { "name", QString::fromStdString(s.name) }, { "kind", kind },
            { "config", normalized(kind, map) },
        };
    }
    return {};
}
