#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include "core/kernel/Kernel.hpp"
#include "core/logging/Logger.hpp"
#include "core/events/Events.hpp"
#include "modules/project/UndoRedoManager.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"
#include "modules/production/AudioMixer.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "modules/display/DisplayEngine.hpp"
#include "modules/display/Providers.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "services/ShowConverter.h"
#include "models/AudioInputListModel.h"
#include "models/VideoSourceListModel.h"
#include "platform/PlatformAccessor.hpp"
#include "services/LiveOutputService.h"
#include "services/SettingsService.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QStringList>
#include <QQmlEngine>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QThreadPool>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <mutex>

#include <atomic>

namespace {

struct PowerToastState {
    std::mutex mutex;
    int lastPercent = -1;
    bool lastOnBattery = false;
    bool initialized = false;
};

// Wraps two QML callables as one bps::project::ICommand — the engine's
// stack only ever sees a Label()/Execute()/Undo(), never that the actual
// steps are QML functions.
class QmlCommand final : public bps::project::ICommand
{
public:
    QmlCommand(QString label, QJSValue doFn, QJSValue undoFn)
        : label_(std::move(label)), do_(std::move(doFn)), undo_(std::move(undoFn))
    {
    }

    void Execute() override { run(do_, "redo"); }
    void Undo() override { run(undo_, "undo"); }

    std::string Label() const override { return label_.toStdString(); }

private:
    // A throwing closure must not vanish: the engine moves the entry between
    // its stacks regardless, so a silent failure desyncs stack and canvas.
    // qWarning also surfaces as a toast via the app's message handler.
    void run(QJSValue &fn, const char *what)
    {
        if (!fn.isCallable())
            return;
        const QJSValue result = fn.call();
        if (result.isError())
            qWarning("Undo command '%s' (%s) failed: %s:%d: %s", qUtf8Printable(label_), what,
                     qUtf8Printable(result.property(QStringLiteral("fileName")).toString()),
                     result.property(QStringLiteral("lineNumber")).toInt(),
                     qUtf8Printable(result.toString()));
    }

    QString label_;
    QJSValue do_;
    QJSValue undo_;
};

// Every entry holds TWO whole-canvas snapshots inside QJSValue closures, so the
// engine's default depth (1000) is far too generous — the old UI-side history
// this replaced capped at 50.
constexpr size_t kMaxUndoDepth = 100;

bps::project::UndoRedoManager &Manager()
{
    return bps::project::UndoRedoManager::Instance();
}

} // namespace

EngineBridge::EngineBridge(QObject *parent)
    : QObject(parent)
{
}

EngineBridge &EngineBridge::instance()
{
    static EngineBridge bridge;
    return bridge;
}

EngineBridge *EngineBridge::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

bool EngineBridge::canUndo() const
{
    return Manager().CanUndo();
}

bool EngineBridge::canRedo() const
{
    return Manager().CanRedo();
}

bool EngineBridge::booted() const
{
    return bps::Kernel::Instance().State() == bps::KernelState::Running;
}

bool EngineBridge::boot()
{
    if (booted())
        return true;
    deviceEnumerationStopping_ = false;

    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString logDir = base + QStringLiteral("/logs");
    const QString dataDir = base + QStringLiteral("/enginedata");
    QDir().mkpath(logDir);
    QDir().mkpath(dataDir);

    // Installed BEFORE Boot() so Logger::Initialize()'s own default-sink path
    // (a relative "logs/engine.log", resolved against the process's current
    // working directory — fine for our own build-dir test runs, fragile for
    // a real double-clicked/shortcut launch where the CWD is unpredictable)
    // never gets added: Initialize() only seeds defaults `if (sinks_.empty())`.
    // Once per process: a failed boot can be retried, and a second AddSink would
    // write every engine line to engine.log twice.
    //
    // The log lives next to the program (<app folder>/logs/engine.log) so it is easy to find and tail; when that folder can't be
    // written (an install under Program Files), and in a sandboxed run, it falls back to the user's app-data folder.
    static bool fileSinkInstalled = false;
    if (!fileSinkInstalled) {
        fileSinkInstalled = true;
        QString dir = logDir;
        if (!QStandardPaths::isTestModeEnabled()) {
            const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
            QDir().mkpath(beside);
            QFile probe(beside + QStringLiteral("/.writable"));
            if (probe.open(QIODevice::WriteOnly)) {
                probe.close();
                probe.remove();
                dir = beside;
            }
        }
        logPath_ = QDir::toNativeSeparators(QDir(dir).absoluteFilePath(QStringLiteral("engine.log")));
        (void)bps::Logger::Instance().AddSink(std::make_shared<bps::FileSink>(logPath_.toStdString()));
    }

    bps::BootOptions options;
    options.dataDir = dataDir.toStdString();
    options.logLevel = bps::LogLevel::Info;
    // pluginDirs left empty, ipcPort left 0 (its BootOptions default) —
    // no plugin loading, no network listener opened.

    Manager().SetLimit(kMaxUndoDepth);

    auto result = bps::Kernel::Instance().Boot(options);
    emit bootedChanged();

    if (!result.ok()) {
        bootError_ = QString::fromStdString(result.error().message);
        emit bootErrorChanged();
        EventBus::instance().publish(QStringLiteral("engine.boot"), QVariantMap{
            {QStringLiteral("level"), QStringLiteral("error")},
            {QStringLiteral("title"), QStringLiteral("Engine")},
            {QStringLiteral("message"), QStringLiteral("Engine failed to boot: ") + bootError_},
        });
        return false;
    }

    write(QStringLiteral("info"), QStringLiteral("App"),
          QStringLiteral("---- %1 started (pid %2) - engine log: %3 - data: %4")
              .arg(QCoreApplication::applicationName()).arg(QCoreApplication::applicationPid())
              .arg(logPath_, QDir::toNativeSeparators(dataDir)));

    // Kernel is up — open the Engine → UI relay (engine log warnings/errors
    // + the curated engine-event set flow into the UI's EventBus and the
    // engineEvent signal from here on). Started AFTER a successful Boot so
    // subscribers can never observe a half-initialized kernel; stopped in
    // shutdown() BEFORE Kernel::Shutdown() tears its systems down.
    startRelay();

    // Real hardware roster, straight from the PAL — the AV board's device
    // selects read audioDevices/screenDevices instead of mock lists, and the
    // hot-plug subscriptions above keep them current from here on.
    enumerateDevices();

    // (The firewall ask no longer reads a persisted flag here — it gates on
    // the rule itself; see requestNdiFirewallAccess.)

    // The Plugins screen's switches are SETTINGS, not just UI state: replay
    // the persisted feature states into the engine's registry once, after
    // boot's auto-evaluation has run (so a user's explicit ON outranks the
    // runtime's auto-OFF, matching the screen's own mental model), then
    // publish. A switch the user flipped last session re-applies here.
    if (SettingsService *settings = SettingsService::instancePtr()) {
        const QString blob = settings->value(QStringLiteral("plugins.featureStates")).toString();
        if (!blob.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8());
            if (doc.isObject()) {
                auto &runtime = bps::adaptive::AdaptiveRuntime::Instance();
                const QJsonObject states = doc.object();
                for (auto it = states.begin(); it != states.end(); ++it) {
                    (void)runtime.SetFeatureEnabled(it.key().toStdString(), it.value().toBool());
                    // The broadcast gates ride the registry: replay the NDI
                    // and SDI entries into the engine before anything touches
                    // the network/capture stacks.
                    if (it.key().compare(QLatin1String("ndi"), Qt::CaseInsensitive) == 0)
                        (void)bps::broadcast::BroadcastEngine::Instance().SetNdiEnabled(
                            it.value().toBool());
                    else if (it.key().compare(QLatin1String("sdi"), Qt::CaseInsensitive) == 0)
                        (void)bps::broadcast::BroadcastEngine::Instance().SetSdiEnabled(
                            it.value().toBool());
                }
            }
        }
    }
    // UNCONDITIONAL sync, even with nothing saved yet (a fresh install):
    // BroadcastEngine::ndiEnabled_ is its OWN independent flag, not read
    // from the registry automatically — without this, a fresh install
    // could show the Plugins toggle off (the registry's real default) while
    // the broadcast stack's actual gate stayed at ITS OWN separate default,
    // letting NDI genuinely send/receive despite the switch reading off.
    // Every other NDI entry point (taps, sends, discovery) already checks
    // BroadcastEngine::ndiEnabled_ centrally, so keeping THIS one flag
    // truthful is what makes that check mean anything.
    {
        auto ndiState = bps::adaptive::AdaptiveRuntime::Instance().GetFeatureState("ndi");
        const bool ndiOn = ndiState.ok() && ndiState.value() == bps::adaptive::FeatureState::Enabled;
        bps::broadcast::BroadcastEngine::Instance().SetNdiEnabled(ndiOn);
    }
    // Same unconditional sync for sdi: BroadcastEngine's gate is its OWN
    // flag (default off — the spam-proof state), and the registry's default
    // for a fresh install is off too, so both agree. A user who switched ON
    // re-applies via the replay above.
    {
        auto sdiState = bps::adaptive::AdaptiveRuntime::Instance().GetFeatureState("sdi");
        const bool sdiOn = sdiState.ok() && sdiState.value() == bps::adaptive::FeatureState::Enabled;
        bps::broadcast::BroadcastEngine::Instance().SetSdiEnabled(sdiOn);
    }
    emit pluginFeaturesChanged();

    // REPLAY the kernel's CURRENT state into the relay. The Booting → Running
    // transition was published on the engine's bus DURING Boot() — before any
    // relay subscriber existed — so without this replay every consumer's
    // default assumption ("Booting") stuck forever (the header showed
    // "Booting" long after boot finished). Ingesting the live state as a
    // state_changed event gives late subscribers exactly what the original
    // transition would have delivered, ring backfill included.
    {
        const QString state = QString::fromLatin1(
            bps::ToString(bps::Kernel::Instance().State()));
        ingestEngineEvent(QStringLiteral("engine.kernel.state_changed"), QVariantMap{
            {QStringLiteral("level"), state == QStringLiteral("Running")
                 ? QStringLiteral("success") : QStringLiteral("info")},
            {QStringLiteral("title"), QStringLiteral("Engine")},
            {QStringLiteral("message"), QStringLiteral("Kernel state: %1").arg(state)},
            {QStringLiteral("to"), state},
        });
    }

    // REPLAY the boot walk's holes, for the same reason as the state replay
    // above: SubsystemDegraded fired during Boot(), before any relay subscriber
    // existed. A degraded session must not LOOK clean on the way up.
    for (const auto &d : bps::Kernel::Instance().Degraded()) {
        ingestEngineEvent(QStringLiteral("engine.kernel.subsystem_degraded"), QVariantMap{
            {QStringLiteral("level"), QStringLiteral("warning")},
            {QStringLiteral("title"), QStringLiteral("Engine")},
            {QStringLiteral("message"),
             QStringLiteral("%1 is unavailable this session — %2")
                 .arg(QString::fromStdString(d.system), QString::fromStdString(d.message))},
            {QStringLiteral("system"), QString::fromStdString(d.system)},
        });
    }

    if (!bootError_.isEmpty()) {
        bootError_.clear();
        emit bootErrorChanged();   // a retried boot succeeded — clear the flag
    }
    // The worker updates this summary again when the real hardware roster
    // arrives; the first toast must not wait for camera capability probing.
    updateBootSummary();
    EventBus::instance().publish(QStringLiteral("engine.boot"), QVariantMap{
        {QStringLiteral("level"), QStringLiteral("success")},
        {QStringLiteral("title"), QStringLiteral("Engine")},
        {QStringLiteral("message"),
         bps::Kernel::Instance().Degraded().empty()
             ? QStringLiteral("Presentation engine booted (%1 systems).")
                   .arg(bps::Kernel::Instance().BootLog().size())
             : QStringLiteral("Presentation engine booted (%1 systems, %2 unavailable).")
                   .arg(bps::Kernel::Instance().BootLog().size())
                   .arg(bps::Kernel::Instance().Degraded().size())},
    });
    return true;
}

QString EngineBridge::bootError() const
{
    return bootError_;
}

void EngineBridge::write(const QString &level, const QString &source, const QString &message)
{
    bps::LogLevel lv = bps::LogLevel::Info;
    if (level == QLatin1String("trace"))        lv = bps::LogLevel::Trace;
    else if (level == QLatin1String("debug"))   lv = bps::LogLevel::Debug;
    else if (level == QLatin1String("warning")) lv = bps::LogLevel::Warning;
    else if (level == QLatin1String("error"))   lv = bps::LogLevel::Error;
    bps::Logger::Instance().Log(lv, "UI", source.toStdString(), message.toStdString());
}

void EngineBridge::log(const QString &level, const QString &source, const QString &message)
{
    write(level, source, message);
}

QStringList EngineBridge::bootLog() const
{
    QStringList out;
    for (const auto &s : bps::Kernel::Instance().BootLog())
        out << QString::fromStdString(s);
    return out;
}

QString EngineBridge::health() const
{
    return QString::fromStdString(bps::Kernel::Instance().GetHealth().detail);
}

void EngineBridge::shutdown()
{
    if (!booted())
        return;
    deviceEnumerationStopping_ = true;
    deviceEnumerationPending_ = false;
    // BOUNDED join: the worker may be inside a platform enumeration (Media
    // Foundation touching camera drivers) that can stall. An unbounded
    // waitForDone() here froze the GUI thread at quit — Windows reports that
    // as AppHangB1 and kills the process, which is what "it crashes when I
    // close it" looked like. The normal case is milliseconds; the bound only
    // changes the pathological one. A late worker is already harmless: its
    // completion is a queued call that drops on deviceEnumerationStopping_.
    (void)deviceEnumerationPool_.waitForDone(2000);
    // Detach every engine-thread producer FIRST — the relay's Logger sink and
    // EventBus subscriptions run on engine threads and must not fire into the
    // UI while (or after) Kernel::Shutdown() is dismantling those systems.
    stopRelay();
    // Meter taps run their own WASAPI threads and must be joined before the
    // PAL's platform backend is torn down underneath them. Same for the
    // camera preview taps (MF Source Reader drain threads).
    stopAllInputMeters();
    stopAllVideoPreviews();
    // The undo stack holds QJSValue closures inside the engine's static
    // UndoRedoManager, which outlives the QML engine — drop them now (aboutToQuit,
    // JS engine still alive) rather than at static destruction.
    Manager().Clear();
    (void)bps::Kernel::Instance().Shutdown();
    emit bootedChanged();
}

void EngineBridge::pushCommand(const QString &label, const QJSValue &doFn, const QJSValue &undoFn)
{
    auto cmd = std::make_shared<QmlCommand>(label, doFn, undoFn);
    (void)Manager().ExecuteCommand(cmd);
    emit stackChanged();
}

bool EngineBridge::undo()
{
    const bool ok = Manager().Undo().ok();
    emit stackChanged();
    return ok;
}

bool EngineBridge::redo()
{
    const bool ok = Manager().Redo().ok();
    emit stackChanged();
    return ok;
}

QString EngineBridge::undoLabel() const
{
    return QString::fromStdString(Manager().UndoLabel());
}

QString EngineBridge::redoLabel() const
{
    return QString::fromStdString(Manager().RedoLabel());
}

void EngineBridge::clearHistory()
{
    Manager().Clear();
    emit stackChanged();
}

// ===========================================================================
// Engine → UI relay — "the kernel drives everything, the UI relays."
//
// Two producers, one ingestion path (ingestEngineEvent):
//
//  1. EngineLogSink — an engine Logger ISink. EVERY engine log record at
//     Warning level or above (engine errors, subscriber exceptions, failed
//     subsystem init, panics...) flows into the UI bus as "engine.log" —
//     a toast when it carries a message, and always a subscriber event.
//
//  2. Curated bps::EventBus subscriptions — the engine's own decoupled
//     event backbone (docs/specs/05). Per-topic subscribers for the
//     user-facing set (kernel/display/render/recording/media/broadcast/
//     project/undo-redo...), each formatted into level/title/message and
//     republished under its VERBATIM engine topic ("recording.failed",
//     "project.undo_performed"), so QML can filter on topic strings that
//     match the engine's documentation exactly.
//
// Threading: the engine's Logger pump and EventBus dispatch run on ENGINE
// threads. The ISink::Write and subscription callbacks therefore do nothing
// but copy data into a lambda and QMetaObject::invokeMethod(this, ...,
// Qt::QueuedConnection) it to the GUI thread, where ingestEngineEvent does
// all UI touching. stopRelay() runs before Kernel::Shutdown() so the
// subscriptions can't fire during teardown.
// ===========================================================================

namespace {

// Forwards every engine log record ≥ Warning into the UI. Sinks are called
// on the Logger's pump thread — never touch UI state here.
class EngineLogSink final : public bps::ISink
{
public:
    explicit EngineLogSink(EngineBridge *bridge) : bridge_(bridge)
    {
        // The Logger honors per-sink minimums (default Trace) — without this every
        // Info line the engine writes would reach Write() and become a toast.
        SetMinLevel(bps::LogLevel::Warning);
    }

    const char *Name() const noexcept override { return "UiRelay"; }

