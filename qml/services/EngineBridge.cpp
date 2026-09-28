#include "services/EngineBridge.h"
#include "services/EventBus.h"

#include "core/kernel/Kernel.hpp"
#include "core/logging/Logger.hpp"
#include "core/events/Events.hpp"
#include "modules/project/UndoRedoManager.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QQmlEngine>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <cmath>

#include <atomic>

namespace {

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

    bootError_.clear();
    // Startup summary for the UI's boot toast: what booted AND what the
    // platform layer found — real device counts, so "loaded" is a fact, not
    // a hope. Names listed (up to a few) make it informative at a glance.
    {
        QStringList audioNames;
        for (const QVariant &v : audioDevices_) {
            audioNames << v.toMap().value("label").toString();
            if (audioNames.size() >= 3) break;
        }
        QStringList screenNames;
        for (const QVariant &v : screenDevices_) {
            screenNames << v.toMap().value("label").toString();
            if (screenNames.size() >= 3) break;
        }
        bootSummary_ = QStringLiteral("Engine booted (%1 systems) · %2 audio device(s): %3 · %4 display(s): %5")
            .arg(bps::Kernel::Instance().BootLog().size())
            .arg(audioDevices_.size()).arg(audioNames.join(QStringLiteral(", ")))
            .arg(screenDevices_.size()).arg(screenNames.join(QStringLiteral(", ")));
    }
    EventBus::instance().publish(QStringLiteral("engine.boot"), QVariantMap{
        {QStringLiteral("level"), QStringLiteral("success")},
        {QStringLiteral("title"), QStringLiteral("Engine")},
        {QStringLiteral("message"), QStringLiteral("Presentation engine booted (%1 systems).")
            .arg(bps::Kernel::Instance().BootLog().size())},
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

} // namespace

// Enumerates the REAL hardware through the engine's own platform layer —
// the PAL's IAudio (WinMM waveIn/waveOut on Windows) for capture/output
// devices, IMonitor for connected displays, IVideo (Media Foundation) for
// cameras, and the BroadcastEngine's NDI provider for network sources.
// GUI-thread-only (touches the QVariantList members QML binds to); called
// from boot() and from the hot-plug relay handlers below. Safe when no PAL
// backend is installed (yields empty lists — QML keeps its own fallback
// roster then).
void EngineBridge::enumerateDevices()
{
    lastEnumerationMs_ = QDateTime::currentMSecsSinceEpoch();
    audioDevices_.clear();
    screenDevices_.clear();
    videoDevices_.clear();
    if (bps::platform::PlatformAccessor::Installed()) {
        auto &platform = bps::platform::PlatformAccessor::Get();
        for (const auto &d : platform.Audio().Enumerate()) {
            const QString name = qstr(d.name);
            audioDevices_.append(QVariantMap{
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
        for (const auto &m : platform.Monitor().Enumerate()) {
            const QString name = qstr(m.name);
            screenDevices_.append(QVariantMap{
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
            screenDevices_.append(QVariantMap{
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
        for (const auto &d : platform.Video().Enumerate()) {
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
            videoDevices_.append(QVariantMap{
                {QStringLiteral("id"), qstr(d.id)},
                {QStringLiteral("label"), name},
                {QStringLiteral("value"), name},   // AV board stores the readable sublabel
                {QStringLiteral("modes"), modes},
                {QStringLiteral("maxFps"), int(d.maxFps)},
            });
        }

        // NDI network sources through the ENGINE's own broadcast stack (the
        // NDI provider runtime-loads the SDK; when it's absent this yields
        // an empty list and an explanatory flag — never a mock roster).
        // Providers register at kernel boot — before that there is no stack
        // to ask, so the status says so instead of surfacing an internal
        // "provider not found" error.
        ndiSources_.clear();
        ndiAvailable_ = false;
        ndiStatus_.clear();
        ndiVersion_.clear();
        ndiState_ = QStringLiteral("unknown");
        if (booted()) {
            auto &broadcast = bps::broadcast::BroadcastEngine::Instance();
            using NdiState = bps::broadcast::BroadcastEngine::NdiRuntimeStatus::State;
            const auto ndi = broadcast.NdiStatus();
            ndiAvailable_ = ndi.state == NdiState::Ready;
            switch (ndi.state) {
            case NdiState::Ready:
                ndiState_ = QStringLiteral("ready");
                ndiVersion_ = qstr(ndi.version);
                break;
            case NdiState::NotInstalled:
                // The actionable case: the UI offers the download page.
                ndiState_ = QStringLiteral("notInstalled");
                ndiStatus_ = QStringLiteral("The NDI runtime isn't installed on this computer.");
                break;
            case NdiState::Error:
                ndiState_ = QStringLiteral("error");
                ndiStatus_ = qstr(ndi.detail);
                break;
            }
            if (ndiAvailable_) {
                auto sources = broadcast.DiscoverNdiSources();
                if (sources.ok()) {
                    for (const auto &s : sources.value()) {
                        const QString nm = qstr(s.name);
                        ndiSources_.append(QVariantMap{
                            {QStringLiteral("id"), qstr(s.urlAddress)},
                            {QStringLiteral("label"), nm},
                            {QStringLiteral("value"), nm},
                            {QStringLiteral("url"), qstr(s.urlAddress)},
                        });
                    }
                    // First discovery pass after the finder's creation is
                    // usually empty (the SDK browses in the background) —
                    // one deferred re-query lets the cache warm up without
                    // blocking this one.
                    // Once per refreshDevices()/boot — the re-query itself must not
                    // re-arm, or an empty network loops forever (and re-runs the
                    // camera enumeration each pass).
                    if (ndiSources_.isEmpty() && !ndiRetried_) {
                        ndiRetried_ = true;
                        QTimer::singleShot(1200, this, [this]() { if (booted()) enumerateDevices(); });
                    }
                } else {
                    ndiAvailable_ = false;
                    ndiStatus_ = QString::fromStdString(sources.error().message);
                }
            }
        }
    }
    emit devicesChanged();
}

// Where the vendor's runtime is downloaded (NDI Tools includes it). Kept here,
// not in QML, so there is exactly one place to update if the vendor moves it.
QString EngineBridge::ndiDownloadUrl() const
{
    return QStringLiteral("https://ndi.video/tools/");
}

void EngineBridge::openNdiDownloadPage()
{
    QDesktopServices::openUrl(QUrl(ndiDownloadUrl()));
}

// "I installed it — check again": bypasses refreshDevices()'s throttle so the
// UI reacts immediately instead of making the user wait out the window.
void EngineBridge::recheckNdi()
{
    lastEnumerationMs_ = 0;
    ndiRetried_ = false;
    enumerateDevices();
}

void EngineBridge::refreshDevices()
{
    // UI code calls this whenever a device-picking dialog opens. Enumeration is
    // synchronous on the GUI thread and Media Foundation briefly activates each
    // camera (can stall, can flash capture LEDs), so back-to-back requests
    // reuse the last result. Hot-plug relay handlers call enumerateDevices()
    // directly and are not throttled.
    constexpr qint64 kMinRefreshGapMs = 10000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastEnumerationMs_ > 0 && now - lastEnumerationMs_ < kMinRefreshGapMs)
        return;
    ndiRetried_ = false;
    enumerateDevices();
}

// ============================================================================
// Live input metering — the UI half of the PAL's WASAPI capture tap.
//
// Taps are a SET keyed by device (the PAL holds one tap per waveIn id):
// the AV screen starts a meter for every device row on the board (and one
// for the default input on the Add dialog), each consumer reads its own
// device's snapshot from the inputLevels list, and stopInputMeter(label)
// releases just that device when its row goes away. Resolution re-runs on
// every enumeration so a hot-plugged device re-binds to its requested tap.
// A 50 ms GUI-side pump republishes inputLevels_ while any tap is live.
// ============================================================================
void EngineBridge::resolveAndStartMeter(const QString &deviceLabel)
{
    auto &audio = bps::platform::PlatformAccessor::Get().Audio();

    // Resolve the roster label → waveIn device number from the CACHED
    // roster (audioDevices_) — a live IAudio::Enumerate() here re-ran the
    // WASAPI mix-format COM queries on every kind chip / device pick, and
    // that storm stalled the dialogs exactly like the video one did.
    const QString want = deviceLabel.trimmed();
    QString id;
    for (const QVariant &v : audioDevices_) {
        const QVariantMap d = v.toMap();
        if (!d.value("isInput").toBool())
            continue;
        const bool isDefault = d.value("isDefault").toBool();
        if (want.isEmpty() ? isDefault : d.value("label").toString() == want) {
            id = d.value("id").toString();
            break;
        }
    }
    if (id.isEmpty())
        return;   // unknown label (unplugged) — retries on next enumeration
    // Windows ids are "wavein:<n>"; other platforms have no WASAPI tap to
    // address (their IAudio keeps the default Unsupported metering) — never
    // guess a number from an alien id scheme.
    if (!id.startsWith(QStringLiteral("wavein:")))
        return;
    const uint32_t deviceId = static_cast<uint32_t>(id.mid(7).toULong());
    if (audio.StartInputMeter(deviceId).ok())
        inputLabels_[deviceId] = want;
}

void EngineBridge::startInputMeter(const QString &deviceLabel)
{
    if (!bps::platform::PlatformAccessor::Installed())
        return;
    requestedMeters_.insert(deviceLabel.trimmed());
    resolveAndStartMeter(deviceLabel);

    if (!inputMeterPump_) {
        inputMeterPump_ = new QTimer(this);
        inputMeterPump_->setInterval(50);
        connect(inputMeterPump_, &QTimer::timeout, this, [this]() {
            if (!bps::platform::PlatformAccessor::Installed())
                return;   // platform torn down mid-pump (shutdown) — idle out
            auto &a = bps::platform::PlatformAccessor::Get().Audio();
            QVariantList levels;
            for (auto it = inputLabels_.constBegin(); it != inputLabels_.constEnd(); ++it) {
                const uint32_t id = it.key();
                const auto snap = a.InputLevels(id);
                QVariantList peaks;
                QVariantList rms;
                for (int c = 0; c < snap.channelCount; ++c) {
                    peaks.append(snap.peaks[c]);
                    rms.append(snap.rms[c]);
                }
                levels.append(QVariantMap{
                    {QStringLiteral("deviceId"), (int)id},
                    {QStringLiteral("label"), it.value()},
                    {QStringLiteral("channelCount"), snap.channelCount},
                    {QStringLiteral("layout"),
                     QString::fromLatin1(bps::platform::IAudio::InputMeterLevels::ToString(snap.layout))},
                    {QStringLiteral("sampleRateHz"), (int)snap.sampleRateHz},
                    {QStringLiteral("framesCaptured"), (qulonglong)snap.framesCaptured},
                    {QStringLiteral("peaks"), peaks},
                    {QStringLiteral("rms"), rms},
                });
            }
            inputLevels_ = std::move(levels);
            emit inputLevelsChanged();
        });
    }
    if (!inputMeterPump_->isActive())
        inputMeterPump_->start();
}

void EngineBridge::stopInputMeter(const QString &deviceLabel)
{
    const QString want = deviceLabel.trimmed();
    if (!requestedMeters_.remove(want))
        return;   // not metering that label — nothing to release
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
    // No requested meters left: stop the pump and publish the empty set.
    if (requestedMeters_.isEmpty()) {
        if (inputMeterPump_)
            inputMeterPump_->stop();
        inputLabels_.clear();
        if (!inputLevels_.isEmpty()) {
            inputLevels_.clear();
            emit inputLevelsChanged();
        }
    }
}

void EngineBridge::stopAllInputMeters()
{
    requestedMeters_.clear();
    if (inputMeterPump_)
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

QImage EngineBridge::latestPreviewFrame(const QString &deviceId)
{
    if (!bps::platform::PlatformAccessor::Installed())
        return {};
    const std::string id = deviceId.toStdString();
    const std::vector<uint8_t> jpeg =
        bps::platform::PlatformAccessor::Get().Video().PreviewFrame(id);
    if (jpeg.empty())
        return {};
    // DECODE-ONCE CACHE — the hot path that killed the UI: requestImage
    // runs on the GUI thread (synchronous QQuickImageProvider), and several
    // consumers (board thumb + dialog pane) poll the same device at 8-15 Hz
    // each. Decoding the JPEG per request meant 3-15 JPEG decodes/frame —
    // the render loop starved and clicks were dropped. Decode only when the
    // tap's bytes actually CHANGED (size + FNV-1a — size alone froze the
    // pane whenever consecutive frames compressed to the same length), and
    // hand every consumer a cheap COW copy.
    const QByteArray raw(reinterpret_cast<const char *>(jpeg.data()), int(jpeg.size()));
    const QString key = id.c_str();
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
    const QString devId = owner_->previewIds_.value(label);
    if (devId.isEmpty()) {
        // One warning per label per session — a missing tap is a wiring
        // bug (start never ran / a different label spelling), not a warm-up.
        if (!owner_->previewNoTapWarned_.contains(label)) {
            owner_->previewNoTapWarned_.insert(label);
            qWarning("EngineBridge: preview requested for '%s' but no tap was started",
                     qUtf8Printable(label));
        }
    }
    QImage frame = owner_->latestPreviewFrame(devId.isEmpty() ? id : devId);
    // One-shot per label: did the provider actually SERVE a frame through
    // the label→id translation? (Decoded-frames-in-cache but a dark pane
    // means this never fired with the pane's label.)
    if (!frame.isNull() && !owner_->previewServedLogged_.contains(label)) {
        owner_->previewServedLogged_.insert(label);
        qInfo("EngineBridge: preview frame SERVED for label '%s'", qUtf8Printable(label));
    }
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

void EngineBridge::startVideoPreview(const QString &deviceLabel, const QString &mode,
                                     const QString &owner)
{
    if (!bps::platform::PlatformAccessor::Installed())
        return;
    auto &video = bps::platform::PlatformAccessor::Get().Video();

    // Resolve the roster label → device id from the CACHED roster
    // (videoDevices_, refreshed at boot/hot-plug/dialog-open) — calling MF
    // Enumerate() here was the second UI freeze: it activates every camera
    // synchronously, hundreds of ms each, and this runs on every dialog
    // open, mode re-pick, and board row construction. A label the cache
    // doesn't know (first hot-plug race) falls back to one live enumeration.
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
        for (const auto &d : video.Enumerate()) {
            if (qstr(d.name) == want) {
                id = qstr(d.id);
                break;
            }
        }
    }
    if (id.isEmpty()) {
        qWarning("EngineBridge: no camera named '%s' to preview", qUtf8Printable(want));
        return;
    }

    // NDI virtual cameras crash their own driver DLL in-process when opened
    // as MF capture devices — this tap must never touch them. The bridge
    // refuses on the LABEL (reliable) after the PAL's id-derived name
    // lookup proved unable to catch these devices. NDI sources belong to
    // the dedicated NDI pipeline; the pane keeps its glyph until then.
    if (want.contains("ndi", Qt::CaseInsensitive)) {
        qWarning("EngineBridge: refusing camera preview tap for NDI virtual device '%s'",
                 qUtf8Printable(want));
        return;
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
        return;
    (void)video.StopPreview(id.toStdString());
    if (!video.StartPreview(id.toStdString(), effectiveMode.toStdString()).ok())
        qWarning("EngineBridge: camera preview tap failed for '%s'", qUtf8Printable(want));
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
            roster << v.toMap().value("label").toString();
        qInfo("EngineBridge[diag] startScreenPreview want='%s' owner=%s roster=%s",
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
    // STALE LABEL SELF-HEAL (the sibling of the dead-id heal below): the
    // boot-time roster may not hold the label AT ALL — a window opened or
    // retitled after the last enumerateDevices (the output-preview take
    // passes the row's stored sublabel straight here, possibly hours after
    // boot). Resolve fresh before refusing: windows re-enumerate live,
    // monitors are stable but cheap to re-ask.
    if (id.isEmpty()) {
        auto &platform = bps::platform::PlatformAccessor::Get();
        for (const auto &w : platform.Video().EnumerateWindows()) {
            if (qstr(w.title).trimmed().compare(want, Qt::CaseInsensitive) == 0) {
                id = qstr(w.id);
                break;
            }
        }
        if (id.isEmpty()) {
            for (const auto &m : platform.Monitor().Enumerate()) {
                const QString name = qstr(m.name);
                if (name == want || (name.isEmpty() && qstr(m.id) == want)) {
                    id = qstr(m.id);
                    break;
                }
            }
        }
        if (id.isEmpty()) {
            // DIAG: with VGR_PREVIEW_DIAG=1 a miss prints the titles live
            // enumeration DID see — the difference is the diagnosis (title
            // drift vs an OS filter hiding the window).
            if (diag) {
                QStringList live;
                for (const auto &w : platform.Video().EnumerateWindows())
                    live << qstr(w.title);
                qInfo("EngineBridge[diag] label miss: live windows=%s",
                      qUtf8Printable(live.join(" | ")));
            }
            qWarning("EngineBridge: no display named '%s' to preview", qUtf8Printable(want));
            return false;
        }
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
    // CACHED-DEAD-ID PRE-CHECK: a cached "win:<hwnd>" can be a DEAD window —
    // the user closes the app and reopens it, enumeration re-runs under the
    // SAME title with a NEW hwnd, but previewIds_ still serves the old id and
    // the tap below would fail and unwind the whole take. Validate a cached
    // window id by title right now: if the live window with this title has a
    // different hwnd, retarget the tap to the fresh id before starting.
    if (id.startsWith(QStringLiteral("win:"))) {
        QString fresh = id;
        for (const auto &w : bps::platform::PlatformAccessor::Get().Video().EnumerateWindows()) {
            if (qstr(w.title).trimmed().compare(want, Qt::CaseInsensitive) == 0) {
                fresh = qstr(w.id);
                break;
            }
        }
        if (fresh != id) {
            if (diag)
                qInfo("EngineBridge[diag] cached id '%s' stale -> live '%s'",
                      qUtf8Printable(id), qUtf8Printable(fresh));
            id = fresh;
        }
    }
    const bool ok = startTap(id);
    // STALE WINDOW IDS SELF-HEAL HERE: a "win:<hwnd>" id captured at
    // enumeration time dies with the window; the app re-opening re-enumerates
    // under the SAME title with a NEW hwnd. Monitor ids ("\\.\DISPLAY1") are
    // stable, so this only ever bites window rows — and without the retry the
    // failed id would stay cached in previewIds_ and POISON the label: every
    // later pick of that window title silently reused the dead hwnd and the
    // pane stayed dark forever (the "shows in the dropdown, never previews"
    // report). One fresh enumeration + one title match retries the tap.
    QString usedId = id;
    if (!ok && id.startsWith(QStringLiteral("win:"))) {
        for (const auto &w : bps::platform::PlatformAccessor::Get().Video().EnumerateWindows()) {
            if (qstr(w.title).trimmed().compare(want, Qt::CaseInsensitive) == 0) {
                usedId = qstr(w.id);
                break;
            }
        }
        if (usedId != id && startTap(usedId))
            qInfo("EngineBridge: screen tap '%s' re-resolved to fresh id '%s'",
                  qUtf8Printable(want), qUtf8Printable(usedId));
        else
            usedId = id;   // still dead — roll back below so a later pick retries
    }
    if (ok || usedId != id) {
        previewIds_[want] = usedId;
        qInfo("EngineBridge: screen tap '%s' -> id '%s' started", qUtf8Printable(want),
              qUtf8Printable(usedId));
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
                text = QStringLiteral("Memory use is %1 â close other apps if playback stutters.").arg(level.toLower());
            } else if (e.resource == "cpu") {
                title = QStringLiteral("High CPU usage");
                text = QStringLiteral("The CPU has stayed at %1 load for a while â heavy work in other apps may slow the presentation.").arg(level.toLower());
            } else if (e.resource == "disk") {
                title = QStringLiteral("Low disk space");
                text = QStringLiteral("Disk usage is %1.").arg(level.toLower());
            }
            relay("engine.resource.pressure_high", QStringLiteral("warning"), title, text);
        }));

    // ---- Content / project notices (formerly reached the UI only as formatted engine
    // log lines â "Engine Â· Core [Warning]..." â via the Notification Service's console
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
