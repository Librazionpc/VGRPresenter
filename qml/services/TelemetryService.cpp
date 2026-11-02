#include "services/TelemetryService.h"

#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "platform/PlatformAccessor.hpp"
#include "services/EngineBridge.h"

#include <QJSEngine>
#include <QQmlEngine>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

namespace {
// Meter refresh: 500ms — live-looking without burning frames on a card that
// usually reads 0/0/0 (the engine's numbers themselves update per frame).
constexpr int kPollMs = 500;

TelemetryService *g_live = nullptr;

std::atomic<size_t> g_samplerThread{0};

QVariantMap unmeasuredHealth()
{
    return {
        { QStringLiteral("cpuPct"), -1 },
        { QStringLiteral("gpuPct"), -1 },
        { QStringLiteral("memUsedPct"), -1 },
        { QStringLiteral("tempC"), -1 },
        { QStringLiteral("gpuName"), QString() },
        { QStringLiteral("throttlePct"), 0 },
        { QStringLiteral("throttling"), false },
        { QStringLiteral("reason"), QStringLiteral("None") },
        { QStringLiteral("batteryPct"), -1 },
        { QStringLiteral("onBattery"), false },
        { QStringLiteral("charging"), false },
    };
}

// Utilization before the engine has reported any. It reports 0 until frames flow (the
// numbers are per-frame), so this is the engine's own answer rather than a stand-in.
QVariantMap zeroUtilization()
{
    return { { QStringLiteral("rendering"), 0 },
             { QStringLiteral("encoding"), 0 },
             { QStringLiteral("output"), 0 } };
}
} // namespace

TelemetryService &TelemetryService::instance()
{
    static TelemetryService s;
    g_live = &s;
    return s;
}

TelemetryService *TelemetryService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

TelemetryService::TelemetryService(QObject *parent)
    : QObject(parent)
{
    utilization_ = zeroUtilization();
    health_ = unmeasuredHealth();
    stop_.store(false, std::memory_order_relaxed);
    sampler_ = std::thread([this] { samplerLoop(); });
}

TelemetryService::~TelemetryService()
{
    // The thread must never outlive the object it reports to. main.cpp already stops it
    // before the engine teardown; this covers a service destroyed any other way.
    stopSampler();
}

// aboutToQuit, before EngineBridge::shutdown (see the header).
void TelemetryService::shutdownSampler()
{
    if (g_live)
        g_live->stopSampler();
}

void TelemetryService::stopSampler()
{
    stop_.store(true, std::memory_order_relaxed);
    wake_.notify_all();                  // do not wait out the sampling interval
    if (sampler_.joinable())
        sampler_.join();
}

void TelemetryService::samplerLoop()
{
    g_samplerThread.store(std::hash<std::thread::id>{}(std::this_thread::get_id()),
                          std::memory_order_relaxed);
    for (;;) {
        // A stop that arrived before this pass means no read at all — a quit that lands in
        // the first moments must not leave a PAL sample running into the teardown.
        if (stop_.load(std::memory_order_relaxed))
            return;
        Sample s = measure();
        s.pass = ++pass_;
        QMetaObject::invokeMethod(this, [this, s] { publish(s); }, Qt::QueuedConnection);
        std::unique_lock<std::mutex> lock(wakeMutex_);
        if (wake_.wait_for(lock, std::chrono::milliseconds(kPollMs),
                           [this] { return stop_.load(std::memory_order_relaxed); }))
            return;
    }
}

// One pass of reading — the whole of it off the interface thread.
TelemetryService::Sample TelemetryService::measure()
{
    Sample s;
    const bps::settings::Utilization u = bps::settings::Telemetry::Instance().Snapshot();
    s.utilization = {
        {QStringLiteral("rendering"), u.rendering},
        {QStringLiteral("encoding"), u.encoding},
        {QStringLiteral("output"), u.output},
    };
    s.health = sampleHealth();
    s.live = sampleLive();
    s.liveHistory = sampleLiveHistory();
    // What the engine is DOING with the machine. Slow-changing (and heavier to
    // read), so one pass in four is plenty.
    if (++pollCount_ % kEnginePollEvery == 0) {
        s.tookEngine = true;
        s.engine = sampleEngine();
    }
    return s;
}

