#pragma once

// The UI's live view of the engine's Telemetry module — the three utilization
// numbers on Settings → General's "Resource profile" card (rendering /
// encoding / output, 0-100). The engine computes everything; this singleton
// republishes it as a QVariantMap, so QML binds stay simple
// (`TelemetryService.utilization.rendering`) and the meters move only when the
// numbers actually change (no timer-driven re-layout churn).
//
// It also carries `health` — the machine's own live health (CPU %, GPU %,
// memory %, package temperature and a 0-100 THROTTLE figure) for the resource
// health modal and Smart Config's live budgets. Those numbers come from the
// PAL sample on the same cadence, so they update at that cadence rather than
// the adaptive runtime's slower heartbeat. A value the platform cannot measure
// is reported as -1 ("not measured") rather than a fake 0.
//
// THREADING — the reason this type is shaped the way it is. `Sample()` asks the
// operating system (system times, memory, disk free space, the network
// interface table, the power status), and the adaptive runtime's own snapshot is
// a walk of its budget/feature tables. Both are cheap in the usual case and can
// still take many milliseconds when the machine is under load — which is exactly
// the state a service is presented in, where a stalled interface costs the
// operator the picture on screen. So the READING happens on a sampler thread of
// its own, and the interface thread only ever PUBLISHES what it was handed:
// compare the maps, emit the change signals, and hand them to QML.
//
// The sampler thread touches the engine's own singletons (the PAL, Telemetry,
// the adaptive runtime) and never a QObject: the maps it builds are plain value
// types, posted to this object's thread when they are ready. That is the same
// contract the engine's adaptive heartbeat already works under — it samples the
// same PAL from a pool thread.
//
// A service that is never asked for stays uncreated (the interface only
// instantiates it when a settings screen or the health modal binds it), and
// shutdownSampler() is a no-op in that case — see its comment.

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "modules/settings/Telemetry.hpp"

class QQmlEngine;
class QJSEngine;

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

    // aboutToQuit, BEFORE EngineBridge::shutdown (main.cpp): stop the sampler and join it,
    // so no PAL or engine read can be in flight while those are torn down under it — the
    // same quit-crash class the search indexer's and the NDI worker's joins exist for.
    //
    // STATIC, and a no-op when nothing ever asked for this service: it is a QML singleton
    // the interface creates on first use (a settings screen, the health modal), and a quit
    // must not CREATE one — with its thread and its first sample — just to stop it.
    static void shutdownSampler();

    explicit TelemetryService(QObject *parent = nullptr);
    ~TelemetryService() override;

signals:
    void utilizationChanged();
    void healthChanged();
    void engineChanged();

private:
    // ONE PASS OF READING, done on the sampler thread: the PAL sample, the engine's
    // utilization, and — every kEnginePollEvery passes — the adaptive runtime's own view.
    // Plain value types throughout, so a sample can cross to the interface thread as it is.
    struct Sample {
        int pass = 0;                 // which pass this is (1, 2, …) — for the diagnostics
        QVariantMap utilization;
        QVariantMap health;
        bool tookEngine = false;
        QVariantMap engine;
    };

    Sample measure();
    // One PAL sample -> the health map (see the `health` property's comment).
    QVariantMap sampleHealth();
    // The adaptive runtime's snapshot -> the engine map (see `engine`).
    QVariantMap sampleEngine();
    // The interface thread's half: compare against what QML already has and emit only the
    // maps that changed (unchanged maps must not re-lay-out a card at the poll cadence).
    void publish(const Sample &s);
    // The sampler thread's body: read, hand over, sleep — until the stop flag is set.
    void samplerLoop();
    // Stop + join. Safe to call twice (a service that was never created, or a quit after an
    // explicit shutdown).
    void stopSampler();

    std::thread sampler_;
    std::atomic<bool> stop_{false};
    std::mutex wakeMutex_;              // guards nothing but the wait below
    std::condition_variable wake_;      // so a quit does not wait out the sampling interval

    QVariantMap utilization_;
    QVariantMap health_;
    QVariantMap engine_;
    // Interface-thread only: the two one-shot diagnostics publish() writes — the thread ids
    // that say where the sampling happens, and the first pass with something measured on it.
    bool threadingLogged_ = false;
    bool readingsLogged_ = false;

    // ---- sampler-thread state, touched by no other thread ----
    // The adaptive runtime's own snapshot is heavier than the PAL sample (it copies the
    // budget/feature tables), so it is read every kEnginePollEvery passes rather than every
    // one.
    static constexpr int kEnginePollEvery = 4;
    int pollCount_ = 0;
    int pass_ = 0;
    // The PAL's cumulative jiffies at the previous pass — CPU% is the delta between them.
    quint64 prevCpuTotal_ = 0;
    quint64 prevCpuIdle_ = 0;
};
