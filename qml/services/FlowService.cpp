#include "services/FlowService.h"

#include "services/EventBus.h"
#include "services/LiveOutputService.h"
#include "services/RecordingService.h"

#include "modules/automation/FlowEngine.hpp"
#include "modules/automation/AutomationPlugin.hpp"
#include "modules/automation/AutomationTypes.hpp"

#include <QTimer>

namespace fa = bps::automation;
using bps::Result;   // the engine's Result<T> (IAction::Execute's return type)

namespace {

void toast(const QString &message, const QString &level = QStringLiteral("error"))
{
    EventBus::instance().notify(message, level, QStringLiteral("Functions"),
                                QStringLiteral("flow.transport"));
}

QString flowStateName(fa::FlowState s)
{
    switch (s) {
    case fa::FlowState::Idle: return QStringLiteral("idle");
    case fa::FlowState::Validating: return QStringLiteral("validating");
    case fa::FlowState::Running: return QStringLiteral("running");
    case fa::FlowState::Paused: return QStringLiteral("paused");
    case fa::FlowState::Stopped: return QStringLiteral("stopped");
    case fa::FlowState::EmergencyStopped: return QStringLiteral("emergency");
    case fa::FlowState::Completed: return QStringLiteral("completed");
    }
    return QStringLiteral("idle");
}

QString nodeStateName(fa::NodeState s)
{
    switch (s) {
    case fa::NodeState::Pending: return QStringLiteral("pending");
    case fa::NodeState::Running: return QStringLiteral("running");
    case fa::NodeState::Completed: return QStringLiteral("completed");
    case fa::NodeState::Failed: return QStringLiteral("failed");
    case fa::NodeState::Skipped: return QStringLiteral("skipped");
    case fa::NodeState::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("pending");
}

QString nodeKindName(fa::NodeKind k)
{
    switch (k) {
    case fa::NodeKind::Action: return QStringLiteral("action");
    case fa::NodeKind::Condition: return QStringLiteral("condition");
    case fa::NodeKind::Parallel: return QStringLiteral("parallel");
    case fa::NodeKind::Delay: return QStringLiteral("delay");
    case fa::NodeKind::Wait: return QStringLiteral("wait");
    case fa::NodeKind::Macro: return QStringLiteral("macro");
    case fa::NodeKind::End: return QStringLiteral("end");
    }
    return QStringLiteral("action");
}

// The starter flow (the engine's .vgr native format — the same shape the
// docs' Sunday-Service demo uses, with REAL action types this app registers).
constexpr const char *kStarterFlow =
    "{\"type\":\"flow\",\"flow\":{"
    "\"id\":\"starter-sunday\",\"name\":\"Starter Sunday Service\","
    "\"variables\":{\"SERVICE_NAME\":\"Morning Service\"},"
    "\"nodes\":["
    "{\"id\":\"live\",\"label\":\"Go live\",\"actionType\":\"go_live\"},"
    "{\"id\":\"countdown\",\"label\":\"Countdown (2 min)\",\"kind\":\"delay\","
    "\"actionPayload\":\"120000\"},"
    "{\"id\":\"welcome\",\"label\":\"Welcome on screen\",\"actionType\":\"log\","
    "\"actionPayload\":\"Welcome to {SERVICE_NAME}\"},"
    "{\"id\":\"end\",\"label\":\"End\",\"kind\":\"end\"}"
    "]}}";

// The app's domain actions — real commands against the live services,
// registered into the engine's action registry (its plugin pattern: the
// engine core never learns about the presentation layer).
void installDomainActions()
{
    auto &flow = fa::FlowEngine::Instance();

    (void)flow.RegisterAction("go_live", []() -> std::shared_ptr<fa::IAction> {
        struct A final : fa::IAction {
            const char *Type() const noexcept override { return "go_live"; }
            Result<fa::ActionOutcome> Execute(fa::AutomationContext &) override {
                LiveOutputService::instance().goLive();
                return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
            }
        };
        return std::make_shared<A>();
    });
    (void)flow.RegisterAction("stop_live", []() -> std::shared_ptr<fa::IAction> {
        struct A final : fa::IAction {
            const char *Type() const noexcept override { return "stop_live"; }
            Result<fa::ActionOutcome> Execute(fa::AutomationContext &) override {
                LiveOutputService::instance().stop();
                return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
            }
        };
        return std::make_shared<A>();
    });
    (void)flow.RegisterAction("next_slide", []() -> std::shared_ptr<fa::IAction> {
        struct A final : fa::IAction {
            const char *Type() const noexcept override { return "next_slide"; }
            Result<fa::ActionOutcome> Execute(fa::AutomationContext &) override {
                LiveOutputService::instance().next();
                return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
            }
        };
        return std::make_shared<A>();
    });
    (void)flow.RegisterAction("previous_slide", []() -> std::shared_ptr<fa::IAction> {
        struct A final : fa::IAction {
            const char *Type() const noexcept override { return "previous_slide"; }
            Result<fa::ActionOutcome> Execute(fa::AutomationContext &) override {
                LiveOutputService::instance().previous();
                return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
            }
        };
        return std::make_shared<A>();
    });
    (void)flow.RegisterAction("clear_air", []() -> std::shared_ptr<fa::IAction> {
        struct A final : fa::IAction {
            const char *Type() const noexcept override { return "clear_air"; }
            Result<fa::ActionOutcome> Execute(fa::AutomationContext &) override {
                // The media layer and a taken input are the two raster holds;
                // slides stay (stopping live is stop_live's job).
                LiveOutputService::instance().clearMedia();
                LiveOutputService::instance().clearInput();
                return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
            }
        };
        return std::make_shared<A>();
    });
    (void)flow.RegisterAction("start_recording", []() -> std::shared_ptr<fa::IAction> {
        struct A final : fa::IAction {
            const char *Type() const noexcept override { return "start_recording"; }
            Result<fa::ActionOutcome> Execute(fa::AutomationContext &) override {
                RecordingService::instance().startRecording(RecordingService::instance().config());
                return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
            }
        };
        return std::make_shared<A>();
    });
    (void)flow.RegisterAction("stop_recording", []() -> std::shared_ptr<fa::IAction> {
        struct A final : fa::IAction {
            const char *Type() const noexcept override { return "stop_recording"; }
            Result<fa::ActionOutcome> Execute(fa::AutomationContext &) override {
                RecordingService::instance().stopRecording();
                return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
            }
        };
        return std::make_shared<A>();
    });
    (void)flow.RegisterAction("mark_recording", []() -> std::shared_ptr<fa::IAction> {
        struct A final : fa::IAction {
            const char *Type() const noexcept override { return "mark_recording"; }
            Result<fa::ActionOutcome> Execute(fa::AutomationContext &ctx) override {
                RecordingService::instance().addMarker(
                    QString::fromStdString(ctx.Substitute(ctx.node ? ctx.node->actionPayload
                                                                   : std::string_view{})));
                return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
            }
        };
        return std::make_shared<A>();
    });
}

} // namespace

FlowService *FlowService::s_instance = nullptr;

FlowService::FlowService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
}

