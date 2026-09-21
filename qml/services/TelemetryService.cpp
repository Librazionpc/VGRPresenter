#include "services/TelemetryService.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QTimer>

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
}