    void Write(std::string_view formatted, const bps::LogRecord &record) override
    {
        Q_UNUSED(formatted)
        if (!bridge_ || record.level < bps::LogLevel::Warning)
            return;
        // The Notification Service echoes every notification it raises to the log (category
        // "Notify"). Each of those already reaches the UI as a properly worded toast through
        // the typed relay below, so re-reporting the log line produced a second, raw toast
        // ("Engine · Core  [Warning][Performance] ...") for every event.
        if (record.category == "Notify")
            return;
        // Lines the UI itself wrote (EngineBridge::write) are for the log file: the UI already showed its own toast for them,
        // so relaying them back would show every one twice.
        if (record.module == "UI")
            return;
        QString level;
        switch (record.level) {
        case bps::LogLevel::Warning: level = QStringLiteral("warning"); break;
        case bps::LogLevel::Error:   level = QStringLiteral("error");   break;
        default:                     level = QStringLiteral("error");   break; // Fatal
        }
        const QString module = QString::fromStdString(record.module);
        const QString message = QString::fromStdString(record.message);
        QMetaObject::invokeMethod(bridge_, [bridge = QPointer<EngineBridge>(bridge_), module, message, level]() {
            if (bridge)
                bridge->ingestEngineEvent(QStringLiteral("engine.log"), QVariantMap{
                    {QStringLiteral("level"), level},
                    {QStringLiteral("title"), QStringLiteral("Engine · %1").arg(module)},
                    {QStringLiteral("message"), message},
                });
        }, Qt::QueuedConnection);
    }

private:
    EngineBridge *bridge_;   // raw — EngineBridge owns this sink's lifetime
};

QString qstr(const std::string &s) { return QString::fromStdString(s); }

// The used-device labels QML handed over, in the PAL's own vocabulary
// (std::string friendly names), for the TARGETED enumerations below.
std::vector<std::string> namesOf(const QStringList &labels)
{
    std::vector<std::string> out;
    out.reserve(size_t(labels.size()));
    for (const QString &l : labels) {
        const std::string name = l.trimmed().toStdString();
        if (!name.empty())
            out.push_back(name);
    }
    return out;
}

} // namespace

// Device APIs can block in COM and hardware drivers, especially Media
// Foundation's per-camera capability probe. Keep those calls off the GUI
// thread and publish only value snapshots back on the UI thread.
void EngineBridge::enumeratePlatformDevicesAsync()
{
    if (deviceEnumerationStopping_)
        return;
    if (deviceEnumerationBusy_) {
        deviceEnumerationPending_ = true;
        return;
    }
    if (!bps::platform::PlatformAccessor::Installed()) {
        audioDevices_.clear();
        screenDevices_.clear();
        videoDevices_.clear();
        emit devicesChanged();
        return;
    }

    deviceEnumerationBusy_ = true;
    QPointer<EngineBridge> guard(this);
    // Snapshot the filter ON THE GUI THREAD: the worker must not read these
    // while a dialog is opening on the other side of it (and the answer has to
    // be the one this sweep was launched with).
    const bool onlyUsed = usedDeviceSweepOnly_;
    // THE SETUP'S OWN DEVICE SET — what the boot report's missing-device check is
    // about, and never the lights' questions below: a status light asking about a
    // source is not a device this setup was configured with, and letting its
    // label into this list made the check count (and then report as MISSING) a
    // device nobody configured.
    const QStringList usedAudio = usedAudioLabels_;
    const QStringList usedVideo = usedVideoLabels_;
    const QStringList usedDisplay = usedDisplayNames_;
    // WHAT THIS SWEEP WALKS: the setup's set ...
    QStringList wantedAudio = usedAudio;
    QStringList wantedVideo = usedVideo;
    QStringList wantedDisplay = usedDisplay;
    // ...PLUS WHAT A STATUS LIGHT IS ASKING ABOUT (see inputSourceAlive). A
    // light names the source its bus routed, and that source is the one device
    // this sweep must come back with an answer for — the used-device set cannot
    // be trusted to carry it (a roster that hands its set over late, a source
    // routed after boot, a row whose kind the hand-over classifies differently)
    // and without this the light sat on "not there" no matter how long the
    // operator waited. The label rides in EVERY list on purpose: a light knows
    // the source's LABEL, not which roster its kind maps to, and naming a window
    // in the audio set (or a mic in the display set) simply matches nothing —
    // each walk only ever asks for what exists, so this can only add the device
    // the light is about. The asks are consumed here: one sweep answers them.
    if (!lightAsks_.isEmpty()) {
        for (const QString &ask : lightAsks_) {
            const QString label = ask.trimmed();
            if (label.isEmpty())
                continue;
            wantedAudio << label;
            wantedVideo << label;
            wantedDisplay << label;
        }
        lightAsks_.clear();
    }
    // The sweep that "carries the app's own device set" is the first one taken
    // AFTER the app has handed that set over — targeted (the normal case) or
    // full (a picker beat it to the machine, see refreshDevices). It is never
    // boot()'s opening enumeration, which runs before the hand-over and
    // therefore knows nothing about this setup's hardware: that one is a layout
    // aid (the primary display), not a device report. Read on the GUI thread at
    // launch time, so a sweep already in flight when the hand-over arrives
    // keeps whatever answer it was launched with. See bootSweepDone.
    const bool bootSweep = deviceHandoverSeen_ || !onlyUsed;
    deviceEnumerationPool_.start([guard, onlyUsed, bootSweep, usedAudio, usedVideo, usedDisplay,
                                  wantedAudio, wantedVideo, wantedDisplay]() {
        QVariantList audioDevices;
        QVariantList screenDevices;
        QVariantList videoDevices;
        if (bps::platform::PlatformAccessor::Installed()) {
            auto &platform = bps::platform::PlatformAccessor::Get();
        // USED-DEVICE SWEEP (see requestUsedDevices): the boot sweep ASKS the
        // platform for the endpoints this setup names — EnumerateFor walks to
        // those and stops. It is NOT "enumerate the machine, then drop what is
        // left over": activating a WASAPI endpoint + querying its mix format,
        // and opening a camera to walk its media types, is the entire cost of
        // an enumeration, and paying it to throw the answer away is exactly
        // what made boot slow. A picker opening drops the filter (see
        // refreshDevices/refreshAudioDevices, which set usedDeviceSweepOnly_ to
        // false) and then the full Enumerate() runs — that is where a NEW
        // device has to come from, and the only moment the whole machine is
        // legitimate.
        const auto audioRoster = onlyUsed
            ? platform.Audio().EnumerateFor(namesOf(wantedAudio))
            : platform.Audio().Enumerate();
        for (const auto &d : audioRoster) {
            const QString name = qstr(d.name);
            audioDevices.append(QVariantMap{
                {QStringLiteral("id"), qstr(d.id)},
                {QStringLiteral("label"), name},
                {QStringLiteral("value"), name},   // AV board stores the readable sublabel
                {QStringLiteral("isInput"), d.isInput},
                {QStringLiteral("isDefault"), d.isDefault},
                // Real endpoint truth from WASAPI (0 = unknown): the
                // dialogs' Channels rows and meter strip-count read these.
                {QStringLiteral("channels"), int(d.channels)},
                {QStringLiteral("sampleRateHz"), int(d.sampleRateHz)},
            });
        }
        // Displays and open windows are the one part of the boot sweep that is
        // still a filter rather than a targeted ask, and deliberately so: there
        // is nothing to ask FOR. QueryDisplayConfig answers for every active
        // path in a single call (there is no "just this monitor" query), and a
        // window's title — the only thing a saved row can name it by — can only
        // be learned by walking the top-level windows. The filter below
        // therefore keeps the cost of a monitor/window roster from reaching the
        // rest of the app (the UI lists, the layout math), not from the OS.
        for (const auto &m : platform.Monitor().Enumerate()) {
            const QString name = qstr(m.name);
            // The PRIMARY display always stays: it is what the app's own window
            // sizes itself against (primaryDisplay). Every other monitor is kept
            // only when an output is bound to it.
            if (onlyUsed && !m.primary
                && !labelWanted(wantedDisplay, name.isEmpty() ? qstr(m.id) : name))
                continue;
            screenDevices.append(QVariantMap{
                {QStringLiteral("id"), qstr(m.id)},
                {QStringLiteral("label"), name.isEmpty() ? qstr(m.id) : name},
                {QStringLiteral("value"), name.isEmpty() ? qstr(m.id) : name},
                {QStringLiteral("primary"), m.primary},
                {QStringLiteral("widthPx"), m.widthPx},
                {QStringLiteral("heightPx"), m.heightPx},
            });
        }
        // Open application WINDOWS join the same screen roster (OBS-style
        // Window Capture): id "win:<hwnd>", label = the window title. The
        // pane/thumb path already resolves roster labels → ids, so window
        // capture rides the exact same tap + provider chain as displays.
        // Re-enumerated on every enumerateDevices (dialog opens) — stale
        // hwnd ids self-heal on the next pick.
        for (const auto &w : platform.Video().EnumerateWindows()) {
            const QString title = qstr(w.title);
            if (title.isEmpty())
                continue;
            // Open windows are the most expensive half of the screen roster
            // (every top-level window, enumerated and titled) and none of them
            // is in use unless a row captured one — so the boot sweep only
            // keeps the windows the roster names.
            if (onlyUsed && !labelWanted(wantedDisplay, title))
                continue;
            screenDevices.append(QVariantMap{
                {QStringLiteral("id"), qstr(w.id)},
                {QStringLiteral("label"), title},
                {QStringLiteral("value"), title},
                {QStringLiteral("primary"), false},
                {QStringLiteral("widthPx"), 0},
                {QStringLiteral("heightPx"), 0},
            });
        }
        // Video capture: REAL device + mode lists from the PAL's IVideo
        // (Media Foundation on Windows). Mode labels are OBS-style
        // "<W>x<H>p<FPS>" (29.97 kept fractional); maxFps is the device's
        // own ceiling — the UI greys any offered mode faster than it.
        // Same targeted walk as audio above: only the cameras the video roster
        // names are ever activated, so a machine with four cameras and one
        // configured opens one.
        const auto videoRoster = onlyUsed
            ? platform.Video().EnumerateFor(namesOf(wantedVideo))
            : platform.Video().Enumerate();
        for (const auto &d : videoRoster) {
            const QString name = qstr(d.name);
            QStringList modes;
            for (const auto &m : d.modes) {
                for (const uint32_t fps : m.fpsRates) {
                    modes.append(QStringLiteral("%1x%2p%3")
                                     .arg(m.width)
                                     .arg(m.height)
                                     .arg(fps % 10 == 7 && fps > 20
                                              ? QString::number(fps / 10.0, 'f', 2)
                                              : QString::number(fps)));
                }
            }
            videoDevices.append(QVariantMap{
                {QStringLiteral("id"), qstr(d.id)},
                {QStringLiteral("label"), name},
                {QStringLiteral("value"), name},   // AV board stores the readable sublabel
                {QStringLiteral("modes"), modes},
                {QStringLiteral("maxFps"), int(d.maxFps)},
            });
        }
        }

        QMetaObject::invokeMethod(guard,
            [guard, onlyUsed, bootSweep, usedAudio, usedVideo, usedDisplay,
             audioDevices = std::move(audioDevices),
             screenDevices = std::move(screenDevices),
             videoDevices = std::move(videoDevices)]() mutable {
                if (!guard)
                    return;
                EngineBridge *self = guard.data();
                self->deviceEnumerationBusy_ = false;
                if (!self->deviceEnumerationStopping_) {
                    self->audioDevices_ = std::move(audioDevices);
                    self->screenDevices_ = std::move(screenDevices);
                    self->videoDevices_ = std::move(videoDevices);
                    // The other half of the used-device sweep: a device the app
                    // was set up with that is NOT there this time is said out
                    // loud, once, right where the user can act on it — instead
                    // of a row that silently meters nothing / previews black.
                    //
                    // The gate is "this sweep actually carried a used set"
                    // (bootSweep with something wanted): boot() runs one
                    // enumeration BEFORE QML hands the used devices over, and
                    // letting THAT consume the one-shot turned the check into
                    // dead code — the notice could never fire at all.
                    //
                    // The notice itself is NOT published here: it has to land
                    // AFTER the engine boot toast, and that toast is published
                    // by the UI off this very devicesChanged signal. So the
                    // answer is left on the bridge for the UI to read once it
                    // has said "booted" (missingUsedDevices() — see Main.qml's
                    // reportMissingDevices, which runs right after reportBoot).
                    const bool anythingWanted = !usedAudio.isEmpty()
                                                || !usedVideo.isEmpty()
                                                || !usedDisplay.isEmpty();
                    if (onlyUsed && anythingWanted && !self->bootDeviceCheckDone_) {
                        self->bootDeviceCheckDone_ = true;
                        QStringList missing;
                        const auto check = [&missing](const QStringList &wanted,
                                                      const QVariantList &found) {
                            for (const QString &label : wanted) {
                                bool present = false;
                                for (const QVariant &v : found)
                                    if (v.toMap().value("label").toString().trimmed()
                                            .compare(label, Qt::CaseInsensitive) == 0) {
                                        present = true;
                                        break;
                                    }
                                if (!present)
                                    missing << label;
                            }
                        };
                        check(usedAudio, self->audioDevices_);
                        check(usedVideo, self->videoDevices_);
                        check(usedDisplay, self->screenDevices_);
                        self->missingUsedDevices_ = missing;
                        self->write(QStringLiteral("info"), QStringLiteral("Devices"),
                                    QStringLiteral("boot device check: %1 used, %2 missing%3")
                                        .arg(usedAudio.size() + usedVideo.size() + usedDisplay.size())
                                        .arg(missing.size())
                                        .arg(missing.isEmpty()
                                                 ? QString()
                                                 : QStringLiteral(" (%1)")
                                                       .arg(missing.mid(0, 3).join(QStringLiteral(", ")))));
                    }
                    // The rosters below are now the app's OWN hardware (either
                    // the targeted walk or the nothing-configured full one), so
                    // the startup report may finally be told what boot actually
                    // found — BEFORE devicesChanged, which is what carries it
                    // there (Main.qml's reportBoot reads this as its gate).
                    // ...and only when the set it swept is still the CURRENT one.
                    // On a cold start the rosters hydrate one after another, so
                    // the first sweep can finish while a newer hand-over (with
                    // the devices those rosters actually name) is already on its
                    // way. Latching on the stale one reports a device list that
                    // was never the app's — the same class of mistake as
                    // reporting the pre-hand-over sweep. The newer sweep always
                    // exists (a changed set re-launches, see requestUsedDevices),
                    // so it does the latching.
                    const bool stillCurrent = usedAudio == self->usedAudioLabels_
                                              && usedVideo == self->usedVideoLabels_
                                              && usedDisplay == self->usedDisplayNames_;
                    if (bootSweep && stillCurrent && !self->bootSweepDone_) {
                        self->bootSweepDone_ = true;
                        emit self->bootSweepDoneChanged();
                    }
                    self->updateBootSummary();
                    // Re-resolve every requested meter against the fresh
                    // roster. This is what the header's "resolution re-runs
                    // on every enumeration so a hot-plugged device re-binds
                    // to its requested tap" actually promises, and until now
                    // nothing did it: a hold taken BEFORE the first (async)
                    // enumeration finished — the Media pane's auto-holds at
                    // Component.onCompleted, a card clicked in the first
                    // second after boot — warned "no input device named …"
                    // and stayed silent for the rest of the session, which is
                    // exactly what "the meters never move" looked like.
                    for (auto it = self->requestedMeters_.constBegin();
                         it != self->requestedMeters_.constEnd(); ++it)
                        self->resolveAndStartMeter(it.key());
                    emit self->devicesChanged();
                }
                const bool refreshAgain = self->deviceEnumerationPending_
                                          && !self->deviceEnumerationStopping_;
                self->deviceEnumerationPending_ = false;
                if (refreshAgain)
                    self->enumeratePlatformDevicesAsync();
            }, Qt::QueuedConnection);
    });
}

void EngineBridge::updateBootSummary()
{
    QStringList audioNames;
    for (const QVariant &v : audioDevices_) {
        audioNames << v.toMap().value("label").toString();
        if (audioNames.size() >= 3) break;
    }
    // Displays carry their REAL pixel size next to the name: "did the engine
    // see my projector at 4K or did it read the scaled size?" is a question the
    // summary has to be able to answer (see WindowsMonitor's rcMonitor note —
    // rcMonitor is DWM-scaled, dmPels* is the panel).
    QStringList screenNames;
    for (const QVariant &v : screenDevices_) {
        const QVariantMap d = v.toMap();
        const int w = d.value("widthPx").toInt();
        const int h = d.value("heightPx").toInt();
        QString label = d.value("label").toString();
        if (w > 0 && h > 0)
            label += QStringLiteral(" %1x%2").arg(w).arg(h);
        screenNames << label;
        if (screenNames.size() >= 3) break;
    }
    // "in use", deliberately: this roster is the app's OWN hardware (see
    // requestUsedDevices — the sweep asks for the devices this setup names, and
    // enumerates nothing else), so counting it as "what the engine found" would
    // read as "your machine has 0 audio devices" on a desk with eight of them
    // that nothing is wired to. What the summary can honestly state is what
    // this session is using.
    QStringList inUse = audioNames;
    inUse += screenNames;
    QStringList cameraNames;
    for (const QVariant &v : videoDevices_) {
        cameraNames << v.toMap().value("label").toString();
        if (cameraNames.size() >= 3) break;
    }
    inUse += cameraNames;
    // Kept beside the summary: the boot toast bolds these names (its crux), so
    // the sentence reads as "… in use: 1 audio, 2 display(s) — <the names>".
    bootInUse_ = inUse;
    bootSummary_ = QStringLiteral("Engine booted (%1 systems) · in use: %2 audio, %3 display(s), %4 camera(s)%5")
        .arg(bps::Kernel::Instance().BootLog().size())
        .arg(audioDevices_.size())
        .arg(screenDevices_.size())
        .arg(videoDevices_.size())
        .arg(inUse.isEmpty() ? QString()
                             : QStringLiteral(" — %1").arg(inUse.join(QStringLiteral(", "))));
    // A degraded boot says so out loud, right where the user reads the summary.
    const auto degraded = bps::Kernel::Instance().Degraded();
    if (!degraded.empty()) {
        QStringList names;
        for (const auto &d : degraded) names << QString::fromStdString(d.system);
        bootSummary_ += QStringLiteral(" · %1 subsystem(s) unavailable: %2")
                            .arg(degraded.size()).arg(names.join(QStringLiteral(", ")));
    }
}

