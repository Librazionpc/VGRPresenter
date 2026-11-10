#include "services/SettingsService.h"

#include "core/config/ConfigurationManager.hpp"
#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/settings/AppSettings.hpp"
#include "modules/settings/DataProtection.hpp"
#include "modules/settings/SmartConfig.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "platform/PlatformAccessor.hpp"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QLocale>
#include <QProcess>
#include <QQmlEngine>
#include <QSettings>
#include <QThread>
#include <QStandardPaths>
#include <QTranslator>
#include <QUrl>

#include <utility>

namespace bs = bps::settings;

namespace {
// One place the app's version is written for the Settings screens (the nav rail and menus still carry their own copy).
constexpr const char *kAppVersion = "0.1.3";

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

// ---------------------------------------------------------------------------
// The renderer the user asked for (Settings · General's hardware switch)
// ---------------------------------------------------------------------------
struct RenderBackendApply {
    bool changed = false;   // the preference really changed (only that is announced)
    bool wantGpu = false;   // what the engine was asked for
    QString active;         // what it is really drawing with ("" = not initialized yet)
};

RenderBackendApply ApplyRenderBackendPreference(const bs::AppSettings &settings)
{
    RenderBackendApply out;
    const bool wantGpu = settings.GetBool("preferences.hardwareAcceleration");
    auto &config = bps::ConfigurationManager::Instance();
    if (config.GetBool("render.hardwareAcceleration", false) == wantGpu)
        return out;   // already in force: there is nothing to ask the engine for

    (void)config.Set("render.hardwareAcceleration", bps::json::Value::Bool(wantGpu),
                     bps::ConfigScope::Runtime);
    auto &render = bps::rendering::RenderEngine::Instance();
    // Initialize() reads this very key, so a store that opened before the render
    // module did has already been obeyed and there is nothing to re-bind yet.
    if (render.Initialized())
        (void)render.Reload();
    out.changed = true;
    out.wantGpu = wantGpu;
    out.active = qstr(render.ActiveBackendName());
    return out;
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
    Q_UNUSED(jsEngine)

    // Kept only so a language change can re-translate the UI: retranslate()
    // re-evaluates every qsTr binding in the engine.
    instance().qmlEngine_ = engine;
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

QString SettingsService::theme() const
{
    // Before the engine is up (or on a fresh store) this is the engine's own
    // default, so the Theme singleton has a value to switch on from the start.
    return settings_ ? qstr(settings_->GetString("appearance.theme")) : QStringLiteral("dark");
}

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
    if (QStandardPaths::isTestModeEnabled()) {
        const QString sandbox = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!sandbox.isEmpty())
            dataDir_ = sandbox.toStdString();
    }
    const std::string baseline =
        platform.Filesystem().Join(platform.Paths().ExecutableDir(), "assets/settings.shipped.json");
    settings_ = std::make_shared<bs::AppSettings>(
        platform.Filesystem().Join(dataDir_, "settings.json"), baseline);
    if (auto loaded = settings_->Load(); !loaded.ok())
        report(tr("Your settings could not be read (%1). The defaults are in use.").arg(qstr(loaded.error().message)), QStringLiteral("warning"));

    // Whatever the user chose last time takes effect now, and every later change does the same.
    (void)bs::ApplyToEngine(*settings_, bps::adaptive::AdaptiveRuntime::Instance());
    {
        auto settings = settings_;   // shared: the worker holds the store alive
        runOffThread([settings] {
            const RenderBackendApply applied = ApplyRenderBackendPreference(*settings);
            (void)applied;
        });
    }
    settings_->Subscribe([this](const std::string &key) {
        refreshValues();
        if (key == "startup.launchAtLogin")
            applyLaunchAtLogin();
        else if (key == "appearance.language")
            announceLanguage();   // applies the locale AND says what it did
        applyEngineSettingsAsync();
        emit valueChanged(qstr(key));
        emit changed();
    });

    refreshValues();
    refreshHardware();
    applyLaunchAtLogin();
    applyLanguage();
    emit changed();

