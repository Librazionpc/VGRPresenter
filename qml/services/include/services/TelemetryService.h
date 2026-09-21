#pragma once

// The UI's live view of the engine's Telemetry module — the three utilization
// numbers on Settings → General's "Resource profile" card (rendering /
// encoding / output, 0-100). The engine computes everything; this singleton
// polls Snapshot() on a timer and republishes as a QVariantMap, so QML binds
// stay simple (`TelemetryService.utilization.rendering`) and the meters move
// only when the numbers actually change (no timer-driven re-layout churn).

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

public:
    static TelemetryService &instance();
    static TelemetryService *create(QQmlEngine *engine, QJSEngine *jsEngine);

    QVariantMap utilization() const { return utilization_; }

    explicit TelemetryService(QObject *parent = nullptr);
    ~TelemetryService() override;

signals:
    void utilizationChanged();

private:
    void poll();

    QTimer *timer_ = nullptr;
    QVariantMap utilization_;
};
