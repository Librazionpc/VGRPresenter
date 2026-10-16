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

// The service the QML engine made, if anything ever asked for one. Set by instance() (the
// interface thread) and read by shutdownSampler() (the interface thread, at quit) — so
// main.cpp's quit path can stop a sampler that is running without CREATING a service, its
// thread and its first PAL sample in a session that never opened a settings screen.
TelemetryService *g_live = nullptr;

// The sampler thread's own id, hashed. Written once by that thread, read by the interface
// thread for the ONE diagnostic below — the line that makes the split of work visible in a
// log rather than only true in the code.
std::atomic<size_t> g_samplerThread{0};

// THE HEALTH MAP WITH NOTHING MEASURED — the shape `health` always has. sampleHealth()
// fills this in, and the constructor publishes it before the first sample lands, so a dial
// that binds health the moment its screen opens reads -1 ("not measured", the documented
// value for a reading the platform has not given) instead of QML's undefined, which a
// numeric property cannot even take. That last part is not theory: the meters on Settings ·
// General's resource card are int-typed, and an empty map in the first moments produced
// exactly that warning.
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
    // 2Hz, and the reading half of it runs OFF this thread (see the header): the PAL sample
    // and the engine reads can each cost milliseconds on a machine that is working hard,
    // and this object's thread is the one drawing the service. Starts unconditionally —
    // every read is safe before boot too (they read zeros until the Kernel wires the module
    // and frames flow).
    //
    // The SHAPES are published here, before the sampler's first pass: QML binds these maps
    // the moment a screen opens, and "not measured yet" has to read as the documented -1
    // rather than as a missing key. No signals — nothing has connected to this object while
    // it is still being constructed.
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
        // Bounded in practice by the sample in flight: each read is one OS call or one
        // engine snapshot, never a wait. Joining is what guarantees no read is still
        // running when the engine begins tearing down.
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
        // Read OUTSIDE any lock ("guard the publish, never the work"), then hand the maps
        // to the interface thread QUEUED — the maps are what QML binds to, and publishing
        // them is the interface thread's business. The same worker-to-GUI bookend the
        // chapter and indexer workers use.
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
        // ONE line per session, and it is the whole claim in one place: the two ids must
        // differ — the PAL sample is read on one thread and published on the thread that
        // draws (see the header). It also dates the change for anyone reading a log from a
        // machine where a settings screen felt slow to open.
        threadingLogged_ = true;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Telemetry"),
                            QStringLiteral("samples are READ on thread 0x%1 and PUBLISHED on the interface "
                                           "thread 0x%2 — a slow platform sample cannot stall the interface")
                                .arg(g_samplerThread.load(std::memory_order_relaxed), 0, 16)
                                .arg(std::hash<std::thread::id>{}(std::this_thread::get_id()), 0, 16));
    }
    if (!readingsLogged_ && s.pass >= 2) {
        // The second pass is the first with something measured to show: CPU% is a DELTA
        // between two PAL samples, so the first pass can only be the baseline (it reads -1)
        // and logging it would look like a fault. This is the other half of the claim —
        // real readings still reach the maps the interface binds.
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
}

// The adaptive runtime's own view: which mode/layer/quality it settled on, how
// many workers it keeps, and the caps + memory budget it is enforcing. This is
// the "good management" half — the health map says how the machine is, this
// says what the engine chose to do about it.
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

// The machine's own live health, from one PAL sample plus the adaptive
// runtime's mode (a battery / on-air mode throttles background work even when
// the part is not hot). CPU% is the jiffies delta since the last poll.
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
    // Plugged in AND topping up (platform::Snapshot::batteryCharging). Smart
    // Config's battery dial caption reads it: "Battery · charging" is a
    // different state from plain "Battery" (on AC, holding), and the dial the
    // user watches while the app runs on a laptop has to say which one it is.
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

    // How hard the machine is being held back. Two honest signals: the CPU
    // running below its nominal clock (% Processor Performance < 100), and a
    // package temperature at/over the thermal manager's 85C line.
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

    // READ UNCONDITIONALLY, deliberately: this runs on the sampler thread, which touches the
    // engine's own singletons and no QObject — so the `booted()` guard the interface thread
    // used (an EngineBridge call, and a QObject) is gone. Before the runtime has probed the
    // hardware the name is simply empty, which is what the surfaces that read it already
    // treat as "no name to show" (ResourceHealthModal, Smart Config's GPU dial).
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