// The interface thread's half: publish, and only what actually moved. An unchanged map
// raising its signal would re-lay-out a settings card twice a second for nothing.
void TelemetryService::publish(const Sample &s)
{
    if (!threadingLogged_) {
        threadingLogged_ = true;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Telemetry"),
                            QStringLiteral("samples are READ on thread 0x%1 and PUBLISHED on the interface "
                                           "thread 0x%2 — a slow platform sample cannot stall the interface")
                                .arg(g_samplerThread.load(std::memory_order_relaxed), 0, 16)
                                .arg(std::hash<std::thread::id>{}(std::this_thread::get_id()), 0, 16));
    }
    if (!readingsLogged_ && s.pass >= 2) {
        readingsLogged_ = true;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Telemetry"),
                            QStringLiteral("first measured pass: cpu=%1% gpu=%2% mem=%3% temp=%4C "
                                           "throttle=%5% (published to QML on the interface thread)")
                                .arg(s.health.value(QStringLiteral("cpuPct"), -1).toInt())
                                .arg(s.health.value(QStringLiteral("gpuPct"), -1).toInt())
                                .arg(s.health.value(QStringLiteral("memUsedPct"), -1).toInt())
                                .arg(s.health.value(QStringLiteral("tempC"), -1).toInt())
                                .arg(s.health.value(QStringLiteral("throttlePct"), -1).toInt()));
    }
    if (s.utilization != utilization_) {
        utilization_ = s.utilization;
        emit utilizationChanged();
    }
    if (s.health != health_) {
        health_ = s.health;
        emit healthChanged();
    }
    // A pass that did not read the engine half leaves the last one standing (see
    // kEnginePollEvery): its numbers are the runtime's mode/budgets, not a per-frame figure.
    if (s.tookEngine && s.engine != engine_) {
        engine_ = s.engine;
        emit engineChanged();
    }
    if (s.live != live_) {
        live_ = s.live;
        emit liveChanged();
    }
    if (s.liveHistory != liveHistory_) {
        liveHistory_ = s.liveHistory;
        emit liveHistoryChanged();
    }
    if (!s.live.isEmpty()) {
        const double frameMs = s.live.value(QStringLiteral("frameMs")).toDouble();
        const double fps = s.live.value(QStringLiteral("composedFps")).toDouble();
        if (!liveCostLogged_) {
            const QString element = s.live.value(QStringLiteral("costElement")).toString();
            if (!element.isEmpty()) {
                liveCostLogged_ = true;
                EngineBridge::write(
                    QStringLiteral("info"), QStringLiteral("Telemetry"),
                    QStringLiteral("live hotspot: %1 — %2% of the composed frame (≈%3 ms of it; the "
                                   "scene's own background is %4% of that frame) — what the "
                                   "slowing-down curve points at")
                        .arg(element)
                        .arg(s.live.value(QStringLiteral("costSharePct")).toDouble(), 0, 'f', 0)
                        .arg(s.live.value(QStringLiteral("costMs")).toDouble(), 0, 'f', 1)
                        .arg(s.live.value(QStringLiteral("costBackgroundPct")).toDouble(), 0, 'f', 0));
            }
        }
        if (frameMs > liveWorstFrameMs_) {
            liveWorstFrameMs_ = frameMs;
            liveWorstElement_ = s.live.value(QStringLiteral("costElement")).toString();
            liveWorstSharePct_ = s.live.value(QStringLiteral("costSharePct")).toDouble();
        }
        if (fps > 0.0 && (liveMinComposedFps_ == 0.0 || fps < liveMinComposedFps_))
            liveMinComposedFps_ = fps;
        liveWasActive_ = true;
    } else if (liveWasActive_) {
        liveWasActive_ = false;
        liveCostLogged_ = false;   // the next go-live names its own hotspot
        const double worst = liveWorstFrameMs_;
        const double slowest = liveMinComposedFps_;
        const QString element = liveWorstElement_;
        const double elementShare = liveWorstSharePct_;
        liveWorstFrameMs_ = 0.0;
        liveMinComposedFps_ = 0.0;
        liveWorstElement_.clear();
        liveWorstSharePct_ = 0.0;
        QString blame;
        if (!element.isEmpty())
            blame = QStringLiteral(" — worst element %1 (%2% of that frame)")
                        .arg(element)
                        .arg(elementShare, 0, 'f', 0);
        EngineBridge::write(
            QStringLiteral("info"), QStringLiteral("Telemetry"),
            QStringLiteral("live session ended: slowest raster %1 ms, lowest composed rate %2 fps%3 "
                           "(the live curve in Telemetry.liveHistory; a rate that falls over a "
                           "session is a slowdown, not a busy engine)")
                .arg(worst, 0, 'f', 1)
                .arg(slowest, 0, 'f', 2)
                .arg(blame));
    }
}

