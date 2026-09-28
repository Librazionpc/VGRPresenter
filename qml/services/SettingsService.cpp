#include "services/SettingsService.h"

#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/settings/AppSettings.hpp"
#include "modules/settings/DataProtection.hpp"
#include "modules/settings/SmartConfig.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "platform/PlatformAccessor.hpp"
#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJSEngine>
#include <QQmlEngine>
#include <QSettings>

namespace bs = bps::settings;

namespace {

// One place the app's version is written for the Settings screens (the nav rail and menus still carry their own copy).
constexpr const char *kAppVersion = "1.0.5-beta.2";

QString qstr(const std::string &s) { return QString::fromStdString(s); }

QVariant toVariant(const bps::json::Value &v)
{
    switch (v.type()) {
    case bps::json::Value::Type::Bool: return v.asBool();
    case bps::json::Value::Type::Number: return static_cast<qlonglong>(v.asInt());
    case bps::json::Value::Type::String: return qstr(std::string(v.asString()));
    default: return {};
    }
}

bps::json::Value toJson(const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Bool: return bps::json::Value::Bool(v.toBool());
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
    case QMetaType::Float: return bps::json::Value::Number(v.toDouble());
    case QMetaType::QString: return bps::json::Value::String(v.toString().toStdString());
    default: return {};   // null: the engine refuses it as the wrong type
    }
}

const char *kindName(bs::SettingKind k)
{
    switch (k) {
    case bs::SettingKind::Bool: return "bool";
    case bs::SettingKind::Int: return "int";
    case bs::SettingKind::Choice: return "choice";
    case bs::SettingKind::Text: return "text";
    }
    return "text";
}

QVariantMap choiceMap(const bs::SettingChoice &c, bs::SettingKind kind)
{
    QVariantMap m;
    // An Int's choices hold the number as text; the screen gets it as a number so it can send it straight back.
    m.insert(QStringLiteral("value"), kind == bs::SettingKind::Int ? QVariant(qstr(c.value).toLongLong()) : QVariant(qstr(c.value)));
    m.insert(QStringLiteral("label"), qstr(c.label));
    m.insert(QStringLiteral("description"), qstr(c.description));
    m.insert(QStringLiteral("color"), qstr(c.color));
    m.insert(QStringLiteral("colorLight"), qstr(c.colorLight));
    return m;
}

} // namespace

SettingsService *SettingsService::s_instance = nullptr;

SettingsService &SettingsService::instance()
{
    static SettingsService s;
    return s;
}

SettingsService *SettingsService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

SettingsService::SettingsService(QObject *parent) : QObject(parent)
{
    s_instance = this;

    // Until the engine's store is open the screens see the engine's own defaults.
    for (const bs::SettingDef &d : bs::AppSettings::Definitions())
        values_.insert(qstr(d.key), toVariant(d.dflt));
    for (const bs::SettingChoice &c : bs::AppSettings::Find("appearance.accent")->choices)
        if (c.value == "purple") { accent_ = QColor(qstr(c.color)); accentLight_ = QColor(qstr(c.colorLight)); }

    // The store needs the engine's platform layer: open it when the engine is up (it may already be, or still be booting).
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this] {
        if (EngineBridge::instance().booted())
            load();
    });
    if (EngineBridge::instance().booted())
        load();
}

QString SettingsService::appVersion() const { return QString::fromLatin1(kAppVersion); }

void SettingsService::report(const QString &message, const QString &level) const
{
    EventBus::instance().notify(message, level, tr("Settings"), QStringLiteral("settings.refused"));
}

// ---------------------------------------------------------------------------
// Opening the store
// ---------------------------------------------------------------------------

void SettingsService::load()
{
    if (settings_ || !EngineBridge::instance().booted())
        return;
    auto &platform = bps::platform::PlatformAccessor::Get();
    dataDir_ = platform.Paths().UserDataDir();
    settings_ = std::make_unique<bs::AppSettings>(platform.Filesystem().Join(dataDir_, "settings.json"));
    if (auto loaded = settings_->Load(); !loaded.ok())
        report(tr("Your settings could not be read (%1). The defaults are in use.").arg(qstr(loaded.error().message)), QStringLiteral("warning"));

    // Whatever the user chose last time takes effect now, and every later change does the same.
    (void)bs::ApplyToEngine(*settings_, bps::adaptive::AdaptiveRuntime::Instance());
    settings_->Subscribe([this](const std::string &key) {
        refreshValues();
        if (auto applied = bs::ApplyToEngine(*settings_, bps::adaptive::AdaptiveRuntime::Instance()); !applied.ok())
            report(tr("The engine could not apply that setting (%1).").arg(qstr(applied.error().message)));
        if (key == "startup.launchAtLogin")
            applyLaunchAtLogin();
        emit valueChanged(qstr(key));
        emit changed();
    });

    refreshValues();
    refreshHardware();
    applyLaunchAtLogin();
    emit changed();
}