    std::vector<std::function<void()>> pending = std::move(pendingReady_);
    pendingReady_.clear();
    for (const auto &callback : pending)
        callback();
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
    if (!settings_)
        return 0;
    const int count = static_cast<int>(settings_->ResetAll());
    if (count > 0) {
        refreshValues();
        applyLaunchAtLogin();
        applyLanguage();
        emit changed();
        applyEngineSettingsAsync();
    }
    return count;
}

void SettingsService::whenReady(std::function<void()> callback)
{
    if (!callback)
        return;
    // Already open (the common case for anything built after boot): act now.
    if (ready()) {
        callback();
        return;
    }
    // Defer to the end of load(). If the store never opens, the callback simply
    // never runs - which is the honest outcome, not a read of defaults.
    pendingReady_.push_back(std::move(callback));
}

void SettingsService::whenReady(const QJSValue &callback)
{
    if (!callback.isCallable())
        return;
    // A QJSValue is implicitly shared and this is the GUI thread (where the
    // store loads), so holding it until the deferred call is safe.
    whenReady(std::function<void()>([callback]() { callback.call(); }));
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
    bs::AudioCounts audio;
    for (const QVariant &v : EngineBridge::instance().audioDevices()) {
        if (v.toMap().value(QStringLiteral("isInput")).toBool())
            ++audio.inputs;
        else
            ++audio.outputs;
    }
    runOffThread([this, audio] {
        const bs::HardwareReport report = bs::CurrentHardwareReport(audio);
        QVariantList rows;
        for (const bs::HardwareRow &r : report.rows)
            rows.append(QVariantMap{ { QStringLiteral("key"), qstr(r.key) }, { QStringLiteral("label"), qstr(r.label) },
                                     { QStringLiteral("value"), qstr(r.value) }, { QStringLiteral("ok"), r.ok } });

        QVariantList advice;
        for (const bs::AdviceRow &a : report.advice)
            advice.append(QVariantMap{ { QStringLiteral("key"), qstr(a.key) }, { QStringLiteral("severity"), qstr(a.severity) },
                                       { QStringLiteral("text"), qstr(a.text) } });

        const QString profile = qstr(report.recommendedProfile);
        const QString headline = qstr(report.headline);
        const QString detail = qstr(report.detail);
        const QString adviceSummary = qstr(report.adviceSummary);
        QMetaObject::invokeMethod(this, [this, rows, advice, profile, headline, detail, adviceSummary] {
            hardwareRows_ = rows;
            hardwareAdvice_ = advice;
            recommendedProfile_ = profile;
            hardwareHeadline_ = headline;
            hardwareDetail_ = detail;
            hardwareAdviceSummary_ = adviceSummary;
            emit hardwareChanged();
        }, Qt::QueuedConnection);
    });
}

void SettingsService::applyRecommendedProfile()
{
    if (!recommendedProfile_.isEmpty()) {
        setValue(QStringLiteral("resources.profile"), recommendedProfile_);
        return;
    }
    runOffThread([this] {
        bs::AudioCounts audio;
        for (const QVariant &v : EngineBridge::instance().audioDevices()) {
            if (v.toMap().value(QStringLiteral("isInput")).toBool())
                ++audio.inputs;
            else
                ++audio.outputs;
        }
        const QString profile = qstr(bs::CurrentHardwareReport(audio).recommendedProfile);
        if (profile.isEmpty())
            return;
        QMetaObject::invokeMethod(this, [this, profile] {
            recommendedProfile_ = profile;
            setValue(QStringLiteral("resources.profile"), profile);
        }, Qt::QueuedConnection);
    });
}

QVariantMap SettingsService::capsFor(const QString &profile) const
{
    const bs::ResourceCaps c = bs::AppSettings::CapsFor(profile.toStdString());
    return { { QStringLiteral("gpu"), c.gpuPct }, { QStringLiteral("cpu"), c.cpuPct } };
}

// ---------------------------------------------------------------------------
// Starting with the OS (the one setting only the app itself can carry out)
// ---------------------------------------------------------------------------