void EngineBridge::enumerateDevices()
{
    lastEnumerationMs_ = QDateTime::currentMSecsSinceEpoch();
    lastAudioEnumerationMs_ = lastEnumerationMs_;
    enumeratePlatformDevicesAsync();
    if (bps::platform::PlatformAccessor::Installed()) {

        // NDI network sources through the ENGINE's own broadcast stack (the
        // NDI provider runtime-loads the SDK; when it's absent this yields
        // an empty list and an explanatory flag — never a mock roster).
        // Providers register at kernel boot — before that there is no stack
        // to ask, so the status says so instead of surfacing an internal
        // "provider not found" error.
        // DO NOT reset the published verdict here. The probe below is ASYNC
        // (probeNdiAsync), so clearing ndiAvailable_/ndiStatus_/ndiState_ upfront
        // publishes "not usable, no reason" for the WHOLE probe window — and
        // every reader in that window takes it as fact. That is the false
        //   "LiveOutputService: NDI output requested but NDI is not usable ()"
        // warning that showed up whenever a device refresh (a settings visit,
        // a resolution/display-mode change) landed mid-live: the probe answered
        // ~30 ms later and the feed started, but the toast was already on
        // screen. The last ANSWERED state stays until the worker replaces it;
        // only the feature-off branch below publishes a verdict synchronously.
        ndiVersion_.clear();
        if (!booted()) {
            ndiSources_.clear();
            ndiAvailable_ = false;
            ndiStatus_.clear();
            ndiState_ = QStringLiteral("unknown");
        } else {
            // The "ndi" feature switch first: a deliberate OFF is its own state
            // ("disabled"), distinct from "notInstalled" (nothing to download)
            // and "error" (nothing broken) — the notice component stays quiet
            // for it and the Plugins row reads amber "Installed", not red.
            auto ndiFeature = bps::adaptive::AdaptiveRuntime::Instance().GetFeatureState("ndi");
            const bool ndiFeatureOn = ndiFeature.ok()
                && ndiFeature.value() == bps::adaptive::FeatureState::Enabled;
            if (!ndiFeatureOn) {
                ndiSources_.clear();
                ndiAvailable_ = false;
                ndiState_ = QStringLiteral("disabled");
                ndiStatus_ = QStringLiteral("NDI is switched off in Settings · Plugins.");
            } else if (ndiState_ == QStringLiteral("disabled")
                       || ndiState_ == QStringLiteral("unknown")) {
                // Switched ON since the last pass (or nothing has ever
                // answered): there is no valid answer to keep, so drop the
                // stale verdict and let the probe below publish one. This also
                // re-arms the probe — a kept "disabled" would skip it.
                ndiSources_.clear();
                ndiAvailable_ = false;
                ndiState_ = QStringLiteral("unknown");
                ndiStatus_.clear();
            }
            // else: keep the last known ready / notInstalled / error answer
            // (and its source list) while the probe refreshes it in place.
        }
        // The probe itself moved OFF this thread (probeNdiAsync below): the
        // first successful check runtime-loads + NDIlib_initialize's the whole
        // NDI runtime inline, and discovery blocks on the finder — running
        // that between a mouse click and the next GUI paint is what took the
        // app down on the Plugins screen's "check again" button. Only the
        // cheap feature gate above stays synchronous; results arrive via
        // devicesChanged once the worker finishes.
        if (booted() && ndiState_ != QStringLiteral("disabled"))
            probeNdiAsync();

        // ---- SDI (DeckLink) — the same honest-runtime treatment as NDI ----
        // The engine's SDI provider resolves the SDK at runtime; an absent
        // library/hardware reads unavailable with its reason, never a mock
        // roster. Enumerating devices costs a COM walk per provider probe —
        // only when the provider reports available.
        sdiDevices_.clear();
        sdiAvailable_ = false;
        sdiStatus_.clear();
        auto &broadcast2 = bps::broadcast::BroadcastEngine::Instance();
        // The "sdi" feature switch gates the whole probe: with the plugin
        // off (the default), no DeckLink SDK resolve happens at all — the
        // absent-library failure used to toast "Stream provider unavailable"
        // on every device refresh. The status line says where to flip it.
        if (!broadcast2.SdiEnabled()) {
            sdiStatus_ = QStringLiteral("SDI is switched off (Settings · Plugins).");
            emit devicesChanged();
            return;
        }
        auto sdiProvider = broadcast2.Probe("sdi");
        if (sdiProvider.ok() && sdiProvider.value() == bps::broadcast::ProviderState::Available) {
            auto devices = broadcast2.EnumerateSdiDevices();
            if (devices.ok() && !devices.value().empty()) {
                sdiAvailable_ = true;
                for (const auto &d : devices.value()) {
                    QVariantMap row;
                    row.insert(QStringLiteral("name"), qstr(d.displayName));
                    row.insert(QStringLiteral("model"), qstr(d.modelName));
                    row.insert(QStringLiteral("index"), d.index);
                    row.insert(QStringLiteral("supportsCapture"), d.supportsCapture);
                    row.insert(QStringLiteral("supportsOutput"), d.supportsOutput);
                    sdiDevices_.append(row);
                }
            } else if (devices.ok()) {
                sdiStatus_ = QStringLiteral("No DeckLink device found on this machine.");
            } else {
                sdiStatus_ = QString::fromStdString(devices.error().message);
            }
        } else if (sdiProvider.ok()) {
            sdiStatus_ = QStringLiteral("The DeckLink runtime (Desktop Video) isn't installed on this computer.");
        } else {
            sdiStatus_ = QString::fromStdString(sdiProvider.error().message);
        }
    }
    emit devicesChanged();
}

// NDI runtime probe + source discovery OFF the GUI thread. Runs on the global
// QThreadPool; results land back on the GUI thread through a queued
// invokeMethod that republishes every ndi* property and fires devicesChanged.
// Everything expensive happens on the worker: LoadLibrary of the NDI runtime,
// NDIlib_initialize (first successful check of a session), and the blocking
// discovery pass. BroadcastEngine serializes these against its own callers
// (recursive mutex), and the SDK is thread-safe once initialized, so a probe
// in flight while the user goes live is exactly as safe as the old inline
// version — minus the GUI-thread stall.
void EngineBridge::probeNdiAsync()
{
    if (!booted()) return;
    bool expected = false;
    if (!ndiProbeBusy_.compare_exchange_strong(expected, true))
        return;   // a probe is already in flight; it publishes fresh state itself

    QPointer<EngineBridge> guard(this);
    QThreadPool::globalInstance()->start([guard]() {
        auto &broadcast = bps::broadcast::BroadcastEngine::Instance();
        using NdiState = bps::broadcast::BroadcastEngine::NdiRuntimeStatus::State;

        const auto ndi = broadcast.NdiStatus();
        bool available = ndi.state == NdiState::Ready;
        QString state, status, version;
        QVariantList sources;
        switch (ndi.state) {
        case NdiState::Ready:
            state = QStringLiteral("ready");
            version = qstr(ndi.version);
            break;
        case NdiState::NotInstalled:
            // The actionable case: the UI offers the download page.
            state = QStringLiteral("notInstalled");
            status = QStringLiteral("The NDI runtime isn't installed on this computer.");
            break;
        case NdiState::Error:
            state = QStringLiteral("error");
            status = qstr(ndi.detail);
            break;
        }
        if (available) {
            auto found = broadcast.DiscoverNdiSources();
            if (found.ok()) {
                for (const auto &s : found.value()) {
                    const QString nm = qstr(s.name);
                    sources.append(QVariantMap{
                        {QStringLiteral("id"), qstr(s.urlAddress)},
                        {QStringLiteral("label"), nm},
                        {QStringLiteral("value"), nm},
                        {QStringLiteral("url"), qstr(s.urlAddress)},
                    });
                }
            } else {
                available = false;
                state = QStringLiteral("error");
                status = QString::fromStdString(found.error().message);
            }
        }

        // Apply on the GUI thread: the ndi* members back Q_PROPERTY reads and
        // devicesChanged is a GUI-thread signal. invokeMethod with `this` as
        // the context object drops the call entirely if this object is
        // destroyed before it runs; the QPointer re-check is the same guard
        // made explicit.
        QMetaObject::invokeMethod(guard, [guard, available, state, status, version,
                                          sources]() mutable {
            if (!guard) return;
            EngineBridge *self = guard.data();
            self->ndiAvailable_ = available;
            self->ndiState_ = state;
            self->ndiStatus_ = status;
            self->ndiVersion_ = version;
            self->ndiSources_ = sources;
            // First discovery pass after the finder's creation is usually
            // empty (the SDK browses in the background) — deferred re-queries
            // let the cache warm up. Two retries (~1.2s and ~3.6s after the
            // first pass), NDI-side only now (the old chain re-ran the whole
            // camera enumeration per pass); the cap keeps an empty network
            // from looping forever.
            if (available && sources.isEmpty() && self->ndiRetries_ < 2) {
                ++self->ndiRetries_;
                QTimer::singleShot(1200, self, [self]() { self->probeNdiAsync(); });
            }
            self->ndiProbeBusy_ = false;
            emit self->devicesChanged();
        }, Qt::QueuedConnection);
    });
}

// Where the vendor's runtime is downloaded (NDI Tools includes it). Kept here,
// not in QML, so there is exactly one place to update if the vendor moves it.
QString EngineBridge::ndiDownloadUrl() const
{
    return QStringLiteral("https://ndi.video/tools/");
}

// ============================================================================
// The ENGINE's feature registry as the Plugins screen's data source. The
// AdaptiveRuntime registers every built-in feature ("ndi", "streaming",
// "cloud", ...) against the machine's real contract at boot; the screen's
// toggle reads/writes THAT state, so "Active/Installed" is the engine's own
// fact — and a flipped switch actually turns the subsystem on or off (the
// broadcast stack honors the "ndi" entry, see ndiRuntimeUsable() and
// enumerateDevices()' gated discovery below).
// ============================================================================
QVariantList EngineBridge::pluginFeatures() const
{
    QVariantList out;
    if (!booted())
        return out;
    for (const auto &f : bps::adaptive::AdaptiveRuntime::Instance().Features()) {
        out.append(QVariantMap{
            {QStringLiteral("id"), QString::fromStdString(f.id)},
            {QStringLiteral("name"), QString::fromStdString(f.name)},
            {QStringLiteral("enabled"), f.state == bps::adaptive::FeatureState::Enabled},
            {QStringLiteral("defaultEnabled"), f.enabledByDefault},
        });
    }
    return out;
}

void EngineBridge::setPluginFeatureEnabled(const QString &featureId, bool enabled)
{
    if (!booted())
        return;
    auto &runtime = bps::adaptive::AdaptiveRuntime::Instance();
    auto r = enabled ? runtime.SetFeatureEnabled(featureId.toStdString(), true)
                     : runtime.SetFeatureEnabled(featureId.toStdString(), false);
    if (!r.ok()) {
        qWarning("EngineBridge: feature '%s' not registered — switch ignored",
                 qUtf8Printable(featureId));
        return;
    }
    // The broadcast stack learns the flip immediately: a disabled "ndi"
    // tears down live senders/receivers (the discovery roster empties and
    // the NDI output tile stops sending); "sdi" opens/closes the DeckLink
    // gate and re-runs the device roster so the AV board's SDI rows appear
    // (ON) or blank out (OFF) without waiting for a hot-plug event.
    if (featureId.compare(QLatin1String("ndi"), Qt::CaseInsensitive) == 0)
        applyNdiFeatureState(enabled);
    else if (featureId.compare(QLatin1String("sdi"), Qt::CaseInsensitive) == 0) {
        bps::broadcast::BroadcastEngine::Instance().SetSdiEnabled(enabled);
        enumerateDevices();
    }
    emit pluginFeaturesChanged();
}

// The "ndi" feature's runtime effect. Disabling pushes the gate down into
// the ENGINE's BroadcastEngine (its NDI-shaped sender/receiver/discovery
// paths refuse, live teardown runs, NdiStatus reports the switch) AND tears
// the UI's receiver taps down; re-enabling re-opens the gate — the next
// take/discovery re-opens the SDK on demand.
void EngineBridge::applyNdiFeatureState(bool enabled)
{
    auto &broadcast = bps::broadcast::BroadcastEngine::Instance();
    broadcast.SetNdiEnabled(enabled);
    // Re-run discovery so the roster reflects the flip immediately (both
    // ways: refill on ON, empty on OFF).
    ndiRetries_ = 0;
    enumerateDevices();
    if (enabled) {
        // Re-arm every card/dialog/board preview that was torn down by the
        // last OFF — LiveOutputService resyncs the OUTPUT take on its own,
        // but nothing else has a "my tap died, please re-fetch" signal, so
        // without this they stayed dark forever after an off->on cycle.
        const auto pending = ndiTapsPendingReattach_;
        ndiTapsPendingReattach_.clear();
        for (auto it = pending.constBegin(); it != pending.constEnd(); ++it)
            for (const QString &owner : it.value())
                (void)startNdiPreview(it.key(), owner);
        return;
    }
    // Snapshot who owned each tap before tearing it down, so a later
    // re-enable can re-arm them (see above).
    ndiTapsPendingReattach_.clear();
    for (auto it = ndiTaps_.constBegin(); it != ndiTaps_.constEnd(); ++it)
        ndiTapsPendingReattach_.insert(it.key(), previewOwners_.value(it.key()));
    // Release every NDI receiver tap (owners included — the feature gate
    // outranks any held preview; a re-enable re-takes from the UI).
    if (broadcast.NdiStatus().state
        == bps::broadcast::BroadcastEngine::NdiRuntimeStatus::State::Ready) {
        for (const QString &recvId : ndiTaps_.values())
            (void)broadcast.DisconnectReceiver(recvId.toStdString());
    }
    for (const QString &name : ndiTaps_.keys()) {   // keys() copies — removal-safe
        previewOwners_.remove(name);
        previewDeliveredAt_.remove(name);   // the receiver is gone with its stamp
        previewPulledAt_.remove(name);      // ...and with its "watching" clock
    }
    ndiTaps_.clear();
    // LiveOutputService drops the OUTPUT take itself (the tiles' inputLabel
    // clears and the compositor layer comes off with it). Discovery was
    // already re-run above (before this teardown) — SetNdiEnabled(false)
    // already emptied it, so a second enumerateDevices() here re-did the
    // exact same work for nothing.
    LiveOutputService::instance().clearNdiInput();
}

void EngineBridge::openNdiDownloadPage()
{
    QDesktopServices::openUrl(QUrl(ndiDownloadUrl()));
}

// Connected-receiver telemetry: the NDI SDK query can block while a send is
// in progress, so never call it from the GUI's live poll. Coalesce refreshes
// onto the worker pool; only the small property update returns to the GUI.
void EngineBridge::refreshNdiConnections()
{
    if (!booted()) return;
    // Plain check-and-set: this runs on the poll's own thread, and the flag is
    // only ever cleared by the callback this function posts BACK to that same
    // thread (see the member's note) — so no interlock is involved.
    if (ndiConnectionProbeBusy_)
        return; // the SDK query is still in flight; the next poll will retry
    ndiConnectionProbeBusy_ = true;

    QPointer<EngineBridge> guard(this);
    QThreadPool::globalInstance()->start([guard]() {
        auto *provider = []() -> bps::display::NdiDisplayProvider * {
            auto p = bps::display::DisplayEngine::Instance().Provider("Ndi");
            return p ? dynamic_cast<bps::display::NdiDisplayProvider *>(p.get()) : nullptr;
        }();
        int connected = -1;
        if (provider) {
            const std::string senderId = provider->SenderId();
            if (!senderId.empty())
                connected = bps::broadcast::BroadcastEngine::Instance()
                                .SenderConnectedReceivers(senderId);
        }
        QMetaObject::invokeMethod(guard, [guard, connected]() {
            // Check the POINTER WE DEREFERENCE, not only the QPointer that
            // produced it: data() re-reads the weak reference, so the two are
            // separate questions, and testing both is what the object-size
            // pass needs before it will accept the writes below.
            EngineBridge *self = guard.data();
            if (!self) return;
            self->ndiConnectionProbeBusy_ = false;
            if (connected != self->ndiConnectedReceivers_) {
                self->ndiConnectedReceivers_ = connected;
                emit self->ndiConnectedReceiversChanged();
            }
        }, Qt::QueuedConnection);
    });
}