void SettingsService::refreshValues()
{
    if (!settings_)
        return;
    QVariantMap map;
    for (const bs::SettingDef &d : bs::AppSettings::Definitions())
        map.insert(qstr(d.key), toVariant(settings_->Get(d.key)));
    values_ = map;

    const std::string accent = settings_->GetString("appearance.accent");
    for (const bs::SettingChoice &c : bs::AppSettings::Find("appearance.accent")->choices)
        if (c.value == accent) { accent_ = QColor(qstr(c.color)); accentLight_ = QColor(qstr(c.colorLight)); }
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

QVariantMap SettingsService::definitions() const
{
    QVariantMap all;
    for (const bs::SettingDef &d : bs::AppSettings::Definitions()) {
        QVariantMap m;
        m.insert(QStringLiteral("key"), qstr(d.key));
        m.insert(QStringLiteral("group"), qstr(d.group));
        m.insert(QStringLiteral("label"), qstr(d.label));
        m.insert(QStringLiteral("description"), qstr(d.description));
        m.insert(QStringLiteral("kind"), QString::fromLatin1(kindName(d.kind)));
        m.insert(QStringLiteral("min"), static_cast<qlonglong>(d.min));
        m.insert(QStringLiteral("max"), static_cast<qlonglong>(d.max));
        QVariantList choices;
        for (const bs::SettingChoice &c : d.choices)
            choices.append(choiceMap(c, d.kind));
        m.insert(QStringLiteral("choices"), choices);
        all.insert(qstr(d.key), m);
    }
    return all;
}

QVariant SettingsService::value(const QString &key) const
{
    return values_.value(key);
}

bool SettingsService::setValue(const QString &key, const QVariant &value)
{
    if (!settings_) {
        report(tr("The engine is not running yet, so that setting cannot be saved."));
        return false;
    }
    if (auto set = settings_->Set(key.toStdString(), toJson(value)); !set.ok()) {
        report(qstr(set.error().message));
        return false;
    }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Settings"), QStringLiteral("%1 = %2").arg(key, value.toString()));
    return true;
}

QString SettingsService::choiceLabel(const QString &key) const
{
    const bs::SettingDef *def = bs::AppSettings::Find(key.toStdString());
    if (!def)
        return {};
    const QVariant current = values_.value(key);
    const QString text = current.toString();
    for (const bs::SettingChoice &c : def->choices)
        if (qstr(c.value) == text)
            return qstr(c.label);
    return text;
}

int SettingsService::resetAll()
{
    return settings_ ? static_cast<int>(settings_->ResetAll()) : 0;
}

QVariantMap SettingsService::caps() const
{
    const bs::ResourceCaps c = settings_ ? settings_->EffectiveCaps() : bs::AppSettings::CapsFor("performance");
    return { { QStringLiteral("gpu"), c.gpuPct }, { QStringLiteral("cpu"), c.cpuPct } };
}

QVariantMap SettingsService::allocation() const
{
    const bs::ProfileAllocation a = bs::AppSettings::AllocationFor(settings_ ? settings_->Profile() : std::string("performance"));
    return { { QStringLiteral("rendering"), a.renderingPct }, { QStringLiteral("encoding"), a.encodingPct }, { QStringLiteral("output"), a.outputPct } };
}

// ---------------------------------------------------------------------------
// Smart Config
// ---------------------------------------------------------------------------

void SettingsService::refreshHardware()
{
    if (!EngineBridge::instance().booted())
        return;
    const bs::HardwareReport report = bs::CurrentHardwareReport();
    QVariantList rows;
    for (const bs::HardwareRow &r : report.rows)
        rows.append(QVariantMap{ { QStringLiteral("key"), qstr(r.key) }, { QStringLiteral("label"), qstr(r.label) },
                                 { QStringLiteral("value"), qstr(r.value) }, { QStringLiteral("ok"), r.ok } });
    hardwareRows_ = rows;
    recommendedProfile_ = qstr(report.recommendedProfile);
    hardwareHeadline_ = qstr(report.headline);
    hardwareDetail_ = qstr(report.detail);
    emit hardwareChanged();
}

void SettingsService::applyRecommendedProfile()
{
    if (recommendedProfile_.isEmpty())
        refreshHardware();
    if (!recommendedProfile_.isEmpty())
        setValue(QStringLiteral("resources.profile"), recommendedProfile_);
}

// ---------------------------------------------------------------------------
// Starting with the OS (the one setting only the app itself can carry out)
// ---------------------------------------------------------------------------

void SettingsService::applyLaunchAtLogin()
{
#ifdef Q_OS_WIN
    if (!settings_)
        return;
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"), QSettings::NativeFormat);
    const QString name = QStringLiteral("VGRPresenter");
    if (settings_->GetBool("startup.launchAtLogin"))
        run.setValue(name, QLatin1Char('"') + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + QLatin1Char('"'));
    else
        run.remove(name);
    run.sync();
    if (run.status() != QSettings::NoError)
        report(tr("Windows would not let the app change its start-up entry."), QStringLiteral("warning"));
#endif
}

// ---------------------------------------------------------------------------
// Backups & recovery
// ---------------------------------------------------------------------------

QString SettingsService::backupDirectory() const
{
    if (dataDir_.empty())
        return {};
    return qstr(bps::platform::PlatformAccessor::Get().Filesystem().Join(dataDir_, "backups"));
}

QString SettingsService::backupShow(const QString &showFile)
{
    if (!settings_ || showFile.isEmpty())
        return {};
    const bs::BackupStore store(backupDirectory().toStdString());
    auto made = store.CreateIfChanged(showFile.toStdString());
    if (!made.ok() || made.value().empty())
        return {};
    // Older copies of this show beyond "Keep last" go.
    std::string show = QFileInfo(showFile).completeBaseName().toStdString();
    (void)store.Prune(show, static_cast<size_t>(settings_->GetInt("backups.keepLast")));
    return qstr(made.value());
}

QVariantList SettingsService::backupsOf(const QString &showName) const
{
    QVariantList out;
    if (dataDir_.empty())
        return out;
    const bs::BackupStore store(backupDirectory().toStdString());
    for (const bs::BackupInfo &b : store.List(showName.toStdString()))
        out.append(QVariantMap{ { QStringLiteral("path"), qstr(b.path) }, { QStringLiteral("stamp"), qstr(b.stamp) },
                                { QStringLiteral("sizeBytes"), static_cast<qlonglong>(b.sizeBytes) } });
    return out;
}

QString SettingsService::recoveryPath() const
{
    if (dataDir_.empty())
        return {};
    const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    return qstr(bs::RecoveryStore(fs.Join(dataDir_, "recovery")).Path());
}

bool SettingsService::noteRecovery(const QString &showName, const QString &originalPath)
{
    if (dataDir_.empty())
        return false;
    const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    return bs::RecoveryStore(fs.Join(dataDir_, "recovery")).Note(showName.toStdString(), originalPath.toStdString()).ok();
}

QVariantMap SettingsService::pendingRecovery() const
{
    if (dataDir_.empty())
        return {};
    const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const auto info = bs::RecoveryStore(fs.Join(dataDir_, "recovery")).Info();
    if (!info)
        return {};
    return { { QStringLiteral("path"), qstr(info->path) }, { QStringLiteral("showName"), qstr(info->showName) },
             { QStringLiteral("originalPath"), qstr(info->originalPath) } };
}

void SettingsService::clearRecovery()
{
    if (dataDir_.empty())
        return;
    const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    (void)bs::RecoveryStore(fs.Join(dataDir_, "recovery")).Clear();
}

bool SettingsService::restoreRecovery(const QString &target)
{
    if (dataDir_.empty())
        return false;
    const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    auto restored = bs::RecoveryStore(fs.Join(dataDir_, "recovery")).RestoreTo(target.toStdString());
    if (!restored.ok())
        report(qstr(restored.error().message));
    return restored.ok();
}

bool SettingsService::fileExists(const QString &path) const
{
    return !path.isEmpty() && bps::platform::PlatformAccessor::Get().Filesystem().IsRegularFile(path.toStdString());
}

bps::presentation::ScriptureSettings SettingsService::scriptureSettings() const
{
    return settings_ ? settings_->Scripture() : bps::presentation::ScriptureSettings{};
}

bps::presentation::ScriptureSettings SettingsService::theTableSettings() const
{
    return settings_ ? settings_->TheTable() : bps::presentation::ScriptureSettings{};
}

// ---------------------------------------------------------------------------
// Output style (Settings · Styles applied to the on-air output)
// ---------------------------------------------------------------------------

void SettingsService::setActiveOutputStyle(const bps::presentation::OutputStyleSpec &spec)
{
    // A not-yet-running engine has nothing to push to; after boot the spec
    // lands immediately and every following frame composes with it.
    if (EngineBridge::instance().booted())
        (void)bps::presentation::PresentationEngine::Instance().SetActiveOutputStyle(spec);
}

void SettingsService::setLiveOutputStyles(
    const std::vector<bps::presentation::OutputStyleSpec> &specs,
    const std::vector<std::string> &bufferNames)
{
    if (EngineBridge::instance().booted())
        (void)bps::presentation::PresentationEngine::Instance().SetLiveOutputStyles(specs, bufferNames);
}