// The chosen language as a person reads it (the engine's own label for the
// choice) — what the change notice names.
QString SettingsService::languageLabel() const
{
    if (!settings_)
        return {};
    const QString current = qstr(settings_->GetString("appearance.language"));
    for (const bs::SettingChoice &c : bs::AppSettings::Find("appearance.language")->choices)
        if (qstr(c.value) == current)
            return qstr(c.label);
    return current;
}

// Answers whether a TRANSLATION was put in force (see announceLanguage).
bool SettingsService::applyLanguage()
{
    if (!settings_)
        return false;

    const QString tag = qstr(settings_->GetString("appearance.language"));
    const QLocale locale(tag.isEmpty() ? QStringLiteral("en-US") : tag);

    QLocale::setDefault(locale);

    if (translator_) {
        QCoreApplication::removeTranslator(translator_.get());
        translator_.reset();
    }
    const QString file = QStringLiteral("vgrpresenter_") + locale.name();
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString dirs[] = { appDir + QStringLiteral("/translations"), QStringLiteral(":/i18n") };
    bool translated = false;
    for (const QString &dir : dirs) {
        auto candidate = std::make_unique<QTranslator>();
        if (candidate->load(file, dir)) {
            QCoreApplication::installTranslator(candidate.get());
            translator_ = std::move(candidate);
            translated = true;
            break;
        }
    }

    // Re-evaluate every qsTr binding (a no-op when nothing was installed) and
    // tell locale-dependent formatters the locale moved.
    if (qmlEngine_)
        qmlEngine_->retranslate();
    ++localeRevision_;
    emit localeChanged();
    return translated;
}

void SettingsService::announceLanguage()
{
    const QString label = languageLabel();
    const QString message = applyLanguage()
        ? tr("Language set to %1 \u2014 dates, times and numbers follow it, and the interface is translated.").arg(label)
        : tr("Language set to %1. Dates, times and numbers follow it now; the interface text stays English until a translation ships with the build.").arg(label);
    EventBus::instance().notify(message, QStringLiteral("info"), tr("Settings"),
                                QStringLiteral("settings.language.changed"));
    EngineBridge::instance().log(QStringLiteral("info"), QStringLiteral("Settings"), message);
}

void SettingsService::applyLaunchAtLogin()
{
    if (!settings_)
        return;
    const bool enable = settings_->GetBool("startup.launchAtLogin");
    // One setting, one meaning on every OS; only WHERE the OS keeps its start-up list differs. (This was
    // Windows-only: on Linux and macOS the "Launch at login" switch saved its value and did nothing.)
#if defined(Q_OS_WIN)
    QSettings run(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"), QSettings::NativeFormat);
    const QString name = QStringLiteral("VGRPresenter");
    if (enable)
        run.setValue(name, QLatin1Char('"') + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + QLatin1Char('"'));
    else
        run.remove(name);
    run.sync();
    if (run.status() != QSettings::NoError)
        report(tr("Windows would not let the app change its start-up entry."), QStringLiteral("warning"));
#else
    // macOS: a LaunchAgent in ~/Library/LaunchAgents (RunAtLoad). Linux: an XDG autostart entry in
    // ~/.config/autostart (honoured by GNOME, KDE, XFCE, …). An AppImage's own path is a temporary
    // mount, so the entry points at the file the user launched ($APPIMAGE) when there is one.
    const QString exe = qEnvironmentVariable("APPIMAGE", QCoreApplication::applicationFilePath());
#  if defined(Q_OS_MACOS)
    const QString file = QDir::homePath() + QStringLiteral("/Library/LaunchAgents/com.vgr.presenter.plist");
    const QByteArray body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "  <key>Label</key><string>com.vgr.presenter</string>\n"
        "  <key>ProgramArguments</key><array><string>" + exe.toHtmlEscaped().toUtf8() + "</string></array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "</dict></plist>\n";
#  else
    const QString file = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                         + QStringLiteral("/autostart/vgrpresenter.desktop");
    const QByteArray body =
        "[Desktop Entry]\nType=Application\nName=VGR Presenter\n"
        "Exec=\"" + exe.toUtf8() + "\"\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
#  endif
    if (enable) {
        QDir().mkpath(QFileInfo(file).absolutePath());
        QFile out(file);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(body) != body.size())
            report(tr("The system would not let the app change its start-up entry."), QStringLiteral("warning"));
    } else if (QFile::exists(file) && !QFile::remove(file)) {
        report(tr("The system would not let the app change its start-up entry."), QStringLiteral("warning"));
    }
#endif
}

