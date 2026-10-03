#pragma once

// The UI's live view of the engine's Telemetry module — the three utilization
// numbers on Settings → General's "Resource profile" card (rendering /
// encoding / output, 0-100). The engine computes everything; this singleton
// polls Snapshot() on a timer and republishes as a QVariantMap, so QML binds
// stay simple (`TelemetryService.utilization.rendering`) and the meters move
// only when the numbers actually change (no timer-driven re-layout churn).
//
// It also carries `health` — the machine's own live health (CPU %, GPU %,
// memory %, package temperature and a 0-100 THROTTLE figure) for the resource
// health modal and Smart Config's live budgets. Those numbers come from the
// PAL sample on the same poll, so they update at the poll cadence rather than
// the adaptive runtime's slower heartbeat. A value the platform cannot measure
// is reported as -1 ("not measured") rather than a fake 0.

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "modules/settings/Telemetry.hpp"

class QQmlEngine;
class QJSEngine;
class QTimer;

class TelemetryService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // { rendering, encoding, output } — live engine utilization (percent 0-100).
    Q_PROPERTY(QVariantMap utilization READ utilization NOTIFY utilizationChanged)
    // { cpuPct, gpuPct, memUsedPct, tempC, gpuName, throttlePct, throttling, reason }
    // — the machine's own live health. cpuPct / gpuPct / memUsedPct / tempC are
    // -1 when the platform cannot measure them; throttlePct is 0 (free running)
    // to 100 (held right down), with `throttling` a convenience bool and
    // `reason` one of "None" | "Thermal" | "Power" | "Battery" | "Presentation".
    Q_PROPERTY(QVariantMap health READ health NOTIFY healthChanged)
    // { mode, layer, quality, workers, gpuCapPct, cpuCapPct, memoryBudgetMb, gpuName }
    // — what the engine's adaptive runtime is currently DOING with the machine
    // (its second slow-changing half is asked at 1/4 the poll cadence).
    Q_PROPERTY(QVariantMap engine READ engine NOTIFY engineChanged)

public:
    static TelemetryService &instance();
    static TelemetryService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    QVariantMap utilization() const { return utilization_; }
    QVariantMap health() const { return health_; }
    QVariantMap engine() const { return engine_; }

    explicit TelemetryService(QObject *parent = nullptr);
    ~TelemetryService() override;

signals:
    void utilizationChanged();
    void healthChanged();
    void engineChanged();

private:
    void poll();
    // One PAL sample -> the health map (see the `health` property's comment).
    QVariantMap sampleHealth();
    // The adaptive runtime's snapshot -> the engine map (see `engine`).
    QVariantMap sampleEngine();

    QTimer *timer_ = nullptr;
    QVariantMap utilization_;
    QVariantMap health_;
    QVariantMap engine_;
    // The adaptive runtime's own snapshot is heavier than the PAL sample (it
    // copies the budget/feature tables), so it is read every kEnginePollEvery
    // polls rather than every one.
    static constexpr int kEnginePollEvery = 4;
    int pollCount_ = 0;
    // The PAL's cumulative jiffies at the previous poll — CPU% is the delta.
    quint64 prevCpuTotal_ = 0;
    quint64 prevCpuIdle_ = 0;
};