FlowService &FlowService::instance()
{
    static FlowService inst;
    return inst;
}

FlowService *FlowService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    // The engine boots after the kernel; the pane may be built before that.
    // Wire (register actions + starter flow + poll pump) on first
    // refreshFlows() after boot — the engine registry refuses registrations
    // until Initialize() anyway.
    return &instance();
}

FlowService::~FlowService()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Wiring + flows
// ---------------------------------------------------------------------------

void FlowService::registerDomainActions()
{
    if (engineWired_)
        return;
    auto &flow = fa::FlowEngine::Instance();
    if (auto r = flow.Initialize(); !r.ok())
        return;   // not bootable yet — retried on the next refreshFlows()
    (void)flow.Start();
    installDomainActions();
    (void)flow.Load(kStarterFlow, "vgr");
    engineWired_ = true;

    if (!pump_) {
        pump_ = new QTimer(this);
        pump_->setInterval(250);
        connect(pump_, &QTimer::timeout, this, &FlowService::pumpTick);
    }
    // The pump runs whenever an execution exists (started in start()).
    refreshFlows();
}

void FlowService::refreshFlows()
{
    registerDomainActions();

    auto &flow = fa::FlowEngine::Instance();
    QVariantList next;
    for (const auto &id : flow.FlowIds()) {
        auto f = flow.GetFlow(id);
        if (!f.ok())
            continue;
        QVariantList nodes;
        for (const fa::FlowNode &n : f.value().nodes)
            nodes.append(QVariantMap{
                { QStringLiteral("id"), QString::fromStdString(n.id) },
                { QStringLiteral("label"), QString::fromStdString(n.label) },
                { QStringLiteral("kind"), nodeKindName(n.kind) },
            });
        next.append(QVariantMap{
            { QStringLiteral("id"), QString::fromStdString(id) },
            { QStringLiteral("name"), QString::fromStdString(f.value().name) },
            { QStringLiteral("nodeCount"), int(f.value().nodes.size()) },
            { QStringLiteral("nodes"), nodes },
        });
    }
    if (next != flows_) {
        flows_ = next;
        emit flowsChanged();
    }
}

bool FlowService::loadFlow(const QString &document)
{
    registerDomainActions();
    auto &flow = fa::FlowEngine::Instance();
    auto id = flow.Load(document.toStdString(), "vgr");
    if (!id.ok()) {
        toast(QStringLiteral("Flow refused: %1")
                  .arg(QString::fromStdString(id.error().message)));
        return false;
    }
    refreshFlows();
    toast(QStringLiteral("Flow '%1' loaded.").arg(id.value()), QStringLiteral("success"));
    return true;
}

void FlowService::loadStarterFlow()
{
    registerDomainActions();
    (void)fa::FlowEngine::Instance().Load(kStarterFlow, "vgr");
    refreshFlows();
}