// ============================================================================
// Firewall authorization for INBOUND NDI connections.
//
// Why this exists: NDI receiving is outbound-only (works through any default
// firewall) but NDI SENDING needs other machines to connect TO us — inbound
// — which every desktop firewall blocks new apps for by default. Windows
// shows its own "Windows Security Alert" prompt the first time an app opens
// a listening port, but a missed/dismissed prompt never re-appears and the
// app then sends frames that nobody receives, silently. requestNdiFirewall
// Access() makes the request explicit and re-runnable.
//
// The actual platform mechanics (Windows Firewall + UAC, Linux ufw/
// firewalld detection, macOS not-yet-implemented) live in the PAL's
// INetwork::ProbeInboundAccess/RequestInboundAccess — this is genuinely
// platform-specific system behavior, not something that belongs hardcoded
// in the Qt bridge layer behind a `#if Q_OS_WIN`. This function is just the
// QML-facing string contract on top of that.
//
// "DNS access" needs nothing separate: NDI discovery is mDNS multicast
// (UDP 5353) which is also network traffic FROM this app — the same
// program-scoped rule covers it, and no separate DNS permission exists on
// any of these platforms.
// ============================================================================

namespace { constexpr const char *kNdiFirewallRule = "VGR Presenter NDI"; }

QString EngineBridge::requestNdiFirewallAccess()
{
    if (!bps::platform::PlatformAccessor::Installed()) {
        EngineBridge::write(QStringLiteral("warning"), QStringLiteral("NDI firewall"),
                            QStringLiteral("platform network layer unavailable — cannot check or add the rule"));
        return QStringLiteral("unavailable");
    }
    auto &net = bps::platform::PlatformAccessor::Get().Network();
    const std::string app = QCoreApplication::applicationFilePath().toStdString();
    const std::string desc = "Allows other computers to receive this app's NDI video";
    using Access = bps::platform::INetwork::FirewallAccess;
    using Outcome = bps::platform::INetwork::FirewallRequestOutcome;

    // The firewall RULE ITSELF is the saved permission, not a settings flag:
    // probe first (one hidden netsh child, only until access is confirmed)
    // and stay quiet when the rule already exists. This also self-heals — a
    // user who refused the OS prompt once gets asked again next time it
    // matters, instead of a persisted flag silencing the ask forever.
    if (net.ProbeInboundAccess(app, kNdiFirewallRule) == Access::Allowed) {
        if (!ndiFirewallPrompted_) {
            ndiFirewallPrompted_ = true;   // session-only anti-nag (see header)
            emit ndiFirewallPromptedChanged();
        }
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("NDI firewall"),
                            QStringLiteral("rule already present — no prompt needed"));
        return QStringLiteral("allowed");
    }

    // Ask at most once per session (the QML gates on ndiFirewallPrompted,
    // which is deliberately NOT persisted any more — see above).
    if (!ndiFirewallPrompted_) {
        ndiFirewallPrompted_ = true;
        emit ndiFirewallPromptedChanged();
    }

    // Every attempt is observable now: the elevated step's refusals are
    // fire-and-forget, so without these lines a request that didn't land
    // was indistinguishable from one that did.
    std::string diag;
    const Outcome outcome = net.RequestInboundAccess(app, kNdiFirewallRule, desc, &diag);
    const QStringList lines = QString::fromStdString(diag).split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines)
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("NDI firewall"), line.trimmed());

    const char *text = "failed";
    switch (outcome) {
        case Outcome::Added:       text = "added";       break;
        case Outcome::Prompted:    text = "prompt";      break;   // verdict lands async
        case Outcome::Denied:      text = "denied";      break;
        case Outcome::Failed:      text = "failed";      break;
        case Outcome::Unavailable: text = "unavailable"; break;
    }
    EngineBridge::write(outcome == Outcome::Added || outcome == Outcome::Prompted
                            ? QStringLiteral("info") : QStringLiteral("warning"),
                        QStringLiteral("NDI firewall"),
                        QStringLiteral("request outcome: %1").arg(QLatin1String(text)));
    return QLatin1String(text);
}

// The elevated add (Windows: UAC) resolves AFTER requestNdiFirewallAccess()
// returned (the user still has the consent dialog up). This re-probes so the
// UI can confirm success and stop retrying once the rule is actually there.
bool EngineBridge::checkNdiFirewallRule()
{
    if (!bps::platform::PlatformAccessor::Installed())
        return false;
    auto &net = bps::platform::PlatformAccessor::Get().Network();
    const std::string app = QCoreApplication::applicationFilePath().toStdString();
    return net.ProbeInboundAccess(app, kNdiFirewallRule)
           == bps::platform::INetwork::FirewallAccess::Allowed;
}

// The primary display's REAL size, out of the roster enumerateDevices() just
// refilled — so the number is the engine platform layer's own (IMonitor),
// refreshed with every hot-plug event, not a second enumeration of its own.
//
// Two things the caller must know about the roster (see enumerateDevices): it
// also carries open application WINDOWS (id "win:<hwnd>", size 0) — skipped
// here, since only real displays have a size — and the primary flag lives on the
// monitor entries. When nothing is flagged primary (a PAL with no primary
// notion) the first display wins, which is the same display this window opens on.
QVariantMap EngineBridge::primaryDisplay() const
{
    QVariantMap first;
    for (const QVariant &v : screenDevices_) {
        const QVariantMap entry = v.toMap();
        if (entry.value(QStringLiteral("widthPx")).toInt() <= 0
            || entry.value(QStringLiteral("heightPx")).toInt() <= 0)
            continue;                                   // a window, not a display
        if (entry.value(QStringLiteral("primary")).toBool())
            return entry;
        if (first.isEmpty())
            first = entry;
    }
    return first;
}

// "I installed it — check again": bypasses refreshDevices()'s throttle so the
// UI reacts immediately instead of making the user wait out the window.
void EngineBridge::recheckNdi()
{
    lastEnumerationMs_ = 0;
    ndiRetries_ = 0;
    enumerateDevices();
}

void EngineBridge::refreshDevices()
{
    // UI code calls this when a picker opens. The actual platform enumeration
    // runs on deviceEnumerationPool_; throttle repeated opens and coalesce any
    // refresh that arrives while a scan is already in flight.
    //
    // A PICKER NEEDS THE WHOLE MACHINE, so this is also where the boot sweep's
    // used-device filter (see requestUsedDevices) is dropped for the rest of the
    // session — and the drop must defeat the throttle: a dialog opened 5 s after
    // boot would otherwise keep offering only the used devices for the whole
    // throttle window, i.e. exactly the devices the user already has.
    const bool wasFiltered = usedDeviceSweepOnly_;
    usedDeviceSweepOnly_ = false;
    constexpr qint64 kMinRefreshGapMs = 10000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!wasFiltered && lastEnumerationMs_ > 0 && now - lastEnumerationMs_ < kMinRefreshGapMs)
        return;
    ndiRetries_ = 0;
    enumerateDevices();
}

void EngineBridge::refreshAudioDevices()
{
    // Same full-roster upgrade as refreshDevices (a device picker is opening).
    const bool wasFiltered = usedDeviceSweepOnly_;
    usedDeviceSweepOnly_ = false;
    constexpr qint64 kMinRefreshGapMs = 10000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!wasFiltered && lastAudioEnumerationMs_ > 0 && now - lastAudioEnumerationMs_ < kMinRefreshGapMs)
        return;
    lastAudioEnumerationMs_ = now;
    enumerateDevices();
}