QVariantMap TelemetryService::sampleLive()
{
    const bps::settings::LiveStats s = bps::settings::Telemetry::Instance().LiveSnapshot();
    return {
        { QStringLiteral("composedFps"), s.composedFps },
        { QStringLiteral("presentFps"), s.presentFps },
        { QStringLiteral("frameMs"), s.frameMs },
        { QStringLiteral("representMs"), s.representMs },
        { QStringLiteral("sharedCopies"), static_cast<qlonglong>(s.sharedCopies) },
        { QStringLiteral("composed"), static_cast<qlonglong>(s.composed) },
        { QStringLiteral("represented"), static_cast<qlonglong>(s.represented) },
        { QStringLiteral("costElement"), QString::fromStdString(s.costElement) },
        { QStringLiteral("costLayer"), QString::fromStdString(s.costLayer) },
        { QStringLiteral("costObject"), QString::fromStdString(s.costObject) },
        { QStringLiteral("costSharePct"), s.costSharePct },
        { QStringLiteral("costMs"), s.costMs },
        { QStringLiteral("costBackgroundPct"), s.costBackgroundPct },
    };
}

QVariantList TelemetryService::sampleLiveHistory()
{
    QVariantList out;
    const std::vector<bps::settings::LiveSample> history =
        bps::settings::Telemetry::Instance().LiveHistory();
    out.reserve(static_cast<int>(history.size()));
    for (const auto& sample : history) {
        out.append(QVariantMap{
            { QStringLiteral("composedFps"), sample.composedFps },
            { QStringLiteral("presentFps"), sample.presentFps },
            { QStringLiteral("frameMs"), sample.frameMs },
            { QStringLiteral("representMs"), sample.representMs },
            { QStringLiteral("sharedCopies"), static_cast<qlonglong>(sample.sharedCopies) },
            { QStringLiteral("costElement"), QString::fromStdString(sample.costElement) },
            { QStringLiteral("costLayer"), QString::fromStdString(sample.costLayer) },
            { QStringLiteral("costObject"), QString::fromStdString(sample.costObject) },
            { QStringLiteral("costSharePct"), sample.costSharePct },
            { QStringLiteral("costMs"), sample.costMs },
            { QStringLiteral("costBackgroundPct"), sample.costBackgroundPct },
        });
    }
    return out;
}

QVariantMap TelemetryService::sampleEngine()
{
    const bps::adaptive::RuntimeSnapshot s = bps::adaptive::AdaptiveRuntime::Instance().Snapshot();
    return {
        { QStringLiteral("mode"), QString::fromStdString(s.mode) },
        { QStringLiteral("layer"), QString::fromStdString(s.layer) },
        { QStringLiteral("quality"), QString::fromStdString(s.quality) },
        { QStringLiteral("workers"), static_cast<int>(s.workerThreads) },
        { QStringLiteral("gpuCapPct"), static_cast<int>(s.gpuCapPct) },
        { QStringLiteral("cpuCapPct"), static_cast<int>(s.cpuCapPct) },
        { QStringLiteral("memoryBudgetMb"), static_cast<qlonglong>(s.memoryBudgetBytes / (1024ull * 1024ull)) },
        { QStringLiteral("gpuName"), QString::fromStdString(s.gpuName) },
    };
}

