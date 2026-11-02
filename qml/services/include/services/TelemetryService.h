#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
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
    Q_PROPERTY(QVariantMap health READ health NOTIFY healthChanged)
    Q_PROPERTY(QVariantMap engine READ engine NOTIFY engineChanged)
    Q_PROPERTY(QVariantMap live READ live NOTIFY liveChanged)
    Q_PROPERTY(QVariantList liveHistory READ liveHistory NOTIFY liveHistoryChanged)

public:
    static TelemetryService &instance();
    static TelemetryService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    QVariantMap utilization() const { return utilization_; }
    QVariantMap health() const { return health_; }
    QVariantMap engine() const { return engine_; }
    QVariantMap live() const { return live_; }
    QVariantList liveHistory() const { return liveHistory_; }

    static void shutdownSampler();

    explicit TelemetryService(QObject *parent = nullptr);
    ~TelemetryService() override;

signals:
    void utilizationChanged();
    void healthChanged();
    void engineChanged();
    void liveChanged();
    void liveHistoryChanged();

private:
    struct Sample {
        int pass = 0;                 // which pass this is (1, 2, …) — for the diagnostics
        QVariantMap utilization;
        QVariantMap health;
        bool tookEngine = false;
        QVariantMap engine;
        QVariantMap live;
        QVariantList liveHistory;
    };

    Sample measure();
    // One PAL sample -> the health map (see the `health` property's comment).
    QVariantMap sampleHealth();
    // The adaptive runtime's snapshot -> the engine map (see `engine`).
    QVariantMap sampleEngine();
    QVariantMap sampleLive();
    QVariantList sampleLiveHistory();
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
    QVariantMap live_;
    QVariantList liveHistory_;
    // Interface-thread only: the two one-shot diagnostics publish() writes — the thread ids
    // that say where the sampling happens, and the first pass with something measured on it.
    bool threadingLogged_ = false;
    bool readingsLogged_ = false;
    bool liveWasActive_ = false;
    double liveWorstFrameMs_ = 0.0;
    double liveMinComposedFps_ = 0.0;
    QString liveWorstElement_;
    double liveWorstSharePct_ = 0.0;
    bool liveCostLogged_ = false;

    // ---- sampler-thread state, touched by no other thread ----
    static constexpr int kEnginePollEvery = 4;
    int pollCount_ = 0;
    int pass_ = 0;
    // The PAL's cumulative jiffies at the previous pass — CPU% is the delta between them.
    quint64 prevCpuTotal_ = 0;
    quint64 prevCpuIdle_ = 0;
};