// ---------------------------------------------------------------------------
// Off the interface thread
// ---------------------------------------------------------------------------

void SettingsService::runOffThread(std::function<void()> fn)
{
    auto *runner = QThread::create(std::move(fn));
    connect(runner, &QThread::finished, runner, &QObject::deleteLater);
    runner->start();
}

void SettingsService::applyEngineSettingsAsync()
{
    auto settings = settings_;   // shared: the worker holds the store alive
    if (!settings)
        return;
    applyRequested_.store(true);
    if (applyQueued_.exchange(true))
        return;                  // a run is in flight; it will pick this up
    runOffThread([this, settings] {
        for (;;) {
            applyRequested_.store(false);
            const RenderBackendApply render = ApplyRenderBackendPreference(*settings);
            if (render.changed)
                QMetaObject::invokeMethod(this, [this, render] {
                    rendererInUse_ = render.active;
                    rendererOnGpu_ = render.active != QLatin1String("software") && render.active != QLatin1String("null")
                                    && !render.active.isEmpty();   // "directx11" or "rhi": any GPU backend
                    if (render.wantGpu && !rendererOnGpu_)
                        report(tr("Hardware rendering is on, but no usable GPU was found — frames are drawn in software."),
                               QStringLiteral("warning"));
                    else if (render.wantGpu)
                        report(tr("Hardware rendering is on — frames are drawn on the graphics card."),
                               QStringLiteral("info"));
                    else
                        report(tr("Hardware rendering is off — frames are drawn in software."),
                               QStringLiteral("info"));
                    emit changed();
                }, Qt::QueuedConnection);
            auto applied = bs::ApplyToEngine(*settings, bps::adaptive::AdaptiveRuntime::Instance());
            if (!applied.ok()) {
                const QString error = qstr(applied.error().message);
                QMetaObject::invokeMethod(this, [this, error] {
                    report(tr("The engine could not apply that setting (%1).").arg(error));
                }, Qt::QueuedConnection);
            }
            // A write that landed WHILE this apply was running is not lost:
            // apply again from the newest values, then settle.
            if (!applyRequested_.load())
                break;
        }
        applyQueued_.store(false);
    });
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

void SettingsService::openBackupDirectory()
{
    const QString dir = backupDirectory();
    if (dir.isEmpty())
        return;
    const std::string folder = dir.toStdString();
    runOffThread([this, dir, folder] {
        auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
        if (!fs.Exists(folder))   // never taken a backup: show an empty folder
            (void)fs.CreateDirectories(folder);
        // The shell hand-off is the interface thread's business (it is a
        // window operation) and it returns at once — nothing is waited for.
        QMetaObject::invokeMethod(this, [dir] {
            (void)QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        }, Qt::QueuedConnection);
    });
}

void SettingsService::showBackupInFolder(const QString &path)
{
    if (path.isEmpty())
        return;
    // Same posture as the other backup paths: only something this store's own
    // folder holds is revealed.
    const QString dir = backupDirectory();
    if (dir.isEmpty() || !path.startsWith(dir + QLatin1Char('/')))
        return;
    const std::string file = path.toStdString();
    runOffThread([this, dir, path, file] {
        if (!bps::platform::PlatformAccessor::Get().Filesystem().IsRegularFile(file))
            return;
        // The shell hand-off runs on the interface thread (it is a window
        // operation, and startDetached does not wait for what it starts).
        QMetaObject::invokeMethod(this, [dir, path] {
#if defined(Q_OS_WIN)
            // Explorer's /select highlights the file in its folder.
            (void)QProcess::startDetached(QStringLiteral("explorer.exe"),
                                           { QStringLiteral("/select,") + QDir::toNativeSeparators(path) });
#elif defined(Q_OS_MACOS)
            // Finder's -R reveals (selects) the file.
            (void)QProcess::startDetached(QStringLiteral("open"), { QStringLiteral("-R"), path });
#else
            // No portable "select a file" on Linux desktops — opening the
            // enclosing folder is still the honest answer to "where is this one?".
            (void)QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
        }, Qt::QueuedConnection);
    });
}

void SettingsService::backupShow(const QString &showFile)
{
    if (!settings_ || showFile.isEmpty())
        return;
    // Everything the worker needs, read ONCE here (see runOffThread).
    const std::string dir = backupDirectory().toStdString();
    const std::size_t keepLast = static_cast<std::size_t>(settings_->GetInt("backups.keepLast"));
    const std::string file = showFile.toStdString();
    const std::string show = QFileInfo(showFile).completeBaseName().toStdString();
    runOffThread([this, dir, keepLast, file, show] {
        const bs::BackupStore store(dir);
        auto made = store.CreateIfChanged(file);
        const bool copied = made.ok() && !made.value().empty();
        // Older copies of this show beyond "Keep last" go.
        if (copied)
            (void)store.Prune(show, keepLast);
        QMetaObject::invokeMethod(this, [this, copied] {
            emit backupMade(copied ? 1 : 0);
            emit backupsChanged();
        }, Qt::QueuedConnection);
    });
}

void SettingsService::requestBackups(const QString &showName)
{
    if (dataDir_.empty())
        return;
    const std::string dir = backupDirectory().toStdString();
    const std::string name = showName.toStdString();
    runOffThread([this, dir, name] {
        const bs::BackupStore store(dir);
        QVariantList out;
        for (const bs::BackupInfo &b : store.List(name))
            out.append(QVariantMap{ { QStringLiteral("path"), qstr(b.path) }, { QStringLiteral("stamp"), qstr(b.stamp) },
                                    { QStringLiteral("sizeBytes"), static_cast<qlonglong>(b.sizeBytes) } });
        QMetaObject::invokeMethod(this, [this, name, out] {
            emit backupsListed(qstr(name), out);
        }, Qt::QueuedConnection);
    });
}

void SettingsService::requestAllBackups()
{
    if (dataDir_.empty())
        return;
    const std::string dir = backupDirectory().toStdString();
    runOffThread([this, dir] {
        const bs::BackupStore store(dir);
        QVariantList out;
        for (const bs::BackupInfo &b : store.List()) {   // no show filter = every show's
            const QString show = qstr(b.show);
            // A category backup's file name is the category itself ("settings",
            // "overlays", "templates"); anything else is a show's copy.
            const QString category = (show == QLatin1String("settings") || show == QLatin1String("overlays")
                                      || show == QLatin1String("templates")) ? show : QStringLiteral("show");
            out.append(QVariantMap{ { QStringLiteral("path"), qstr(b.path) }, { QStringLiteral("show"), show },
                                    { QStringLiteral("category"), category },
                                    { QStringLiteral("stamp"), qstr(b.stamp) },
                                    { QStringLiteral("sizeBytes"), static_cast<qlonglong>(b.sizeBytes) } });
        }
        QMetaObject::invokeMethod(this, [this, out] { emit allBackupsListed(out); }, Qt::QueuedConnection);
    });
}

void SettingsService::backupCategories()
{
    if (!settings_ || dataDir_.empty())
        return;
    const std::string dir = backupDirectory().toStdString();
    const std::string dataDir = dataDir_;
    const std::size_t keepLast = static_cast<std::size_t>(settings_->GetInt("backups.keepLast"));
    struct Category { const char *key; const char *file; const char *name; };
    const Category categories[] = {
        { "backups.includeSettings", "settings.json", "settings" },
        { "backups.includeOverlays",  "overlays.json",  "overlays" },
        { "backups.includeTemplates", "templates.json", "templates" },
    };
    // Which categories are on, read here (the worker touches no member).
    std::vector<std::pair<std::string, std::string>> selected;   // { file, name }
    for (const Category &c : categories)
        if (settings_->GetBool(c.key))
            selected.emplace_back(c.file, c.name);
    runOffThread([this, dir, dataDir, keepLast, selected] {
        const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
        const bs::BackupStore store(dir);
        int made = 0;
        for (const auto &[file, name] : selected) {
            const std::string source = fs.Join(dataDir, file);
            if (!fs.IsRegularFile(source))
                continue;   // never written yet — nothing to copy
            if (auto copy = store.CreateIfChanged(source); copy.ok() && !copy.value().empty())
                ++made;
            (void)store.Prune(name, keepLast);
        }
        QMetaObject::invokeMethod(this, [this, made] {
            emit backupMade(made);
            emit backupsChanged();
        }, Qt::QueuedConnection);
    });
}

void SettingsService::restoreCategoryBackup(const QString &backupPath)
{
    if (backupPath.isEmpty() || dataDir_.empty())
        return;
    // Only a path inside this store's own folder is accepted (the same posture
    // deleteBackup takes), so a stray path can never be copied over anything.
    const QString dir = backupDirectory();
    if (dir.isEmpty() || !backupPath.startsWith(dir + QLatin1Char('/'))) {
        emit restoreFinished(false, QString());
        return;
    }
    // The backup's base name is "<name>__<stamp>": resolve the live file it is a
    // copy of, and refuse anything that is not one of the three categories.
    const QString stem = QFileInfo(backupPath).completeBaseName();
    const int sep = stem.indexOf(QLatin1String("__"));
    const QString name = sep >= 0 ? stem.left(sep) : stem;
    QString live;
    if (name == QLatin1String("settings"))
        live = QStringLiteral("settings.json");
    else if (name == QLatin1String("overlays"))
        live = QStringLiteral("overlays.json");
    else if (name == QLatin1String("templates"))
        live = QStringLiteral("templates.json");
    else {
        emit restoreFinished(false, QString());   // a show backup goes through restoreBackup
        return;
    }
    const std::string source = backupPath.toStdString();
    const std::string target =
        bps::platform::PlatformAccessor::Get().Filesystem().Join(dataDir_, live.toStdString());
    auto settings = settings_;   // shared: the worker holds the store alive
    runOffThread([this, source, target, name, settings] {
        auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
        auto copied = fs.CopyAtomic(source, target);
        if (!copied.ok()) {
            const QString error = qstr(copied.error().message);
            QMetaObject::invokeMethod(this, [this, name, error] {
                report(error);
                emit restoreFinished(false, name);
            }, Qt::QueuedConnection);
            return;
        }
        // The re-read is a file read, so it belongs here too (only the values
        // and the bindings they feed have to wait for the interface thread).
        if (settings && name == QLatin1String("settings"))
            (void)settings->Load();
        QMetaObject::invokeMethod(this, [this, name] {
            // Bring what reads the file back in line with what was restored.
            if (name == QLatin1String("settings")) {
                refreshValues();
                applyLaunchAtLogin();
                applyLanguage();
                emit changed();
                // The engine's copy of the restored values, off-thread and
                // coalesced (see applyEngineSettingsAsync).
                applyEngineSettingsAsync();
            } else if (name == QLatin1String("overlays")) {
                DesignLibraryService::overlays().reload();
            } else {
                DesignLibraryService::templates().reload();
            }
            emit restoreFinished(true, name);
        }, Qt::QueuedConnection);
    });
}

void SettingsService::deleteBackup(const QString &path)
{
    if (dataDir_.empty() || path.isEmpty())
        return;
    const std::string dir = backupDirectory().toStdString();
    const std::string victim = path.toStdString();
    runOffThread([this, dir, victim] {
        const bs::BackupStore store(dir);
        auto removed = store.Remove(victim);
        const QString error = removed.ok() ? QString() : qstr(removed.error().message);
        QMetaObject::invokeMethod(this, [this, error] {
            if (!error.isEmpty())
                report(error);
            else
                emit backupsChanged();
        }, Qt::QueuedConnection);
    });
}

QString SettingsService::recoveryPath() const
{
    if (dataDir_.empty())
        return {};
    const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    return qstr(bs::RecoveryStore(fs.Join(dataDir_, "recovery")).Path());
}

void SettingsService::noteRecovery(const QString &showName, const QString &originalPath)
{
    if (dataDir_.empty())
        return;
    const std::string dataDir = dataDir_;
    const std::string name = showName.toStdString();
    const std::string original = originalPath.toStdString();
    runOffThread([dataDir, name, original] {
        const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
        (void)bs::RecoveryStore(fs.Join(dataDir, "recovery")).Note(name, original);
    });
}

void SettingsService::requestPendingRecovery()
{
    if (dataDir_.empty()) {
        emit pendingRecoveryReady({});
        return;
    }
    const std::string dataDir = dataDir_;
    runOffThread([this, dataDir] {
        const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
        const auto info = bs::RecoveryStore(fs.Join(dataDir, "recovery")).Info();
        QVariantMap out;
        if (info)
            out = { { QStringLiteral("path"), qstr(info->path) }, { QStringLiteral("showName"), qstr(info->showName) },
                    { QStringLiteral("originalPath"), qstr(info->originalPath) } };
        QMetaObject::invokeMethod(this, [this, out] { emit pendingRecoveryReady(out); }, Qt::QueuedConnection);
    });
}

void SettingsService::clearRecovery()
{
    if (dataDir_.empty())
        return;
    const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
    (void)bs::RecoveryStore(fs.Join(dataDir_, "recovery")).Clear();
}

void SettingsService::restoreBackup(const QString &backupPath, const QString &target)
{
    if (backupPath.isEmpty() || target.isEmpty())
        return;
    const std::string source = backupPath.toStdString();
    const std::string dest = target.toStdString();
    const QString label = target;
    runOffThread([this, source, dest, label] {
        auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
        // Never over an existing file: a restore ADDS a show, it does not replace one.
        if (fs.Exists(dest)) {
            QMetaObject::invokeMethod(this, [this, label] {
                report(tr("There is already a file with that name."));
                emit restoreFinished(false, label);
            }, Qt::QueuedConnection);
            return;
        }
        auto copied = fs.CopyAtomic(source, dest);
        const QString error = copied.ok() ? QString() : qstr(copied.error().message);
        QMetaObject::invokeMethod(this, [this, error, label] {
            if (!error.isEmpty())
                report(error);
            emit restoreFinished(error.isEmpty(), label);
        }, Qt::QueuedConnection);
    });
}

void SettingsService::restoreRecovery(const QString &target)
{
    if (dataDir_.empty())
        return;
    const std::string dataDir = dataDir_;
    const std::string dest = target.toStdString();
    const QString label = target;
    runOffThread([this, dataDir, dest, label] {
        const auto &fs = bps::platform::PlatformAccessor::Get().Filesystem();
        auto restored = bs::RecoveryStore(fs.Join(dataDir, "recovery")).RestoreTo(dest);
        const QString error = restored.ok() ? QString() : qstr(restored.error().message);
        QMetaObject::invokeMethod(this, [this, error, label] {
            if (!error.isEmpty())
                report(error);
            emit restoreFinished(error.isEmpty(), label);
        }, Qt::QueuedConnection);
    });
}

bool SettingsService::fileExists(const QString &path) const
{
    return !path.isEmpty() && bps::platform::PlatformAccessor::Get().Filesystem().IsRegularFile(path.toStdString());
}

bps::presentation::BookFillOptions SettingsService::scriptureSettings() const
{
    return settings_ ? settings_->Scripture() : bps::presentation::BookFillOptions{};
}

bps::presentation::BookFillOptions SettingsService::theTableSettings() const
{
    return settings_ ? settings_->TheTable() : bps::presentation::BookFillOptions{};
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
    const std::vector<std::string> &bufferNames,
    const std::vector<std::pair<int, int>> &bufferSizes)
{
    const bool booted = EngineBridge::instance().booted();
    qInfo("SettingsService: live output styles push booted=%d specs=%d buffers=%d",
          booted ? 1 : 0, static_cast<int>(specs.size()), static_cast<int>(bufferNames.size()));
    if (booted) {
        const auto result = bps::presentation::PresentationEngine::Instance().SetLiveOutputStyles(specs, bufferNames, bufferSizes);
        if (!result.ok())
            qWarning("SettingsService: live output styles push failed: %s", result.error().message.c_str());
    }
}