QVariantMap TelemetryService::sampleHealth()
{
    double cpuPct = -1.0;
    double gpuPct = -1.0;
    double memUsedPct = -1.0;
    double tempC = -1.0;
    double perfPct = -1.0;
    // Battery: -1 = no battery / not reported. Surfaced for the Smart Config
    // resource dials, which show charge as its own gauge.
    double batteryPct = -1.0;
    bool onBattery = false;
    bool charging = false;

    if (bps::platform::PlatformAccessor::Installed()) {
        const bps::platform::Snapshot s = bps::platform::PlatformAccessor::Get().Sample();
        if (s.cpuTotalJiffies > 0) {
            // The FIRST poll has no previous sample, so it only records the
            // baseline; every poll after it has a real delta.
            const quint64 dTotal = s.cpuTotalJiffies - prevCpuTotal_;
            const quint64 dIdle = s.cpuIdleJiffies - prevCpuIdle_;
            if (prevCpuTotal_ > 0 && s.cpuTotalJiffies > prevCpuTotal_ && dTotal > 0)
                cpuPct = std::clamp(100.0 * (1.0 - static_cast<double>(dIdle) / static_cast<double>(dTotal)),
                                    0.0, 100.0);
            prevCpuTotal_ = s.cpuTotalJiffies;
            prevCpuIdle_ = s.cpuIdleJiffies;
        }
        gpuPct = s.gpuPct;
        tempC = s.cpuTempC;
        perfPct = s.cpuPerfPct;
        if (s.batteryPercent >= 0)
            batteryPct = s.batteryPercent;
        onBattery = s.onBattery;
        charging = s.batteryCharging;
        if (s.totalRamBytes > 0)
            memUsedPct = std::clamp(100.0 * static_cast<double>(s.totalRamBytes - s.availableRamBytes)
                                        / static_cast<double>(s.totalRamBytes),
                                    0.0, 100.0);
    }

    double throttlePct = 0.0;
    bool thermal = false;
    if (perfPct >= 0.0) {
        const double under = std::clamp(100.0 - perfPct, 0.0, 100.0);
        throttlePct = std::max(throttlePct, under);
        thermal = thermal || under >= 10.0;
    }
    if (tempC >= 85.0) {
        throttlePct = std::max(throttlePct, std::clamp((tempC - 85.0) / 15.0 * 100.0, 0.0, 100.0));
        thermal = true;
    }

    // Battery / on-air modes throttle background work without the part being hot.
    QString reason = QStringLiteral("None");
    bool throttling = throttlePct >= 5.0;
    if (thermal) {
        reason = QStringLiteral("Thermal");
    } else if (bps::adaptive::AdaptiveRuntime::Instance().GetUserMode() == bps::adaptive::UserMode::Presentation) {
        reason = QStringLiteral("Presentation");
        throttling = true;
    } else if (bps::adaptive::AdaptiveRuntime::Instance().GetUserMode() == bps::adaptive::UserMode::Battery) {
        reason = QStringLiteral("Battery");
        throttling = true;
    }

    const QString gpuName =
        QString::fromStdString(bps::adaptive::AdaptiveRuntime::Instance().Hardware().gpuName);

    // Filled into the unmeasured shape, so this map has the same keys whichever readings
    // the platform could give (and the constructor's `health` and this one cannot drift).
    QVariantMap out = unmeasuredHealth();
    out[QStringLiteral("cpuPct")] = static_cast<int>(std::lround(cpuPct));
    out[QStringLiteral("gpuPct")] = gpuPct < 0.0 ? -1 : static_cast<int>(std::lround(gpuPct));
    out[QStringLiteral("memUsedPct")] = memUsedPct < 0.0 ? -1 : static_cast<int>(std::lround(memUsedPct));
    out[QStringLiteral("tempC")] = tempC < 0.0 ? -1 : static_cast<int>(std::lround(tempC));
    out[QStringLiteral("gpuName")] = gpuName;
    out[QStringLiteral("throttlePct")] = static_cast<int>(std::lround(throttlePct));
    out[QStringLiteral("throttling")] = throttling;
    out[QStringLiteral("reason")] = reason;
    out[QStringLiteral("batteryPct")] = batteryPct < 0.0 ? -1 : static_cast<int>(std::lround(batteryPct));
    out[QStringLiteral("onBattery")] = onBattery;
    out[QStringLiteral("charging")] = charging;
    return out;
}