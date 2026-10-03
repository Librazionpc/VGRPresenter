#include "services/TelemetryService.h"

#include "modules/adaptive/AdaptiveRuntime.hpp"
#include "platform/PlatformAccessor.hpp"
#include "services/EngineBridge.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
// Meter refresh: 500ms — live-looking without burning frames on a card that
// usually reads 0/0/0 (the engine's numbers themselves update per frame).
constexpr int kPollMs = 500;
} // namespace

TelemetryService &TelemetryService::instance()
{
    static TelemetryService s;
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
    // Poll, not push: the engine has no UI-facing thread to signal from, and a
    // 2Hz poll of three ints is nothing. Starts unconditionally — the snapshot
    // is safe before boot too (it just reads zeros until the Kernel wires the
    // module and frames flow).
    timer_ = new QTimer(this);
    timer_->setInterval(kPollMs);
    connect(timer_, &QTimer::timeout, this, &TelemetryService::poll);
    timer_->start();
    poll();
}

TelemetryService::~TelemetryService() = default;

void TelemetryService::poll()
{
    const bps::settings::Utilization u = bps::settings::Telemetry::Instance().Snapshot();
    const QVariantMap next{
        {QStringLiteral("rendering"), u.rendering},
        {QStringLiteral("encoding"), u.encoding},
        {QStringLiteral("output"), u.output},
    };
    if (next != utilization_) {
        utilization_ = next;
        emit utilizationChanged();
    }

    if (QVariantMap health = sampleHealth(); health != health_) {
        health_ = std::move(health);
        emit healthChanged();
    }

    // What the engine is DOING with the machine. Slow-changing (and heavier to
    // read), so one poll in four is plenty.
    if (++pollCount_ % kEnginePollEvery == 0) {
        if (QVariantMap engine = sampleEngine(); engine != engine_) {
            engine_ = std::move(engine);
            emit engineChanged();
        }
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

    QString gpuName;
    if (EngineBridge::instance().booted())
        gpuName = QString::fromStdString(bps::adaptive::AdaptiveRuntime::Instance().Hardware().gpuName);

    return {
        { QStringLiteral("cpuPct"), static_cast<int>(std::lround(cpuPct)) },
        { QStringLiteral("gpuPct"), gpuPct < 0.0 ? -1 : static_cast<int>(std::lround(gpuPct)) },
        { QStringLiteral("memUsedPct"), memUsedPct < 0.0 ? -1 : static_cast<int>(std::lround(memUsedPct)) },
        { QStringLiteral("tempC"), tempC < 0.0 ? -1 : static_cast<int>(std::lround(tempC)) },
        { QStringLiteral("gpuName"), gpuName },
        { QStringLiteral("throttlePct"), static_cast<int>(std::lround(throttlePct)) },
        { QStringLiteral("throttling"), throttling },
        { QStringLiteral("reason"), reason },
        { QStringLiteral("batteryPct"), batteryPct < 0.0 ? -1 : static_cast<int>(std::lround(batteryPct)) },
        { QStringLiteral("onBattery"), onBattery },
    };
}