bool EngineBridge::labelWanted(const QStringList &wanted, const QString &label)
{
    const QString want = label.trimmed();
    for (const QString &w : wanted)
        if (w.compare(want, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

void EngineBridge::requestUsedDevices(const QStringList &audioLabels,
                                      const QStringList &videoLabels,
                                      const QStringList &displayNames)
{
    const auto trimmed = [](const QStringList &in) {
        QStringList out;
        for (const QString &l : in) {
            const QString t = l.trimmed();
            if (!t.isEmpty() && !out.contains(t, Qt::CaseInsensitive))
                out << t;
        }
        return out;
    };
    const QStringList newAudio = trimmed(audioLabels);
    const QStringList newVideo = trimmed(videoLabels);
    const QStringList newDisplays = trimmed(displayNames);
    // Only a CHANGED set is worth a log line (and, below, a re-sweep): the
    // hand-over is re-issued whenever a roster is restored or edited, and the
    // steady state is "same answer as last time".
    const bool changed = newAudio != usedAudioLabels_ || newVideo != usedVideoLabels_
                         || newDisplays != usedDisplayNames_;
    usedAudioLabels_ = newAudio;
    usedVideoLabels_ = newVideo;
    usedDisplayNames_ = newDisplays;
    const bool firstHandover = !deviceHandoverSeen_;
    deviceHandoverSeen_ = true;
    // This hand-over can arrive MORE THAN ONCE, and that is the point: the
    // rosters that own these devices hydrate at their own pace (the AV board's
    // video roster is restored after this window has already completed), so an
    // early call legitimately knows nothing yet. Each later call replaces the
    // set — and re-sweeping is free when the sweep is targeted, because a
    // device nobody named is never touched.
    //
    // NOTHING configured (a first run) simply means nothing is ASKED FOR: the
    // rosters come back empty, which is the honest answer (this setup uses no
    // devices). The old fallback to a full boot sweep here was exactly the
    // probing this path exists to avoid — and every picker that needs the
    // machine (Settings · Audio & Video's Add/Edit) calls refreshDevices()/
    // refreshAudioDevices(), which do the full walk when it is actually needed.
    //
    // Once a picker HAS asked for the whole machine, this is no longer ours to
    // re-run: the picker paths own their own refresh, and a roster edit must
    // not drag a full enumeration along with it.
    if (!usedDeviceSweepOnly_)
        return;
    // Nothing new to say, and a sweep already carries this answer: leave the
    // one in flight (or the one that landed) alone. The FIRST hand-over always
    // sweeps, even when it hands over an empty set — that sweep is what says
    // "this setup needs no devices", and the boot report waits on it.
    if (!changed && !firstHandover)
        return;
    write(QStringLiteral("info"), QStringLiteral("Devices"),
          QStringLiteral("boot sweep limited to the devices this setup uses: ")
              + QStringLiteral("%1 audio, %2 video, %3 display(s)")
                    .arg(usedAudioLabels_.size())
                    .arg(usedVideoLabels_.size())
                    .arg(usedDisplayNames_.size()));
    // The boot sweep. `boot()` already enumerated once (monitors only, nothing
    // configured yet); this is the sweep that actually carries the used set, and
    // the pool coalesces the two when they collide.
    enumerateDevices();
}

// ============================================================================
// Live input metering — the UI half of the PAL's WASAPI capture tap.
//
// Taps are a SET keyed by device (the PAL holds one tap per waveIn id):
// the AV screen starts a meter for every device row on the board that NAMES a
// device, each consumer reads its own device's snapshot from the inputLevels
// list, and stopInputMeter(label) releases just that device when its row goes
// away. Resolution re-runs on every enumeration so a hot-plugged device
// re-binds to its requested tap.
// A 50 ms GUI-side pump republishes inputLevels_ while any tap is live.
//
// An EMPTY label is refused (no device chosen ⇒ no capture session), and a
// label that is already held is never re-resolved — see both functions below.
// ============================================================================
// The roster label → PAL wavein number, from the CACHED roster (audioDevices_)
// — a live IAudio::Enumerate() here re-ran the WASAPI mix-format COM queries on
// every kind chip / device pick, and that storm stalled the dialogs exactly like
// the video one did. Matching is TRIMMED + CASE-INSENSITIVE: a hand-stored
// sublabel that differs by whitespace/case used to die SILENTLY (the meter card
// showed idle forever — "no signal" with no reason logged).
//
// Windows ids are "wavein:<n>"; other platforms have no WASAPI tap to address
// (their IAudio keeps the default Unsupported metering) — never guess a number
// from an alien id scheme.
bool EngineBridge::resolveDeviceTap(const QString &deviceLabel, uint32_t *deviceId) const
{
    const QString want = deviceLabel.trimmed();
    if (want.isEmpty())
        return false;
    for (const QVariant &v : audioDevices_) {
        const QVariantMap d = v.toMap();
        if (!d.value("isInput").toBool())
            continue;
        if (d.value("label").toString().trimmed().compare(want, Qt::CaseInsensitive) != 0)
            continue;
        const QString id = d.value("id").toString();
        if (!id.startsWith(QStringLiteral("wavein:")))
            return false;
        if (deviceId)
            *deviceId = static_cast<uint32_t>(id.mid(7).toULong());
        return true;
    }
    return false;
}

void EngineBridge::resolveAndStartMeter(const QString &deviceLabel)
{
    auto &audio = bps::platform::PlatformAccessor::Get().Audio();

    const QString want = deviceLabel.trimmed();
    // NO DEFAULT-INPUT MODE. An empty label used to resolve to the platform's
    // default input, so a dialog opened with nothing picked (or a card whose
    // row names no device) opened a REAL WASAPI capture session on the default
    // microphone: every strip that reads a snapshot animated off it, the
    // MonitorWall's audio button lit (anyInputMetering), and the mixer graph
    // bound that device to the device-less row. A tap belongs to a device the
    // user actually chose.
    if (want.isEmpty()) {
        qWarning("EngineBridge: refused an input meter for an empty device label "
                 "(no device selected — no capture session opened)");
        return;
    }
    uint32_t deviceId = 0;
    if (!resolveDeviceTap(want, &deviceId)) {
        // LOUD now: the earlier silent return hid the exact failure class
        // the "meter doesn't get signal" report was (unresolvable label).
        qWarning("EngineBridge: no input device named '%s' to meter (unplugged, "
                 "roster drift, or a non-WASAPI id)", qUtf8Printable(want));
        return;
    }
    // Already tapped: this call is the per-enumeration / per-sync
    // re-resolution, so there is nothing new to announce.
    const bool alreadyLive = inputLabels_.contains(deviceId);
    if (audio.StartInputMeter(deviceId, want.toStdString()).ok()) {
        inputLabels_[deviceId] = want;
        // ONE line per device tap, the mirror of the video chain's "first
        // preview frame": it is what makes "the meters don't move" answerable
        // from engine.log alone (no tap line = nothing ever asked for a tap;
        // a line with no signal behind it = the device itself is silent).
        if (!alreadyLive)
            write(QStringLiteral("info"), QStringLiteral("AudioMeters"),
                  QStringLiteral("input meter tap live for '%1' (wavein:%2)")
                      .arg(want)
                      .arg(deviceId));
    } else
        qWarning("EngineBridge: input meter tap failed to start for '%s' (wavein:%u)",
                 qUtf8Printable(want), deviceId);
}

void EngineBridge::startInputMeter(const QString &deviceLabel)
{
    if (!bps::platform::PlatformAccessor::Installed())
        return;
    const QString want = deviceLabel.trimmed();
    // The refused case (see resolveAndStartMeter): a hold on "no device" must
    // not enter requestedMeters_ either — that map is what anyInputMetering
    // (the MonitorWall's audio button) and the pump's lifetime read, so an
    // empty hold would light the button with nothing tapped.
    if (want.isEmpty())
        return;
    // Refcounted: the board row and a dialog editing the same device can
    // each hold this label open independently (see the header comment).
    const auto known = requestedMeters_.find(want);
    const bool firstHold = known == requestedMeters_.end();
    ++requestedMeters_[want];
    // RE-RESOLVE ONLY FOR A LABEL NOT ALREADY HELD. Every start used to call
    // through to the PAL, and the PAL REPLACES a live tap on Start (it joins
    // the old capture thread and spawns a new one), so the AV board's
    // idempotent re-sync — or a second hold on one device — tore down and
    // rebuilt a working WASAPI session on the GUI thread for nothing (a
    // visible hitch each time a dialog opened). A device that vanished and
    // came back is still re-bound, by the enumeration callback.
    if (firstHold)
        resolveAndStartMeter(want);
    emit audioMeteringChanged();   // the overlay gates on ANY audio metering
    ensureMeterPump();
}

// The ONE place the 50 ms pump is created (it used to be created inline at
// every entry point, which is how a new one — the mix's own source taps —
// could easily have been forgotten and left the mixer unfed).
void EngineBridge::ensureMeterPump()
{
    if (!inputMeterPump_) {
        inputMeterPump_ = new QTimer(this);
        inputMeterPump_->setInterval(50);
        connect(inputMeterPump_, &QTimer::timeout, this, [this]() { pumpMeterSnapshot(); });
    }
    if (!inputMeterPump_->isActive())
        inputMeterPump_->start();
}

// One tick of the shared meter pump (~20 Hz): publishes the input taps'
// snapshots AND — when output metering is on — the program-mix loopback
// snapshot. The UI's meters breathe off these two lists.
void EngineBridge::pumpMeterSnapshot()
{
    if (!bps::platform::PlatformAccessor::Installed())
        return;   // platform torn down mid-pump (shutdown) — idle out
    auto &a = bps::platform::PlatformAccessor::Get().Audio();
    QVariantList levels;
    // ONE snapshot shape for both carriers: a capture DEVICE tap and a STREAM
    // tap (a video/NDI row's own audio) publish identically under their own
    // label, so every reader — the AV board's strips, the Media pane's L/R
    // pills, a bus's channel rows — resolves a level the same way whichever
    // kind of source it is looking at.
    const auto appendSnapshot = [&](uint32_t tapId, const QString &label) {
        const auto snap = a.InputLevels(tapId);
        QVariantList peaks;
        QVariantList rms;
        for (int c = 0; c < snap.channelCount; ++c) {
            peaks.append(snap.peaks[c]);
            rms.append(snap.rms[c]);
        }
        // The ONE line that says real audio is flowing (see the header): a tap
        // line alone only proves a tap was requested, not that the source is
        // producing anything. Loud on purpose — it is the difference between
        // "the meter is broken" and "the microphone/file is silent".
        if (!firstMeterSignal_ && snap.channelCount > 0) {
            for (int c = 0; c < snap.channelCount; ++c) {
                if (snap.peaks[c] <= 0.0005f)
                    continue;
                firstMeterSignal_ = true;
                write(QStringLiteral("info"), QStringLiteral("AudioMeters"),
                      QStringLiteral("first meter signal: '%1' %2 channel(s), peak=%3")
                          .arg(label)
                          .arg(snap.channelCount)
                          .arg(static_cast<double>(snap.peaks[c]), 0, 'f', 3));
                break;
            }
        }
        levels.append(QVariantMap{
            {QStringLiteral("deviceId"), (int)tapId},
            {QStringLiteral("label"), label},
            {QStringLiteral("channelCount"), snap.channelCount},
            {QStringLiteral("layout"),
             QString::fromLatin1(bps::platform::IAudio::InputMeterLevels::ToString(snap.layout))},
            {QStringLiteral("sampleRateHz"), (int)snap.sampleRateHz},
            {QStringLiteral("framesCaptured"), (qulonglong)snap.framesCaptured},
            {QStringLiteral("peaks"), peaks},
            {QStringLiteral("rms"), rms},
        });
    };
    for (auto it = inputLabels_.constBegin(); it != inputLabels_.constEnd(); ++it)
        appendSnapshot(it.key(), it.value());
    for (auto it = streamTapIds_.constBegin(); it != streamTapIds_.constEnd(); ++it)
        appendSnapshot(it.value(), it.key());
    inputLevels_ = std::move(levels);
    emit inputLevelsChanged();

    // NOTE (2026-10, removal): this pump used to end with refreshMixSources()
    // and a program-mix (outputLevels) block. Both went with the unreachable
    // engine-audio path: refreshMixSources() ACQUIRED a capture tap per routed
    // source (PAL IAudio::StartInputMeter — a real WASAPI session), and its only
    // consumer was the render callback of setAudioRender, which nothing called.
    // So the result was a capture session on every routed input, opened silently
    // ~20×/s, on nothing but the strength of a METER tick — the "go live is
    // opening WASAPI" report (GO LIVE starts this pump for every active tile's
    // meter). The output levels it published came from AudioMixer::ProgramLevel,
    // which only ever updated inside that same render callback — which is why
    // they read a flat zero while the monitor tiles' bars were really driven by
    // the input taps below. The pump is now a pure read: it publishes snapshots
    // and touches no device.
}

void EngineBridge::stopInputMeter(const QString &deviceLabel)
{
    const QString want = deviceLabel.trimmed();
    const auto it = requestedMeters_.find(want);
    if (it == requestedMeters_.end())
        return;   // not metering that label — nothing to release
    if (--it.value() > 0)
        return;   // another consumer (board row / other dialog) still holds it open
    requestedMeters_.erase(it);
    // Stop the tap under its label (labels map 1:1 to ids while the device
    // is present; a vanished device's tap already died with its endpoint).
    if (bps::platform::PlatformAccessor::Installed()) {
        auto &audio = bps::platform::PlatformAccessor::Get().Audio();
        for (auto it = inputLabels_.constBegin(); it != inputLabels_.constEnd(); ++it) {
            if (it.value() == want) {
                (void)audio.StopInputMeter(it.key());
                break;
            }
        }
    }
    for (auto it = inputLabels_.begin(); it != inputLabels_.end();) {
        if (it.value() == want)
            it = inputLabels_.erase(it);
        else
            ++it;
    }
    // No requested meters left: stop the pump and publish the empty set —
    // unless a stream meter is still riding it.
    if (requestedMeters_.isEmpty() && requestedStreams_.isEmpty()) {
        if (inputMeterPump_)
            inputMeterPump_->stop();
        inputLabels_.clear();
        if (!inputLevels_.isEmpty()) {
            inputLevels_.clear();
            emit inputLevelsChanged();
        }
    }
    emit audioMeteringChanged();   // the overlay gates on ANY audio metering
}

void EngineBridge::stopAllInputMeters()
{
    requestedMeters_.clear();
    // Stream meters share this pump — only stop it when nothing else needs it.
    if (inputMeterPump_ && requestedStreams_.isEmpty())
        inputMeterPump_->stop();
    if (bps::platform::PlatformAccessor::Installed()) {
        auto &audio = bps::platform::PlatformAccessor::Get().Audio();
        for (const uint32_t id : audio.ActiveInputMeters())
            (void)audio.StopInputMeter(id);
    }
    inputLabels_.clear();
    if (!inputLevels_.isEmpty()) {
        inputLevels_.clear();
        emit inputLevelsChanged();
    }
    // stopInputMeter() (the single-device release, above) emits this —
    // this whole-roster release didn't, so anyInputMetering bindings (the
    // MonitorWall toolbar's audio button, the monitor tiles' meter-bar
    // visibility) never re-evaluated: requestedMeters_ genuinely went empty,
    // but nothing told QML to re-read it, so the button stayed stuck lit after
    // a "stop all" click.
    emit audioMeteringChanged();
}

// ============================================================================
// Live video preview — the UI half of the PAL's MF Source Reader tap.
//
// Resolution flows label-first: QML knows the roster LABEL (what the board
// stores), the PAL knows symbolic-link device ids. startVideoPreview keeps
// a label→id map, refreshed on every call — the same re-resolution rule the
// meter taps follow. QML polls the frames by re-fetching
// image://videopreview/<label>?<nonce> (~15 Hz timer); the provider drains
// the PAL's newest JPEG per device id.
// ============================================================================
QImage EngineBridge::previewFrameFor(const QString &label)
{
    // Same translation the image provider does (label → engine id through
    // previewIds_), then the SAME decode-once cache — the service-side frame
    // detection reads exactly what the tiles' Image requests would read.
    const QString devId = previewIds_.value(label.trimmed());
    return devId.isEmpty() ? QImage{} : latestPreviewFrame(devId);
}

// The status light's probe (see the header): the cached answer when there is
// one, otherwise a RATE-LIMITED ping that NAMES the source, so the sweep that
// answers it is asked for that device instead of hoping it is in the used set.
bool EngineBridge::inputSourceAlive(const QString &label, const QString &kind)
{
    if (inputSourceReachable(label, kind))
        return true;
    const QString want = label.trimmed();
    if (!bps::platform::PlatformAccessor::Installed())
        return false;
    // A Media-kind source is a FILE: it does not appear in any device sweep, so
    // there is nothing to ping for it (inputSourceReachable already answered
    // from the path itself). NDI is the same: its runtime owns its own probe.
    if (kind == QLatin1String("media"))
        return false;
    // NOTHING NAMED IS NOTHING TO ASK FOR: an empty label means the row names no
    // device, which no sweep can turn into an answer — pinging for it would walk
    // the machine every five seconds for the rest of the session and change
    // nothing (the "no device" answer is the honest one).
    if (want.isEmpty())
        return false;
    const QString &ask = want;
    if (!lightProbeLogged_.contains(kind + QLatin1Char('|') + ask)) {
        lightProbeLogged_.insert(kind + QLatin1Char('|') + ask);
        // ONE line per source per session — the diagnostic that a light is not
        // answering ("I expect to see all green" and the rosters disagree about
        // what this source is called are otherwise indistinguishable from a
        // device that is genuinely gone).
        const auto names = [](const QVariantList &roster, int cap) {
            QStringList out;
            for (const QVariant &v : roster) {
                out << v.toMap().value("label").toString();
                if (out.size() >= cap)
                    break;
            }
            return out.join(QStringLiteral(" | "));
        };
        // Through the engine LOG, not qInfo: this app's log is the single source
        // of truth for a run's health (and the only durable record — there is no
        // console in a normal launch), and this line is exactly the kind of
        // one-shot diagnostic the other chains leave there.
        write(QStringLiteral("info"), QStringLiteral("Devices"),
              QStringLiteral("status light for '%1' (%2) is not in the device or screen ")
                  .arg(ask, kind.isEmpty() ? QStringLiteral("no kind") : kind)
              + QStringLiteral("rosters — pinging the device set for it. Audio [%1] Video [%2] Screen [%3]")
                    .arg(names(audioDevices_, 8), names(videoDevices_, 8),
                         names(screenDevices_, 8)));
    }
    // 5 s: long enough that a card re-evaluating at the pump's rate asks once,
    // short enough that a device plugged in while the operator watches the card
    // comes right within a few seconds. The refresh itself is refused outright
    // while a scan is in flight, so this can never stack enumerations.
    constexpr qint64 kLightPingGapMs = 5000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastLightPingMs_ > 0 && now - lastLightPingMs_ < kLightPingGapMs)
        return false;
    if (deviceEnumerationBusy_)
        return false;
    lastLightPingMs_ = now;
    // The ping is the SAME sweep boot ran, with this source's label added to
    // what it walks (see enumeratePlatformDevicesAsync): the used-device filter
    // STAYS ON, so a status light still never opens or activates a device it is
    // not asking about, and the whole walk happens on the device-enumeration
    // worker. Several lights asking between two sweeps share the next one —
    // each ask is a label in the same targeted walk, not another enumeration.
    if (!lightAsks_.contains(ask) && lightAsks_.size() < 16)
        lightAsks_ << ask;
    // ...QUEUED, never inline. This probe is called FROM a QML binding (the bus
    // status light), and enumerateDevices() ends by emitting devicesChanged —
    // synchronously, on its NDI half. A binding that triggers the very signal
    // whose handler bumps the revision it reads is a binding loop, and QML says
    // so out loud ("Binding loop detected for property busStatus"). Deferring by
    // one event-loop turn keeps the ask and lets the binding finish first.
    QMetaObject::invokeMethod(this, [this]() { enumerateDevices(); }, Qt::QueuedConnection);
    return false;
}

QString EngineBridge::inputDelivery(const QString &label, const QString &kind)
{
    // The app's own taps, read for what they have actually DELIVERED — no device
    // is opened, no enumeration runs, nothing is asked of the PAL (see the header).
    const QString want = label.trimmed();
    if (want.isEmpty())
        return {};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto verdict = [now](qint64 deliveredAt) {
        return now - deliveredAt <= kDeliveryWindowMs ? QStringLiteral("ok")
                                                      : QStringLiteral("starved");
    };
    if (kind == QLatin1String("audio")) {
        // A meter tap: the capture session that is running for this label is the
        // evidence. Its snapshots are level data, so "silent" is real data — a
        // quiet room and a muted mic both read ok, and only a hold whose tap
        // never started (or a device that went away under it) reads starved.
        if (!requestedMeters_.contains(want))
            return {};   // nothing is pulling from it
        for (auto it = inputLabels_.constBegin(); it != inputLabels_.constEnd(); ++it)
            if (it.value() == want)
                return QStringLiteral("ok");
        return QStringLiteral("starved");
    }
    // A VIDEO tap (camera, display/window, NDI receiver): the shared owner table
    // says whether the app holds one, and the tap key — device id for a capture
    // tap, source name for an NDI receiver — finds the stamp its frames write.
    if (!previewOwners_.contains(want) || previewOwners_.value(want).isEmpty())
        return {};   // nothing is pulling from it
    const QString key = previewIds_.value(want, want);
    // HELD IS NOT WATCHED (see kWatchWindowMs): a tap nothing asked for frames
    // recently has no live evidence to offer — its silence is "nobody looked",
    // not "nothing came" — so the light keeps its presence answer instead of
    // painting a good source amber because its card scrolled out of view.
    const qint64 pulledAt = previewPulledAt_.value(key, 0);
    if (pulledAt == 0 || now - pulledAt > kWatchWindowMs)
        return {};
    return verdict(previewDeliveredAt_.value(key, 0));
}

qint64 EngineBridge::previewDeliveryAgeMs(const QString &label) const
{
    const QString key = previewIds_.value(label.trimmed(), label.trimmed());
    const qint64 at = previewDeliveredAt_.value(key, 0);
    return at == 0 ? -1 : QDateTime::currentMSecsSinceEpoch() - at;
}

qint64 EngineBridge::previewPullAgeMs(const QString &label) const
{
    const QString key = previewIds_.value(label.trimmed(), label.trimmed());
    const qint64 at = previewPulledAt_.value(key, 0);
    return at == 0 ? -1 : QDateTime::currentMSecsSinceEpoch() - at;
}

QString EngineBridge::previewFailure(const QString &deviceLabel)
{
    const QString label = deviceLabel.trimmed();
    if (label.isEmpty() || !bps::platform::PlatformAccessor::Installed())
        return {};
    const QString devId = previewIds_.value(label);
    if (devId.isEmpty())
        return {};   // no tap: "not started" is the caller's business, not a failure
    return QString::fromStdString(
        bps::platform::PlatformAccessor::Get().Video().PreviewFailure(devId.toStdString()));
}

// The last frame each source actually delivered, by tap key (device id for
// camera/screen taps, source name for NDI receivers). Preview taps are
// PULL-based: any transient empty read — a screen-capture session re-dressing
// mid-desktop-composition, a camera re-locking after a mode change, an NDI
// tick where the receiver had nothing new — used to surface as a one-frame
// empty QImage, which the QML provider turns into its 1×1 placeholder and
// every pane's "frames flowing" test (sourceSize > 1) flips FALSE for that
// tick: the decorative glyph art blinks over the live feed and back. A
// preview pane shows the newest frame it EVER had until a real new one
// arrives (exactly what a hardware preview monitor does) — one QMap entry
// per open tap, a COW QImage copy. Cleared when the tap's last owner
// releases it (stopVideoPreview/stopScreenPreview/stopNdiPreview) and in
// stopAllVideoPreviews, so a closed pane never resurrects a stale frame.
QImage EngineBridge::heldPreviewFrame(const QString &key) const
{
    return previewLastFrame_.value(key);
}

void EngineBridge::setHeldPreviewFrame(const QString &key, const QImage &frame)
{
    if (key.isEmpty())
        return;
    if (frame.isNull())
        previewLastFrame_.remove(key);   // nothing ever arrived — keep it that way
    else
        previewLastFrame_[key] = frame;
}

QImage EngineBridge::latestPreviewFrame(const QString &deviceId)
{
    if (!bps::platform::PlatformAccessor::Installed())
        return {};
    const std::string id = deviceId.toStdString();
    const QString key = QString::fromStdString(id);
    // WATCHED (see kWatchWindowMs): somebody asked the PAL for this tap's newest
    // frame just now, whether or not one came back. Stamped BEFORE the empty
    // test on purpose — an empty answer from a tap nothing is rendering is not
    // evidence of anything, and only this clock can tell the two apart.
    previewPulledAt_[key] = QDateTime::currentMSecsSinceEpoch();
    const std::vector<uint8_t> jpeg =
        bps::platform::PlatformAccessor::Get().Video().PreviewFrame(id);
    if (jpeg.empty())
        // HOLD-LAST-FRAME: a transient empty pull must not blink the pane (see heldPreviewFrame)
        return heldPreviewFrame(key);
    // DECODE-ONCE CACHE — the hot path that killed the UI: requestImage
    // runs on the GUI thread (synchronous QQuickImageProvider), and several
    // consumers (board thumb + dialog pane) poll the same device at 8-15 Hz
    // each. Decoding the JPEG per request meant 3-15 JPEG decodes/frame —
    // the render loop starved and clicks were dropped. Decode only when the
    // tap's bytes actually CHANGED (size + FNV-1a — size alone froze the
    // pane whenever consecutive frames compressed to the same length), and
    // hand every consumer a cheap COW copy.
    const QByteArray raw(reinterpret_cast<const char *>(jpeg.data()), int(jpeg.size()));
    // THE DELIVERY STAMP (see inputDelivery): the PAL handed over a NON-EMPTY
    // JPEG, so this tap delivered a frame — whether or not its bytes differ from
    // the last one. Stamping inside the changed-bytes branch below (where it
    // first lived) made a STILL feed read as no feed at all: a covered lens, a
    // static desktop, or any synthetic pattern encodes to byte-identical JPEGs
    // frame after frame, the decode-once cache then early-returns on every pull,
    // and the light sat amber over a pane that was showing the picture (the same
    // pull sets the service's `producing_`). The hold-last-frame path above never
    // stamps — an empty pull is not a delivery — which is what makes a tap whose
    // device went away read starved instead of fine.
    previewDeliveredAt_[key] = QDateTime::currentMSecsSinceEpoch();
    quint32 hash = 2166136261u;
    for (char b : raw) {
        hash ^= static_cast<quint8>(b);
        hash *= 16777619u;
    }
    const quint32 storedHash = previewFrameHash_.value(key, 0);
    const int storedLen = previewRawLen_.value(key, -1);
    if (storedLen == raw.size() && storedHash == hash)
        return previewDecoded_.value(key);   // identical bytes → cached decode
    QImage frame;
    frame.loadFromData(raw, "JPEG");
    if (!frame.isNull()) {
        previewDecoded_[key] = frame;
        previewRawLen_[key] = raw.size();
        previewFrameHash_[key] = hash;
        // One-shot chain confirmation per device: the log line proves the
        // whole tap → publish → fetch → decode path worked end to end.
        if (!previewFirstFrameLogged_.contains(key)) {
            previewFirstFrameLogged_.insert(key);
            qInfo("EngineBridge: first preview frame decoded for '%s' (%dx%d)",
                  qUtf8Printable(key), frame.width(), frame.height());
        }
    }
    return frame;
}

QImage EngineBridge::VideoPreviewProvider::requestImage(const QString &id, QSize *size, const QSize &requested)
{
    // id carries the roster LABEL, percent-encoded by the QML side, PLUS
    // the nonce query (QQuickImageProvider does NOT strip "?n=..." — the
    // first version looked up "Integrated Webcam?n=1" and missed the tap
    // map on every request). Strip the query, then percent-decode. THE KEY
    // CHAIN: the tap is keyed by the ENGINE device id (symbolic link), so
    // the label translates through previewIds_.
    QString path = id;
    const int queryAt = path.indexOf(QLatin1Char('?'));
    if (queryAt >= 0)
        path.truncate(queryAt);
    const QString label = QUrl::fromPercentEncoding(path.toUtf8());
    QImage frame;
    if (owner_->previewIds_.contains(label)) {
        // Camera/screen tap: drain the PAL's newest JPEG through the usual
        // label→engine-id translation.
        const QString devId = owner_->previewIds_.value(label);
        frame = owner_->latestPreviewFrame(devId.isEmpty() ? id : devId);
    } else if (owner_->ndiTaps_.contains(label)) {
        // NDI receiver tap: drain the engine's newest UYVY/BGRA frame and
        // convert — the SAME provider URL the tiles/cards already pump, so
        // NDI inputs ride the exact preview pipeline camera feeds do.
        frame = owner_->previewNdiFrameFor(label);
    } else {
        // One warning per label per session — a missing tap is a wiring
        // bug (start never ran / a different label spelling), not a warm-up.
        if (!owner_->previewNoTapWarned_.contains(label)) {
            owner_->previewNoTapWarned_.insert(label);
            qWarning("EngineBridge: preview requested for '%s' but no tap was started",
                     qUtf8Printable(label));
        }
        frame = owner_->latestPreviewFrame(id);
    }
    // One-shot per label: did the provider actually SERVE a frame through
    // the label→id translation? (Decoded-frames-in-cache but a dark pane
    // means this never fired with the pane's label.)
    if (!frame.isNull() && !owner_->previewServedLogged_.contains(label)) {
        owner_->previewServedLogged_.insert(label);
        qInfo("EngineBridge: preview frame SERVED for label '%s'", qUtf8Printable(label));
    }
    // HOLD-LAST-FRAME, provider side: every consumer drains through HERE, so
    // remembering the newest good frame per label (camera/screen keys use the
    // device id, NDI the source name) keeps a stale-served pane from blinking
    // even when the miss above took the no-tap path.
    owner_->setHeldPreviewFrame(label, frame);
    if (frame.isNull()) {
        // 1×1 transparent keeps the QML Image valid while the tap warms up
        // — the pane keeps its glyph underneath.
        frame = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        frame.fill(Qt::transparent);
    } else if (requested.width() > 0 && frame.width() > requested.width()) {
        // Downscale to the pane's demand — FastTransformation: Smooth on
        // every pump at 15 Hz was the second UI-thread sink; a preview
        // pane cannot see the difference.
        frame = frame.scaledToWidth(requested.width(), Qt::FastTransformation);
    }
    if (size)
        *size = frame.size();
    return frame;
}

bool EngineBridge::startVideoPreview(const QString &deviceLabel, const QString &mode,
                                     const QString &owner)
{
    if (!bps::platform::PlatformAccessor::Installed())
        return false;
    auto &video = bps::platform::PlatformAccessor::Get().Video();

    // Resolve the roster label → device id from the CACHED roster
    // (videoDevices_, refreshed at boot/hot-plug). Do not enumerate
    // here: this runs during QML board construction and can block on camera drivers.
    const QString want = deviceLabel.trimmed();
    QString id;
    for (const QVariant &v : videoDevices_) {
        const QVariantMap d = v.toMap();
        if (d.value("label").toString() == want) {
            id = d.value("id").toString();
            break;
        }
    }
    if (id.isEmpty()) {
        qWarning("EngineBridge: no camera named '%s' to preview", qUtf8Printable(want));
        return false;
    }

    // NDI virtual cameras crash their own driver DLL in-process when opened
    // as MF capture devices — this tap must never touch them. The bridge
    // refuses on the LABEL (reliable) after the PAL's id-derived name
    // lookup proved unable to catch these devices. NDI sources belong to
    // the dedicated NDI pipeline; the pane keeps its glyph until then.
    if (want.contains("ndi", Qt::CaseInsensitive)) {
        qWarning("EngineBridge: refusing camera preview tap for NDI virtual device '%s'",
                 qUtf8Printable(want));
        return false;
    }

    // Owner-idempotent + ensure-semantics: the board row (owner "board",
    // mode-less) and the dialog pane (owner "dialog", mode-locked) can hold
    // the SAME camera at once. A repeat start from the same owner must not
    // tear the tap down; an empty mode adopts the existing lock instead of
    // re-locking the device default (that would bounce the dialog's tap).
    // (fresh is evaluated before the insert below.)
    const bool fresh = !previewOwners_.contains(want)
                       || previewOwners_.value(want).isEmpty();
    previewOwners_[want].insert(owner);
    const bool ensureOnly = mode.isEmpty() && previewModes_.contains(want);
    const QString effectiveMode = ensureOnly ? previewModes_.value(want) : mode;
    const bool needRestart = fresh
                             || previewIds_.value(want) != id
                             || previewModes_.value(want) != effectiveMode;
    previewIds_[want] = id;
    previewModes_[want] = effectiveMode;
    if (!needRestart)
        return true;
    (void)video.StopPreview(id.toStdString());
    if (!video.StartPreview(id.toStdString(), effectiveMode.toStdString(),
                            want.toStdString()).ok()) {
        qWarning("EngineBridge: camera preview tap failed for '%s'", qUtf8Printable(want));
        return false;
    }
    // A NEW tap starts with the warm-up grace on its clock (see inputDelivery):
    // opening a camera takes a moment, and "no frame yet" is not "no frames".
    previewDeliveredAt_[id] = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void EngineBridge::stopVideoPreview(const QString &deviceLabel, const QString &owner)
{
    const QString want = deviceLabel.trimmed();
    auto it = previewOwners_.find(want);
    if (it == previewOwners_.end())
        return;
    it->remove(owner);
    if (!it->isEmpty())
        return;   // another owner (board row / dialog pane) still holds the tap
    previewOwners_.erase(it);
    const QString id = previewIds_.take(want);
    previewModes_.remove(want);
    previewLastFrame_.remove(id);   // tap gone — the held frame must not resurrect
    previewDeliveredAt_.remove(id); // ...and neither may its delivery stamp
    previewPulledAt_.remove(id);    // ...nor its "something was watching" clock
    if (id.isEmpty() || !bps::platform::PlatformAccessor::Installed())
        return;
    (void)bps::platform::PlatformAccessor::Get().Video().StopPreview(id.toStdString());
}

// ---- Live SCREEN preview -------------------------------------------------
// Same label-first, owner-counted discipline as the camera taps, keyed by
// the monitor's roster label. The label resolves through the CACHED
// screenDevices_ roster (refreshed at boot / enumerateDevices) to the
// monitor id ("\\\.\DISPLAY1"); the tap lives in the PAL's shared preview
// table and frames flow back through the SAME videopreview provider —
// monitor ids can never collide with camera symlinks.
bool EngineBridge::inputSourceReachable(const QString &label, const QString &kind)
{
    // QUIET probe using the current cached roster (plus, for a Media-kind
    // source, the filesystem). This runs in output and Media UI flows, so it
    // must never enumerate hardware synchronously.
    if (!bps::platform::PlatformAccessor::Installed())
        return false;
    const QString want = label.trimmed();
    // The label as a FILE — a Media-kind source names a video/audio file, not a
    // device, so its "is it still there" question has nothing to do with any
    // roster: it is a path, and the answer is the path's own.
    const auto fileIsThere = [&want]() {
        return !want.isEmpty() && QFileInfo::exists(want);
    };
    if (kind == QLatin1String("media"))
        return fileIsThere();
    // One roster lookup, shared by every kind below: `flag` names a boolean the
    // entry must also carry (the audio roster's isInput), empty for the rest.
    const auto inRoster = [&want](const QVariantList &roster, const char *flag) {
        for (const QVariant &v : roster) {
            const QVariantMap d = v.toMap();
            if (flag && !d.value(QLatin1String(flag)).toBool())
                continue;
            if (d.value("label").toString().trimmed().compare(want, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    // NO LABEL, NOTHING TO CHECK. Every caller in the app means "a row with no
    // device chosen" by an empty label (the meter tap refuses one, the boot
    // hand-over skips one), so this is not-available — asking "does this machine
    // have any input at all" instead would answer for a row that names nothing,
    // and would flip that row's slash state from "can't reach" to "fine".
    if (want.isEmpty())
        return false;
    if (kind == QLatin1String("audio"))
        return inRoster(audioDevices_, "isInput");
    if (kind == QLatin1String("screen"))
        return inRoster(screenDevices_, nullptr);
    if (kind == QLatin1String("camera"))
        return inRoster(videoDevices_, nullptr);
    if (kind == QLatin1String("ndi")) {
        // A source is reachable while the engine's discovery roster knows
        // it (the same full-name string the roster stores). An NDI runtime
        // that isn't usable makes every source unreachable — the card's
        // slashed state then reads honestly instead of offering a take
        // that could never produce frames.
        if (!booted() || !ndiAvailable_)
            return false;
        return inRoster(ndiSources_, nullptr);
    }
    // NO KIND, OR ONE THIS DOES NOT SPELL: not a reason to answer "gone". Ask
    // every roster, and the filesystem last. The card knows what it routed; a
    // kind string that failed to travel (an empty role, a value from a newer
    // row) must not be the thing that pins that card's light amber forever.
    return inRoster(screenDevices_, nullptr)
        || inRoster(videoDevices_, nullptr)
        || inRoster(audioDevices_, nullptr)
        || inRoster(ndiSources_, nullptr)
        || fileIsThere();
}

bool EngineBridge::startScreenPreview(const QString &monitorLabel, const QString &owner)
{
    if (!bps::platform::PlatformAccessor::Installed())
        return false;
    const QString want = monitorLabel.trimmed();
    // DIAGNOSTIC (env-gated): the window-thumbnail chain has one suspect per
    // stage — resolution here, tap start below, provider label→id after.
    // VGR_PREVIEW_DIAG=1 prints each stage's inputs; inert normally.
    const bool diag = qEnvironmentVariableIsSet("VGR_PREVIEW_DIAG");
    if (diag) {
        QStringList roster;
        for (const QVariant &v : screenDevices_)
            roster << v.toMap().value("label").toString();            qInfo("EngineBridge[diag] startScreenPreview want='%s' owner=%s roster=%s",
              qUtf8Printable(want), qUtf8Printable(owner), qUtf8Printable(roster.join(" | ")));
    }
    QString id;
    for (const QVariant &v : screenDevices_) {
        const QVariantMap d = v.toMap();
        // Trimmed + case-insensitive: roster labels are hand-stored strings,
        // and a mismatch here silently downgrades a real window to the
        // "no display named" toast below.
        if (d.value("label").toString().trimmed().compare(want, Qt::CaseInsensitive) == 0) {
            id = d.value("id").toString();
            break;
        }
    }
    if (id.isEmpty()) {
        // A window may have opened since the last roster snapshot. Queue a
        // refresh and let the caller retry once devicesChanged is emitted;
        // never run EnumWindows/monitor enumeration inside a take or render UI.
        if (!deviceEnumerationBusy_)
            enumerateDevices();
        qWarning("EngineBridge: no cached display named '%s' to preview yet", qUtf8Printable(want));
        return false;
    }
    previewOwners_[want].insert(owner);
    if (previewIds_.contains(want)) {
        if (diag)
            qInfo("EngineBridge[diag] startScreenPreview: existing tap id='%s'",
                  qUtf8Printable(previewIds_.value(want)));
        return true;
    }
    auto startTap = [this](const QString &tapId) {
        return bps::platform::PlatformAccessor::Get().Video()
                   .StartScreenPreview(tapId.toStdString()).ok();
    };
    const bool ok = startTap(id);
    if (ok) {
        previewIds_[want] = id;
        // Warm-up grace on this tap's clock (see inputDelivery).
        previewDeliveredAt_[id] = QDateTime::currentMSecsSinceEpoch();
        qInfo("EngineBridge: screen tap '%s' -> id '%s' started", qUtf8Printable(want),
              qUtf8Printable(id));
        return true;
    }
    // Start FAILED: release this owner's claim too — a claim with no tap means
    // no later startScreenPreview for this label can ever try again.
    qWarning("EngineBridge: screen preview tap failed for '%s'", qUtf8Printable(want));
    auto it = previewOwners_.find(want);
    if (it != previewOwners_.end()) {
        it->remove(owner);
        if (it->isEmpty())
            previewOwners_.erase(it);
    }
    // The thumbnail may have been built BEFORE this label existed in the
    // roster (its provider request would have warned "no tap") — the nonce
    // bump happens on the QML side; nothing to do here.
    return false;
}

void EngineBridge::stopScreenPreview(const QString &monitorLabel, const QString &owner)
{
    const QString want = monitorLabel.trimmed();
    auto it = previewOwners_.find(want);
    if (it == previewOwners_.end())
        return;
    it->remove(owner);
    if (!it->isEmpty())
        return;
    previewOwners_.erase(it);
    const QString id = previewIds_.take(want);
    previewLastFrame_.remove(id);   // tap gone — the held frame must not resurrect
    previewDeliveredAt_.remove(id); // ...and neither may its delivery stamp
    previewPulledAt_.remove(id);    // ...nor its "something was watching" clock
    if (id.isEmpty() || !bps::platform::PlatformAccessor::Installed())
        return;
    (void)bps::platform::PlatformAccessor::Get().Video().StopScreenPreview(id.toStdString());
}

void EngineBridge::stopAllVideoPreviews()
{
    if (bps::platform::PlatformAccessor::Installed()) {
        auto &video = bps::platform::PlatformAccessor::Get().Video();
        for (const QString &id : previewIds_.values())
            (void)video.StopPreview(id.toStdString());
    }
    previewIds_.clear();
    previewModes_.clear();
    previewOwners_.clear();
    previewLastFrame_.clear();   // every tap died with this call — held frames must not resurrect
    previewDeliveredAt_.clear();  // ...and no tap may keep its delivery stamp either
    // The NDI receivers are ENGINE objects, not PAL taps — the AV screen's
    // teardown releases them here too, so a settings-screen close can never
    // leak a live network receiver.
    // Gated on the ATOMIC feature switch, NOT NdiStatus(). NdiStatus() takes
    // the BroadcastEngine mutex and runs a live vendor-SDK probe, and this runs
    // on the GUI thread from a QML handler (AudioVideoScreen's teardown) — the
    // captured hang showed the GUI thread blocked exactly here, waiting for the
    // provider mutex behind NDI SDK work. NdiEnabled() is a lock-free atomic;
    // when it is on, the receiver set is exactly what we are tearing down, and
    // DisconnectReceiver on an absent id is a no-op.
    if (bps::broadcast::BroadcastEngine::Instance().NdiEnabled()) {
        for (const QString &recvId : ndiTaps_.values())
            (void)bps::broadcast::BroadcastEngine::Instance().DisconnectReceiver(
                recvId.toStdString());
    }
    ndiTaps_.clear();
}

// ============================================================================
// Live NDI receiver tap — the input half of the broadcast stack. The NDI
// source's full discovery name ("<HOST> (<sender>)") is the tap key, the
// same string the roster stores as its sublabel; the engine receiver is
// created through the BroadcastEngine's NDI provider (which runtime-loads
// the vendor SDK) and pulled non-blocking on every frame request. Owner
// counting rides the shared previewOwners_ table keyed by the raw source
// name (see ndiTaps_'s own comment on why that's not a collision risk) so a
// card preview and an output take (or two dialogs) can hold the same source
// without fighting — the receiver dies with its last owner, exactly like a
// camera tap.
// ============================================================================
bool EngineBridge::startNdiPreview(const QString &sourceName, const QString &owner)
{
    const QString want = sourceName.trimmed();
    if (want.isEmpty())
        return false;
    auto &owners = previewOwners_[want];
    if (!owners.contains(owner))
        owners.insert(owner);
    if (ndiTaps_.contains(want))
        return true;   // already attached — this owner just joined
    if (!booted()) {
        qWarning("EngineBridge: NDI preview requested for '%s' but the engine is not booted",
                 qUtf8Printable(want));
        return false;
    }
    auto &broadcast = bps::broadcast::BroadcastEngine::Instance();
    auto recv = broadcast.CreateNdiReceiver(want.toStdString());
    if (!recv.ok()) {
        // Runtime absent / SDK load failure / receiver refused: undo the
        // owner claim so a later start (after recheckNdi()) can try again,
        // and surface the reason in the log (the UI keeps its warm-up
        // placeholder — never a fake feed).
        qWarning("EngineBridge: NDI receiver failed for '%s': %s",
                 qUtf8Printable(want), recv.error().message.c_str());
        auto it = previewOwners_.find(want);
        if (it != previewOwners_.end()) {
            it->remove(owner);
            if (it->isEmpty())
                previewOwners_.erase(it);
        }
        return false;
    }
    const QString recvId = qstr(recv.value());   // engine id (std::string) → QString
    ndiTaps_.insert(want, recvId);
    // Warm-up grace on the receiver's clock (see inputDelivery): an NDI feed has
    // a network between it and its first frame.
    previewDeliveredAt_[want] = QDateTime::currentMSecsSinceEpoch();
    qInfo("EngineBridge: NDI receiver '%s' -> %s started", qUtf8Printable(want),
          qUtf8Printable(recvId));
    return true;
}

void EngineBridge::stopNdiPreview(const QString &sourceName, const QString &owner)
{
    const QString want = sourceName.trimmed();
    auto it = previewOwners_.find(want);
    if (it == previewOwners_.end())
        return;
    it->remove(owner);
    if (!it->isEmpty())
        return;   // another consumer still holds this source
    previewOwners_.erase(it);
    const QString recvId = ndiTaps_.take(want);
    previewLastFrame_.remove(want);   // receiver gone — the held frame must not resurrect
    previewDeliveredAt_.remove(want); // ...and neither may its delivery stamp
    previewPulledAt_.remove(want);    // ...nor its "something was watching" clock
    if (!recvId.isEmpty() && booted())
        (void)bps::broadcast::BroadcastEngine::Instance().DisconnectReceiver(recvId.toStdString());
}

QImage EngineBridge::previewNdiFrameFor(const QString &sourceName)
{
    const QString want = sourceName.trimmed();
    const QString recvId = ndiTaps_.value(want);
    if (recvId.isEmpty() || !booted())
        return {};   // no receiver / engine gone — the caller keeps its placeholder
    // WATCHED (see kWatchWindowMs), same as the capture taps: the receiver was
    // asked for its newest frame just now.
    previewPulledAt_[want] = QDateTime::currentMSecsSinceEpoch();
    auto &broadcast = bps::broadcast::BroadcastEngine::Instance();
    bps::broadcast::VideoFrameInfo info;
    std::vector<uint8_t> payload;
    auto r = broadcast.ReceiveFrame(recvId.toStdString(), info, payload);
    if (!r.ok() || !r.value() || payload.empty() || info.width == 0 || info.height == 0)
        // Nothing new this tick (non-blocking capture) — warm-up, or the
        // network feed hiccuped. HOLD-LAST-FRAME (see heldPreviewFrame): the
        // pane keeps showing the newest frame it ever received instead of
        // blinking to its glyph art for the tick. NO DELIVERY STAMP here: this
        // tick carried nothing, and that is exactly the distinction the status
        // light's live half is made of.
        return heldPreviewFrame(want);
    // A real payload arrived: stamp it (see inputDelivery).

    previewDeliveredAt_[want] = QDateTime::currentMSecsSinceEpoch();

    // UYVY (2 bytes/px, Y0 U Y1 V) or BGRA/RGBA (4 bytes/px) → RGBA8 for
    // the QML image pipeline (Format_RGBA8888, the SAME layout the live
    // preview provider hands out). Rows may carry a stride; honor it.
    const int bpp = info.fourCC == 0x59565955 ? 2 : 4;
    const size_t stride = payload.size() / info.height;
    QImage out(int(info.width), int(info.height), QImage::Format_RGBA8888);
    for (uint32_t y = 0; y < info.height; ++y) {
        const uint8_t *row = payload.data() + y * stride;
        uchar *dst = out.scanLine(int(y));
        for (uint32_t x = 0; x < info.width; ++x) {
            if (bpp == 2) {
                // UYVY packs chroma per PIXEL PAIR (Y0 U Y1 V): even pixels
                // own the pair's U/V, odd pixels reuse the preceding pair's
                // (reading x*2+2 on the LAST pixel would run one byte past
                // the final row when the buffer is tightly packed).
                const uint8_t Y = row[x * 2];
                // Chroma reads clamped into the row: an even pixel owns the
                // pair's U/V, an odd pixel reuses the preceding pair's (and
                // a pathological odd-width frame can't read past the end).
                const size_t rowBytes = std::min(stride, size_t(info.width) * 2);
                const uint8_t u = (x & 1) != 0 ? row[x * 2 - 1]
                                               : row[std::min(size_t(x) * 2 + 1, rowBytes - 1)];
                const uint8_t v = (x & 1) != 0 ? row[x * 2 - 2]
                                               : row[std::min(size_t(x) * 2 + 2, rowBytes - 1)];
                const int Cb = int(u) - 128;
                const int Cr = int(v) - 128;
                // BT.601, full range — the conversion the display engine's
                // own RGBA→UYVY uses, inverted.
                const int R = Y + (91881 * Cr >> 16);
                const int G = Y - ((22554 * Cb + 46802 * Cr) >> 16);
                const int B = Y + (116130 * Cb >> 16);
                dst[x * 4 + 0] = uchar(qBound(0, R, 255));
                dst[x * 4 + 1] = uchar(qBound(0, G, 255));
                dst[x * 4 + 2] = uchar(qBound(0, B, 255));
                dst[x * 4 + 3] = 255;
            } else {
                // BGRA and RGBA both map to RGBA8888 by swapping the
                // matching byte pair; the two non-native orders are rare
                // enough to accept as-is (colors would swap, never crash).
                dst[x * 4 + 0] = info.fourCC == 0x41524742 ? row[x * 4 + 2] : row[x * 4 + 0];
                dst[x * 4 + 1] = row[x * 4 + 1];
                dst[x * 4 + 2] = info.fourCC == 0x41524742 ? row[x * 4 + 0] : row[x * 4 + 2];
                dst[x * 4 + 3] = row[x * 4 + 3];
            }
        }
    }
    setHeldPreviewFrame(want, out);   // newest good frame remembered (see heldPreviewFrame)
    return out;
}

void EngineBridge::startRelay()
{
    auto &bus = bps::EventBus::Instance();
    auto *self = this;

    // Every formatter below: build the payload (level/title/message following
    // the UI EventBus's toast convention) under the engine's verbatim topic,
    // then marshal to the GUI thread. Subscriptions stay alive until
    // stopRelay(); engine dispatches between stop and Kernel::Shutdown are
    // impossible because stopRelay() runs first, inside shutdown().
    auto relay = [self](const char *topic, QString level, QString title, QString message) {
        QMetaObject::invokeMethod(self, [self, topic = QString::fromLatin1(topic),
                                         level = std::move(level), title = std::move(title),
                                         message = std::move(message)]() mutable {
            self->ingestEngineEvent(topic, QVariantMap{
                {QStringLiteral("level"), level},
                {QStringLiteral("title"), title},
                {QStringLiteral("message"), message},
            });
        }, Qt::QueuedConnection);
    };

    // ---- Kernel lifecycle -------------------------------------------------
    // state_changed carries STRUCTURED from/to fields (not just a formatted
    // message) — the shared header's connection indicator reacts to the
    // kernel's actual state machine, not a parsed string.
    relaySubs_.push_back(bus.Subscribe<bps::events::KernelStateChanged>(
        [self](const bps::events::KernelStateChanged &e) {
            const QString from = QString::fromLatin1(bps::ToString(e.from));
            const QString to = QString::fromLatin1(bps::ToString(e.to));
            QMetaObject::invokeMethod(self, [self, from, to]() {
                self->ingestEngineEvent(QStringLiteral("engine.kernel.state_changed"), QVariantMap{
                    {QStringLiteral("level"), to == QStringLiteral("Running")
                         ? QStringLiteral("success") : QStringLiteral("info")},
                    {QStringLiteral("title"), QStringLiteral("Engine")},
                    {QStringLiteral("message"), QStringLiteral("Kernel: %1 → %2").arg(from, to)},
                    {QStringLiteral("from"), from},
                    {QStringLiteral("to"), to},
                });
            }, Qt::QueuedConnection);
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::KernelPanic>(
        [relay](const bps::events::KernelPanic &e) {
            relay("engine.kernel.panic", QStringLiteral("error"), QStringLiteral("Engine panic"),
                  qstr(e.error.message));
        }));
    // A subsystem that could not initialize: the engine is UP and this part of
    // it is not. Warning, not error — and named, so the user learns WHICH
    // feature is unavailable instead of watching every screen answer "the
    // engine is not running yet". (Boot-time ones are replayed in boot().)
    relaySubs_.push_back(bus.Subscribe<bps::events::SubsystemDegraded>(
        [relay](const bps::events::SubsystemDegraded &e) {
            relay("engine.kernel.subsystem_degraded", QStringLiteral("warning"),
                  QStringLiteral("Engine"),
                  QStringLiteral("%1 is unavailable this session — %2")
                      .arg(qstr(e.system), qstr(e.message)));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::SystemHealthChanged>(
        [relay](const bps::events::SystemHealthChanged &e) {
            if (e.report.IsHealthy())
                return;   // healthy chatter stays off the wire — only regressions surface
            relay("engine.system.health_changed",
                  e.report.state == bps::HealthState::Failing
                      ? QStringLiteral("error") : QStringLiteral("warning"),
                  QStringLiteral("Engine health"),
                  QStringLiteral("%1: %2").arg(qstr(e.system),
                      e.report.lastError.empty() ? qstr(e.report.detail) : qstr(e.report.lastError)));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::ResourcePressureHigh>(
        [relay](const bps::events::ResourcePressureHigh &e) {
            // One toast per SUSTAINED episode (the engine's PressureLatch raises this only
            // after the load held for a while and rate-limits repeats), worded for the
            // resource that is actually under pressure.
            const QString res = qstr(e.resource);
            const QString level = QString::fromLatin1(bps::ToString(e.level));
            QString title = QStringLiteral("High %1 usage").arg(res);
            QString text = QStringLiteral("%1 load is %2.").arg(res, level.toLower());
            if (e.resource == "memory") {
                title = QStringLiteral("Low memory");
                text = QStringLiteral("Memory use is %1 — close other apps if playback stutters.").arg(level.toLower());
            } else if (e.resource == "cpu") {
                title = QStringLiteral("High CPU usage");
                text = QStringLiteral("The CPU has stayed at %1 load for a while — heavy work in other apps may slow the presentation.").arg(level.toLower());
            } else if (e.resource == "disk") {
                title = QStringLiteral("Low disk space");
                text = QStringLiteral("Disk usage is %1.").arg(level.toLower());
            }
            relay("engine.resource.pressure_high", QStringLiteral("warning"), title, text);
        }));

    // ---- Content / project notices (formerly reached the UI only as formatted engine
    // log lines — "Engine · Core [Warning]..." — via the Notification Service's console
    // channel; the log sink now skips those, so each event gets ONE properly worded toast).
    relaySubs_.push_back(bus.Subscribe<bps::events::ContentAssetImported>(
        [relay](const bps::events::ContentAssetImported &e) {
            relay("content.asset_imported", QStringLiteral("success"), QStringLiteral("Import finished"),
                  QStringLiteral("%1 (%2 asset%3)").arg(qstr(e.name)).arg(e.assetsCreated)
                      .arg(e.assetsCreated == 1 ? QString() : QStringLiteral("s")));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::ContentAssetDeleted>(
        [relay](const bps::events::ContentAssetDeleted &) {
            relay("content.asset_deleted", QStringLiteral("info"), QStringLiteral("Asset deleted"),
                  QStringLiteral("The asset was removed from the library."));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::ContentValidationFailed>(
        [relay](const bps::events::ContentValidationFailed &e) {
            relay("content.validation_failed", QStringLiteral("warning"), QStringLiteral("Validation failed"),
                  qstr(e.reason));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::ProjectCreated>(
        [relay](const bps::events::ProjectCreated &e) {
            relay("project.created", QStringLiteral("success"), QStringLiteral("Project created"), qstr(e.name));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::PackageExported>(
        [relay](const bps::events::PackageExported &e) {
            relay("project.package_exported", QStringLiteral("success"), QStringLiteral("Package exported"),
                  QStringLiteral("%1 (%2 asset%3)").arg(qstr(e.path)).arg(e.assetCount)
                      .arg(e.assetCount == 1 ? QString() : QStringLiteral("s")));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::BackupCompleted>(
        [relay](const bps::events::BackupCompleted &e) {
            relay("project.backup_completed", QStringLiteral("success"), QStringLiteral("Backup completed"),
                  qstr(e.destination));
        }));

    // ---- Platform hot plug -------------------------------------------------
    // The kernel's platform watcher drains IPlatform::PollChanges() and
    // publishes DeviceConnected/DeviceRemoved (audio fingerprint diff) and
    // MonitorConnected/MonitorDisconnected. Each one re-enumerates the
    // device lists (so every open select reflects the hardware the instant
    // it changes) and surfaces the change as a standard engine event.
    auto deviceChange = [self](const char *topic, QString title, QString message) {
        QMetaObject::invokeMethod(self, [self, topic = QString::fromLatin1(topic),
                                         title = std::move(title), message = std::move(message)]() mutable {
            self->enumerateDevices();
            self->ingestEngineEvent(topic, QVariantMap{
                {QStringLiteral("level"), QStringLiteral("info")},
                {QStringLiteral("title"), title},
                {QStringLiteral("message"), message},
            });
        }, Qt::QueuedConnection);
    };
    relaySubs_.push_back(bus.Subscribe<bps::events::DeviceConnected>(
        [deviceChange](const bps::events::DeviceConnected &e) {
            deviceChange("platform.device_connected", QStringLiteral("Audio device"),
                         QStringLiteral("%1 connected").arg(qstr(e.device)));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::DeviceRemoved>(
        [deviceChange](const bps::events::DeviceRemoved &e) {
            deviceChange("platform.device_removed", QStringLiteral("Audio device"),
                         QStringLiteral("%1 removed").arg(qstr(e.device)));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::MonitorConnected>(
        [deviceChange](const bps::events::MonitorConnected &e) {
            deviceChange("platform.monitor_connected", QStringLiteral("Display"),
                         QStringLiteral("%1 connected").arg(qstr(e.monitorId)));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::MonitorDisconnected>(
        [deviceChange](const bps::events::MonitorDisconnected &e) {
            deviceChange("platform.monitor_disconnected", QStringLiteral("Display"),
                         QStringLiteral("%1 disconnected").arg(qstr(e.monitorId)));
        }));

    // ---- Power (AC ↔ battery) — battery percent / AC state
    auto powerChange = [self](const char *topic, const QString &message, const QString &level) {
        QMetaObject::invokeMethod(self, [self, topic = QString::fromLatin1(topic),
                                         message, level]() mutable {
            SettingsService::instance().refreshHardware();
            self->ingestEngineEvent(topic, QVariantMap{
                {QStringLiteral("level"), level},
                {QStringLiteral("title"), QStringLiteral("Power")},
                {QStringLiteral("message"), message},
            });
        }, Qt::QueuedConnection);
    };
    const auto powerToastState = std::make_shared<PowerToastState>();
    relaySubs_.push_back(bus.Subscribe<bps::events::PowerChanged>(
        [self, powerChange, powerToastState](const bps::events::PowerChanged &e) {
            const bool onBattery = e.detail.find("on battery") != std::string::npos;
            int percent = -1;
            const auto open = e.detail.find('(');
            const auto mark = e.detail.find('%', open == std::string::npos ? 0 : open + 1);
            if (open != std::string::npos && mark != std::string::npos) {
                try {
                    percent = std::stoi(e.detail.substr(open + 1, mark - open - 1));
                } catch (...) {
                    percent = -1;
                }
            }

            bool initialized = false;
            bool powerStateChanged = false;
            int previousPercent = -1;
            {
                std::lock_guard<std::mutex> lock(powerToastState->mutex);
                initialized = powerToastState->initialized;
                previousPercent = powerToastState->lastPercent;
                powerStateChanged = initialized && powerToastState->lastOnBattery != onBattery;
                if (percent >= 0) powerToastState->lastPercent = percent;
                powerToastState->lastOnBattery = onBattery;
                powerToastState->initialized = true;
            }

            if (powerStateChanged) {
                const QString message = onBattery
                    ? QStringLiteral("Running on battery — background work is paused so the show lasts. %1")
                          .arg(qstr(e.detail))
                    : QStringLiteral("Plugged in — the full profile is available again. %1")
                          .arg(qstr(e.detail));
                powerChange("platform.power_changed", message,
                            onBattery ? QStringLiteral("warning") : QStringLiteral("info"));
            } else {
                // Keep the live Smart Config battery row current without
                // showing a toast for every one-percent hardware update.
                QMetaObject::invokeMethod(self, [self] {
                    SettingsService::instance().refreshHardware();
                }, Qt::QueuedConnection);
            }

            if (!initialized || previousPercent < 0 || percent < 0 || percent == previousPercent)
                return;

            static constexpr int levels[] = {30, 50, 80, 100};
            const bool charging = percent > previousPercent;
            for (const int levelPercent : levels) {
                const bool crossed = charging
                    ? previousPercent < levelPercent && percent >= levelPercent
                    : previousPercent >= levelPercent && percent < levelPercent;
                if (!crossed) continue;

                const QString direction = charging
                    ? QStringLiteral("charging") : QStringLiteral("discharging");
                powerChange("platform.battery_level",
                            QStringLiteral("Battery %1: %2%")
                                .arg(direction).arg(levelPercent),
                            charging ? QStringLiteral("info") : QStringLiteral("warning"));
            }
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::BatteryLow>(
        [powerChange](const bps::events::BatteryLow &e) {
            powerChange("platform.battery_low",
                        QStringLiteral("Battery is low (%1) - plug in soon so the show isn't cut short.")
                            .arg(qstr(e.detail)),
                        QStringLiteral("warning"));
        }));

    // ---- Display / outputs -------------------------------------------------
    relaySubs_.push_back(bus.Subscribe<bps::events::DisplayFailed>(
        [relay](const bps::events::DisplayFailed &e) {
            relay("display.failed", QStringLiteral("error"), QStringLiteral("Display failed"),
                  QStringLiteral("%1: %2").arg(qstr(e.deviceId), qstr(e.detail)));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::DisplayRestored>(
        [relay](const bps::events::DisplayRestored &e) {
            relay("display.restored", QStringLiteral("success"), QStringLiteral("Display restored"),
                  QStringLiteral("Device %1 back online").arg(qstr(e.deviceId)));
        }));

    // ---- Render / media ----------------------------------------------------
    relaySubs_.push_back(bus.Subscribe<bps::events::RenderError>(
        [relay](const bps::events::RenderError &e) {
            relay("render.error", QStringLiteral("error"), QStringLiteral("Render error"), qstr(e.detail));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::RenderGpuOutOfMemory>(
        [relay](const bps::events::RenderGpuOutOfMemory &e) {
            relay("render.gpu_out_of_memory", QStringLiteral("error"), QStringLiteral("GPU out of memory"),
                  qstr(e.detail));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::MediaDecodeFailed>(
        [relay](const bps::events::MediaDecodeFailed &e) {
            relay("media.decode_failed", QStringLiteral("error"), QStringLiteral("Media failed to decode"),
                  qstr(e.detail));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::MediaImported>(
        [relay](const bps::events::MediaImported &e) {
            relay("media.imported", QStringLiteral("success"), QStringLiteral("Media added"),
                  QStringLiteral("%1 (%2)").arg(qstr(e.name), qstr(e.type)));
        }));

    // ---- Recording ----------------------------------------------------------
    relaySubs_.push_back(bus.Subscribe<bps::events::RecordingFailed>(
        [relay](const bps::events::RecordingFailed &e) {
            relay("recording.failed", QStringLiteral("error"), QStringLiteral("Recording failed"), qstr(e.error));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::RecordingRecovered>(
        [relay](const bps::events::RecordingRecovered &e) {
            relay("recording.recovered", QStringLiteral("success"), QStringLiteral("Recording recovered"),
                  QStringLiteral("%1 (%2 segment(s) safe)").arg(qstr(e.recordingId)).arg(e.segments));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::DiskSpaceWarning>(
        [relay](const bps::events::DiskSpaceWarning &e) {
            relay("recording.disk_space_warning",
                  e.level == "emergency" ? QStringLiteral("error") : QStringLiteral("warning"),
                  QStringLiteral("Disk space"),
                  QStringLiteral("%1% free on the recording drive").arg(e.freePercent));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::DroppedFramesDetected>(
        [relay](const bps::events::DroppedFramesDetected &e) {
            relay("recording.dropped_frames", QStringLiteral("warning"), QStringLiteral("Dropped frames"),
                  QStringLiteral("%1 frame(s) dropped during recording").arg(qulonglong(e.count)));
        }));

    // ---- Broadcast ----------------------------------------------------------
    relaySubs_.push_back(bus.Subscribe<bps::events::BroadcastError>(
        [relay](const bps::events::BroadcastError &e) {
            relay("broadcast.error", QStringLiteral("error"), QStringLiteral("Stream error"),
                  QStringLiteral("%1: %2").arg(qstr(e.provider), qstr(e.message)));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::BroadcastProviderUnavailable>(
        [relay](const bps::events::BroadcastProviderUnavailable &e) {
            relay("broadcast.provider_unavailable", QStringLiteral("error"),
                  QStringLiteral("Stream provider unavailable"),
                  QStringLiteral("%1: %2").arg(qstr(e.provider), qstr(e.reason)));
        }));

    // ---- Project / undo-redo (the engine already publishes these from its
    // own UndoRedoManager — the UI's history activity now shows up here too)
    relaySubs_.push_back(bus.Subscribe<bps::events::ProjectSaved>(
        [relay](const bps::events::ProjectSaved &e) {
            relay("project.saved", QStringLiteral("success"), QStringLiteral("Project saved"),
                  e.autosave ? QStringLiteral("%1 (autosaved)").arg(qstr(e.name)) : qstr(e.name));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::RecoveryAvailable>(
        [relay](const bps::events::RecoveryAvailable &e) {
            relay("project.recovery_available", QStringLiteral("warning"), QStringLiteral("Recovery available"),
                  QStringLiteral("%1: %2").arg(qstr(e.projectId), qstr(e.detail)));
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::UndoPerformed>(
        [relay, self](const bps::events::UndoPerformed &e) {
            // Silent-telemetry shape (no "message" key): delivered to every
            // subscriber, but NOT a toast — one per undo keystroke would spam.
            QMetaObject::invokeMethod(self, [self, name = qstr(e.commandName), depth = e.depth]() {
                self->ingestEngineEvent(QStringLiteral("project.undo_performed"), QVariantMap{
                    {QStringLiteral("level"), QStringLiteral("info")},
                    {QStringLiteral("title"), QStringLiteral("Undo")},
                    {QStringLiteral("command"), name},
                    {QStringLiteral("depth"), qulonglong(depth)},
                });
            }, Qt::QueuedConnection);
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::RedoPerformed>(
        [relay, self](const bps::events::RedoPerformed &e) {
            QMetaObject::invokeMethod(self, [self, name = qstr(e.commandName), depth = e.depth]() {
                self->ingestEngineEvent(QStringLiteral("project.redo_performed"), QVariantMap{
                    {QStringLiteral("level"), QStringLiteral("info")},
                    {QStringLiteral("title"), QStringLiteral("Redo")},
                    {QStringLiteral("command"), name},
                    {QStringLiteral("depth"), qulonglong(depth)},
                });
            }, Qt::QueuedConnection);
        }));

    // ---- Production graph changes made outside the UI's own models ----------
    // Silent telemetry (no "message": never a toast). BusListModel listens for
    // these on engineEvent and re-syncs its cached rows from the graph.
    relaySubs_.push_back(bus.Subscribe<bps::events::ProductionRestored>(
        [self](const bps::events::ProductionRestored &e) {
            QMetaObject::invokeMethod(self, [self, name = qstr(e.name)]() {
                self->ingestEngineEvent(QStringLiteral("production.restored"), QVariantMap{
                    {QStringLiteral("level"), QStringLiteral("info")},
                    {QStringLiteral("title"), QStringLiteral("Production")},
                    {QStringLiteral("name"), name},
                });
            }, Qt::QueuedConnection);
        }));
    relaySubs_.push_back(bus.Subscribe<bps::events::BusChanged>(
        [self](const bps::events::BusChanged &e) {
            QMetaObject::invokeMethod(self, [self, busId = qstr(e.busId), change = qstr(e.change)]() {
                self->ingestEngineEvent(QStringLiteral("production.bus_changed"), QVariantMap{
                    {QStringLiteral("level"), QStringLiteral("info")},
                    {QStringLiteral("title"), QStringLiteral("Production")},
                    {QStringLiteral("busId"), busId},
                    {QStringLiteral("change"), change},
                });
            }, Qt::QueuedConnection);
        }));

    // ---- Engine log lines ≥ Warning (subsystem errors, panics, subscriber
    // exceptions — everything the engine itself logged) -----------------------
    (void)bps::Logger::Instance().AddSink(std::make_shared<EngineLogSink>(this));
}

void EngineBridge::stopRelay()
{
    auto &bus = bps::EventBus::Instance();
    for (bps::Subscription &s : relaySubs_)
        (void)bus.Unsubscribe(s);
    relaySubs_.clear();
    // The sink is owned by the Logger's shared_ptr vector; detach it from us
    // before the bridge (or the kernel) can go away.
    (void)bps::Logger::Instance().RemoveSink("UiRelay");
}

void EngineBridge::ingestEngineEvent(const QString &topic, const QVariantMap &payload)
{
    // GUI thread only (all producers marshal here before calling).
    QVariantMap entry = payload;
    entry.insert(QStringLiteral("topic"), topic);
    entry.insert(QStringLiteral("ts"), QDateTime::currentMSecsSinceEpoch());
    // Origin stamp — the mirror of EventBus::notify's origin="ui": lets any
    // subscriber (activity log, filters, the overlay) tell engine-raised
    // traffic from UI-raised without parsing topic strings.
    if (!entry.contains(QStringLiteral("origin")))
        entry.insert(QStringLiteral("origin"), QStringLiteral("engine"));
    recentEngineEvents_.append(entry);
    while (recentEngineEvents_.size() > kMaxRecentEvents)
        recentEngineEvents_.removeFirst();
    emit engineEvent(topic, payload);
    // Same channel NotificationCenter subscribes to — a payload carrying a
    // non-empty "message" becomes a toast, one without stays telemetry.
    EventBus::instance().publish(topic, payload);
}

QVariantList EngineBridge::recentEngineEvents(int max) const
{
    QVariantList out = recentEngineEvents_;
    while (out.size() > max)
        out.removeFirst();
    return out;
}

// ============================================================================
// (REMOVED 2026-10) THE ENGINE'S AUDIO OUT.
//
// There used to be audioRenderActive()/setAudioRender() here: a WASAPI render
// client plus the AudioMixer summing the graph's buses into it, with the meter
// pump's refreshMixSources() acquiring a capture tap per routed source to feed
// it. Nothing in the app ever called setAudioRender (the Audio & Video screen's
// manual toggle was removed by request), so the whole path was unreachable —
// and it was the only consumer of those mix-source captures. What was left was
// the cost with none of the function: the pump kept acquiring WASAPI capture
// sessions on every routed input, for a meter tick, silently (the PAL logs no
// line when a capture starts) — the "go live is opening WASAPI" report. Gone,
// and with it the only code in the bridge that could open a capture device for
// a reason other than a user-held meter. The engine's AudioMixer class and its
// unit tests stay: they are the design record for a real playout path, which
// now has to be built on purpose instead of half-existing.
// ============================================================================

// ============================================================================
// STREAM meters — a video/NDI row's OWN audio.
//
// A media file and an NDI stream carry audio that no capture device can reach,
// so the PAL's stream tap (IAudio::StartStreamTap) is the one carrier for it.
// The tap publishes through the SAME two entry points a device tap does
// (InputLevels + ReadTapAudio), which is why a video row needs no second meter
// path in the UI: it lands in inputLevels under its own label and the mixer
// drains it like any other source.
// ============================================================================
bool EngineBridge::ensureStreamTap(const QString &label, const QString &kind,
                                   const QString &source)
{
    if (label.isEmpty() || !bps::platform::PlatformAccessor::Installed())
        return false;
    if (streamTapIds_.contains(label))
        return true;   // already open — a decoder is never restarted under its own meters
    auto &audio = bps::platform::PlatformAccessor::Get().Audio();
    const QString src = source.trimmed().isEmpty() ? label : source.trimmed();
    // A REFUSED open is retried on a slow beat, not on every call: the mix's
    // source refresh runs ~20×/s while rendering, so a row whose media file is
    // gone (or an NDI kind nothing feeds yet) would otherwise re-attempt the
    // decode and write a warning line 20 times a second.
    const QString refusedKey = label + QLatin1Char('\n') + kind;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const auto refused = streamRefusedAt_.constFind(refusedKey);
    if (refused != streamRefusedAt_.constEnd() && nowMs - refused.value() < 10000)
        return false;
    const uint32_t tapId = bps::platform::IAudio::kStreamTapBase + nextStreamTap_++;
    const auto started = audio.StartStreamTap(tapId, src.toStdString(), kind.toStdString());
    if (!started.ok()) {
        streamRefusedAt_[refusedKey] = nowMs;
        // The HONEST refusal — a file that is not there (or an NDI runtime that
        // is not loaded) gets no tap at all: the row meters silence rather than
        // a level nothing produced. Logged loudly, because from the UI a refused
        // tap and a genuinely silent source look identical.
        write(QStringLiteral("warning"), QStringLiteral("AudioMeters"),
              QStringLiteral("stream meter refused for '%1' (%2, opens '%3'): %4 "
                             "(retried every 10 s while the row stays routed)")
                  .arg(label, kind, src)
                  .arg(QString::fromStdString(started.error().message)));
        return false;
    }
    streamRefusedAt_.remove(refusedKey);
    streamTapIds_[label] = tapId;
    streamKinds_[label] = kind;
    streamSources_[label] = src;
    write(QStringLiteral("info"), QStringLiteral("AudioMeters"),
          QStringLiteral("stream meter tap live for '%1' (%2, id %3, opens '%4')")
              .arg(label, kind)
              .arg(tapId)
              .arg(src));
    return true;
}

void EngineBridge::releaseStreamTap(const QString &label)
{
    const auto known = streamTapIds_.find(label);
    if (known == streamTapIds_.end())
        return;
    // STILL HELD? A UI consumer (requestedStreams_) or the playout mix (a claim
    // naming this same tap) keeps it open — one decoder, one row.
    if (requestedStreams_.contains(label))
        return;
    if (bps::platform::PlatformAccessor::Installed())
        (void)bps::platform::PlatformAccessor::Get().Audio().StopStreamTap(known.value());
    write(QStringLiteral("info"), QStringLiteral("AudioMeters"),
          QStringLiteral("stream meter tap closed for '%1'").arg(label));
    streamTapIds_.erase(known);
    streamKinds_.remove(label);
    streamSources_.remove(label);
}

void EngineBridge::startStreamMeter(const QString &label, const QString &kind,
                                    const QString &source)
{
    const QString want = label.trimmed();
    // An EMPTY label is refused for the same reason a device label is (see
    // startInputMeter): a tap belongs to a source the user actually chose, and
    // a row with nothing picked must read "no signal", not open a decoder.
    if (want.isEmpty() || !bps::platform::PlatformAccessor::Installed())
        return;
    const bool firstHold = !requestedStreams_.contains(want);
    ++requestedStreams_[want];
    if (firstHold) {
        streamKinds_[want] = kind;
        streamSources_[want] = source;
        ensureStreamTap(want, kind, source);
    } else {
        // A re-hold with a DIFFERENT source (the row was re-picked) must move
        // the decoder: the old tap goes, a new one opens on the new file.
        const QString src = source.trimmed().isEmpty() ? want : source.trimmed();
        if (streamSources_.value(want) != src || streamKinds_.value(want) != kind) {
            const auto old = streamTapIds_.take(want);
            if (old != 0)
                (void)bps::platform::PlatformAccessor::Get().Audio().StopStreamTap(old);
            ensureStreamTap(want, kind, source);
        }
    }
    ensureMeterPump();
    emit audioMeteringChanged();
}

void EngineBridge::stopStreamMeter(const QString &label)
{
    const QString want = label.trimmed();
    const auto it = requestedStreams_.find(want);
    if (it == requestedStreams_.end())
        return;   // not held here — nothing to release
    if (--it.value() > 0)
        return;   // another card or dialog still holds this row open
    requestedStreams_.erase(it);
    releaseStreamTap(want);
    if (requestedMeters_.isEmpty() && requestedStreams_.isEmpty()) {
        if (inputMeterPump_)
            inputMeterPump_->stop();
        if (!inputLevels_.isEmpty()) {
            inputLevels_.clear();
            emit inputLevelsChanged();
        }
    }
    emit audioMeteringChanged();
}

// Which of these blocks the ENGINE will paint under the compositor's video
// layer. Asked of the rule's ONE home (SceneBuilder::PlanBlockLayers) rather
// than re-derived here: the monitor tile needs the same answer every real
// output gets, or the tile and the outputs disagree about what covers a feed.
// The blocks arrive as the QML maps ShowConverter is the single mapping for, so
// they go back through the same conversion the rest of this bridge uses.
QVariantList EngineBridge::backdropBlockIndexes(const QVariantList &blocks) const
{
    std::vector<bps::presentation::ContentBlock> converted;
    converted.reserve(static_cast<size_t>(blocks.size()));
    for (const QVariant &b : blocks)
        converted.push_back(ShowConverter::blockFromVariant(b.toMap()));
    const bps::presentation::SceneBuilder::BlockLayerPlan plan =
        bps::presentation::SceneBuilder::PlanBlockLayers(converted);
    QVariantList out;
    for (size_t i = 0; i < plan.underVideo.size(); ++i)
        if (plan.underVideo[i])
            out.append(static_cast<int>(i));
    return out;
}
