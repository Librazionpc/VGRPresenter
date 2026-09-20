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
#include <QDir>
#include <QJSEngine>
#include <QQmlEngine>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

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

    void Execute() override
    {
        if (do_.isCallable())
            do_.call();
    }

    void Undo() override
    {
        if (undo_.isCallable())
            undo_.call();
    }

    std::string Label() const override { return label_.toStdString(); }

private:
    QString label_;
    QJSValue do_;
    QJSValue undo_;
};

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
    (void)bps::Logger::Instance().AddSink(
        std::make_shared<bps::FileSink>((logDir + QStringLiteral("/engine.log")).toStdString()));

    bps::BootOptions options;
    options.dataDir = dataDir.toStdString();
    options.logLevel = bps::LogLevel::Info;
    // pluginDirs left empty, ipcPort left 0 (its BootOptions default) —
    // no plugin loading, no network listener opened.

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
            .arg(audioDevices_.size()).arg(audioNames.join(QStringLiteral(", ")).toHtmlEscaped())
            .arg(screenDevices_.size()).arg(screenNames.join(QStringLiteral(", ")).toHtmlEscaped());
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
    explicit EngineLogSink(EngineBridge *bridge) : bridge_(bridge) {}

    const char *Name() const noexcept override { return "UiRelay"; }

    void Write(std::string_view formatted, const bps::LogRecord &record) override
    {
        Q_UNUSED(formatted)
        if (!bridge_)
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
        if (booted()) {
            auto &broadcast = bps::broadcast::BroadcastEngine::Instance();
            ndiAvailable_ = broadcast.NdiAvailable();
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
                    if (ndiSources_.isEmpty())
                        QTimer::singleShot(1200, this, [this]() { if (booted()) enumerateDevices(); });
                } else {
                    ndiAvailable_ = false;
                    ndiStatus_ = QString::fromStdString(sources.error().message);
                }
            } else {
                ndiStatus_ = QStringLiteral("NDI SDK not available on this machine");
            }
        }
    }
    emit devicesChanged();
}

void EngineBridge::refreshDevices() { enumerateDevices(); }

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
            relay("engine.resource.pressure_high", QStringLiteral("warning"),
                  QStringLiteral("Resource pressure"), QStringLiteral("%1 running high").arg(qstr(e.resource)));
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