void FlowService::removeFlow(const QString &flowId)
{
    (void)fa::FlowEngine::Instance().RemoveFlow(flowId.toStdString());
    refreshFlows();
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

void FlowService::start(const QString &flowId)
{
    registerDomainActions();
    auto execId = fa::FlowEngine::Instance().Start(flowId.toStdString());
    if (!execId.ok()) {
        toast(QStringLiteral("Flow refused: %1")
                  .arg(QString::fromStdString(execId.error().message)));
        return;
    }
    executionId_ = QString::fromStdString(execId.value());
    refreshExecution();
    emit executionChanged();
}

void FlowService::pause()
{
    if (!executionId_.isEmpty())
        (void)fa::FlowEngine::Instance().Pause(executionId_.toStdString());
    refreshExecution();
    emit executionChanged();
}

void FlowService::resume()
{
    if (!executionId_.isEmpty())
        (void)fa::FlowEngine::Instance().Resume(executionId_.toStdString());
    refreshExecution();
    emit executionChanged();
}

void FlowService::skip()
{
    if (!executionId_.isEmpty())
        (void)fa::FlowEngine::Instance().Skip(executionId_.toStdString());
    refreshExecution();
    emit executionChanged();
}

void FlowService::stop()
{
    if (!executionId_.isEmpty())
        (void)fa::FlowEngine::Instance().Stop(executionId_.toStdString());
    refreshExecution();
    emit executionChanged();
}

void FlowService::jumpTo(const QString &nodeId)
{
    if (!executionId_.isEmpty())
        (void)fa::FlowEngine::Instance().JumpTo(executionId_.toStdString(),
                                                nodeId.toStdString());
    refreshExecution();
    emit executionChanged();
}

bool FlowService::executeAction(const QString &actionType, const QString &payload)
{
    registerDomainActions();
    auto r = fa::FlowEngine::Instance().ExecuteAction(actionType.toStdString(),
                                                      payload.toStdString());
    if (!r.ok()) {
        toast(QStringLiteral("Action refused: %1")
                  .arg(QString::fromStdString(r.error().message)));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// State + pump
// ---------------------------------------------------------------------------

void FlowService::refreshExecution()
{
    QVariantMap next;
    bool active = false;

    if (!executionId_.isEmpty()) {
        auto view = fa::FlowEngine::Instance().ExecutionState(executionId_.toStdString());
        if (view.ok()) {
            const fa::ExecutionStateView &v = view.value();
            active = v.state == fa::FlowState::Running || v.state == fa::FlowState::Paused;
            next.insert(QStringLiteral("state"), flowStateName(v.state));
            next.insert(QStringLiteral("flowId"), QString::fromStdString(v.flowId));
            next.insert(QStringLiteral("currentNodeId"),
                        QString::fromStdString(v.currentNodeId));
            next.insert(QStringLiteral("waitingFor"),
                        QString::fromStdString(v.waitingFor));

            // The node timeline: the flow's own node order (GetFlow), annotated
            // with each node's live state from the execution's history.
            QVariantList nodes;
            auto f = fa::FlowEngine::Instance().GetFlow(v.flowId);
            std::map<std::string, const fa::NodeRecord *, std::less<>> byId;
            for (const fa::NodeRecord &rec : v.history)
                byId[rec.nodeId] = &rec;
            if (f.ok()) {
                for (const fa::FlowNode &n : f.value().nodes) {
                    QVariantMap row{
                        { QStringLiteral("id"), QString::fromStdString(n.id) },
                        { QStringLiteral("label"), QString::fromStdString(n.label) },
                        { QStringLiteral("kind"), nodeKindName(n.kind) },
                    };
                    auto it = byId.find(n.id);
                    row.insert(QStringLiteral("state"),
                               it != byId.end() ? nodeStateName(it->second->state)
                                                : QStringLiteral("pending"));
                    nodes.append(row);
                }
            }
            next.insert(QStringLiteral("nodes"), nodes);
        } else {
            executionId_.clear();   // the execution is gone (engine restart)
        }
    }

    if (!active) {
        if (pump_)
            pump_->stop();
        running_ = false;
        if (!executionId_.isEmpty())
            executionId_.clear();
    } else {
        running_ = next.value(QStringLiteral("state")).toString() == QLatin1String("running");
        if (pump_ && !pump_->isActive())
            pump_->start();
    }

    if (next != execution_) {
        execution_ = next;
        emit executionChanged();
    }
}

void FlowService::pumpTick()
{
    // The engine is externally-clocked: Poll() advances delay/timer nodes
    // whose deadline has passed (the Drain loop re-reads NowMs()).
    (void)fa::FlowEngine::Instance().Poll();
    refreshExecution();
    // A completed/stopped execution clears itself through refreshExecution.
    if (executionId_.isEmpty())
        pump_->stop();
}

void FlowService::shutdown()
{
    if (pump_)
        pump_->stop();
}
