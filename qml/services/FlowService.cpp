#include "services/FlowService.h"

#include "services/EngineWorker.h"
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
    // Claimed HERE, on the interface thread, so a second caller cannot queue a
    // second wiring while the first is still in flight (the engine part below
    // runs on the worker, and the posts are FIFO: a command queued right after
    // this one still finds a started engine).
    engineWired_ = true;
    if (!pump_) {
        // A QTimer belongs to the thread that created it: made here, started and
        // stopped by refreshExecution (also here).
        pump_ = new QTimer(this);
        pump_->setInterval(250);
        connect(pump_, &QTimer::timeout, this, &FlowService::pumpTick);
    }
    // WIRING THE ENGINE IS ENGINE WORK: Initialize/Start, the domain actions and
    // the starter flow all go to the worker, never the interface thread (this
    // used to run on the thread that draws, on the first pane visit).
    vgr::EngineWorker::instance().runOffThread([this] {
        auto &flow = fa::FlowEngine::Instance();
        if (auto r = flow.Initialize(); !r.ok()) {
            // Not bootable yet: give the claim back so the next refreshFlows()
            // retries (the original contract).
            vgr::EngineWorker::onInterfaceThread(this, [this] { engineWired_ = false; });
            return;
        }
        (void)flow.Start();
        installDomainActions();
        (void)flow.Load(kStarterFlow, "vgr");
    });
    // The pump runs whenever an execution exists (started in start()).
    refreshFlows();
}

// Reads the engine's flow registry ON THE WORKER and publishes the list here.
// The rows QML binds to are built off the interface thread and handed over as a
// finished value — the thread that draws only assigns and emits.
void FlowService::refreshFlows()
{
    registerDomainActions();
    vgr::EngineWorker::instance().runOffThread([this] {
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
        vgr::EngineWorker::onInterfaceThread(this, [this, next] {
            if (next != flows_) {
                flows_ = next;
                emit flowsChanged();
            }
        });
    });
}

// Loading is engine work; the ANSWER is what makes its way back (the toast says
// which way it went). True means "accepted for loading" — the load itself and
// its outcome land on the flow list a moment later.
bool FlowService::loadFlow(const QString &document)
{
    registerDomainActions();
    vgr::EngineWorker::instance().runOffThread([this, document] {
        auto id = fa::FlowEngine::Instance().Load(document.toStdString(), "vgr");
        const bool failed = !id.ok();
        const QString detail = failed ? QString::fromStdString(id.error().message)
                                      : QString::fromStdString(id.value());
        vgr::EngineWorker::onInterfaceThread(this, [this, failed, detail] {
            if (failed) {
                toast(QStringLiteral("Flow refused: %1").arg(detail));
                return;
            }
            toast(QStringLiteral("Flow '%1' loaded.").arg(detail), QStringLiteral("success"));
            refreshFlows();
        });
    });
    return true;
}

void FlowService::loadStarterFlow()
{
    registerDomainActions();
    vgr::EngineWorker::instance().runOffThread([this] {
        (void)fa::FlowEngine::Instance().Load(kStarterFlow, "vgr");
        vgr::EngineWorker::onInterfaceThread(this, [this] { refreshFlows(); });
    });
}

void FlowService::removeFlow(const QString &flowId)
{
    vgr::EngineWorker::instance().runOffThread([this, flowId] {
        (void)fa::FlowEngine::Instance().RemoveFlow(flowId.toStdString());
        vgr::EngineWorker::onInterfaceThread(this, [this] { refreshFlows(); });
    });
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

void FlowService::start(const QString &flowId)
{
    registerDomainActions();
    vgr::EngineWorker::instance().runOffThread([this, flowId] {
        auto execId = fa::FlowEngine::Instance().Start(flowId.toStdString());
        const bool failed = !execId.ok();
        const QString detail = failed ? QString::fromStdString(execId.error().message)
                                      : QString::fromStdString(execId.value());
        vgr::EngineWorker::onInterfaceThread(this, [this, failed, detail] {
            if (failed) {
                toast(QStringLiteral("Flow refused: %1").arg(detail));
                return;
            }
            executionId_ = detail;
            emit executionChanged();          // the pump refreshes from here
        });
    });
}

// The four transport commands share one shape: the engine call goes to the
// worker with the id it applies to; the refreshed state comes back through
// refreshExecution (which is itself off-thread). The helper is NAMED for what it
// does — it runs the command off the interface thread — because that is the
// hand-off shape the GUI-thread guard recognizes (tools/check_gui_thread_blocks),
// and the name keeps it recognizable to a reader too.
void FlowService::pause()   { runTransportOffThread(QStringLiteral("pause")); }
void FlowService::resume()  { runTransportOffThread(QStringLiteral("resume")); }
void FlowService::skip()    { runTransportOffThread(QStringLiteral("skip")); }
void FlowService::stop()    { runTransportOffThread(QStringLiteral("stop")); }

void FlowService::runTransportOffThread(const QString &what)
{
    const QString id = executionId_;
    if (id.isEmpty()) {
        emit executionChanged();
        return;
    }
    vgr::EngineWorker::instance().runOffThread([this, what, id] {
        auto &flow = fa::FlowEngine::Instance();
        if (what == QLatin1String("pause"))
            (void)flow.Pause(id.toStdString());
        else if (what == QLatin1String("resume"))
            (void)flow.Resume(id.toStdString());
        else if (what == QLatin1String("skip"))
            (void)flow.Skip(id.toStdString());
        else
            (void)flow.Stop(id.toStdString());
        vgr::EngineWorker::onInterfaceThread(this, [this] {
            refreshExecution();
            emit executionChanged();
        });
    });
}

void FlowService::jumpTo(const QString &nodeId)
{
    const QString id = executionId_;
    if (id.isEmpty()) {
        emit executionChanged();
        return;
    }
    vgr::EngineWorker::instance().runOffThread([this, id, nodeId] {
        (void)fa::FlowEngine::Instance().JumpTo(id.toStdString(), nodeId.toStdString());
        vgr::EngineWorker::onInterfaceThread(this, [this] {
            refreshExecution();
            emit executionChanged();
        });
    });
}

// Fire one action now (the pane's quick-action buttons). Engine work on the
// worker; a refusal comes back as its toast. True means "accepted for firing".
bool FlowService::executeAction(const QString &actionType, const QString &payload)
{
    registerDomainActions();
    vgr::EngineWorker::instance().runOffThread([this, actionType, payload] {
        auto r = fa::FlowEngine::Instance().ExecuteAction(actionType.toStdString(),
                                                         payload.toStdString());
        if (r.ok())
            return;
        const QString detail = QString::fromStdString(r.error().message);
        vgr::EngineWorker::onInterfaceThread(this, [this, detail] {
            toast(QStringLiteral("Action refused: %1").arg(detail));
        });
    });
    return true;
}

// ---------------------------------------------------------------------------
// State + pump
// ---------------------------------------------------------------------------

// Reads the engine's execution state ON THE WORKER and publishes the map here
// (the pane binds `execution`, `running` and `executionId`, and the pump calls
// this every 250 ms — a quarter-second cadence is exactly where a lock wait on
// the thread that draws would be felt).
void FlowService::refreshExecution()
{
    const QString id = executionId_;
    vgr::EngineWorker::instance().runOffThread([this, id] {
        QVariantMap next;
        bool active = false;
        bool gone = false;

        if (!id.isEmpty()) {
            auto view = fa::FlowEngine::Instance().ExecutionState(id.toStdString());
            if (view.ok()) {
                const fa::ExecutionStateView &v = view.value();
                active = v.state == fa::FlowState::Running || v.state == fa::FlowState::Paused;
                next.insert(QStringLiteral("state"), flowStateName(v.state));
                next.insert(QStringLiteral("flowId"), QString::fromStdString(v.flowId));
                next.insert(QStringLiteral("currentNodeId"),
                            QString::fromStdString(v.currentNodeId));
                next.insert(QStringLiteral("waitingFor"),
                            QString::fromStdString(v.waitingFor));

                // The node timeline: the flow's own node order (GetFlow),
                // annotated with each node's live state from the history.
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
                gone = true;   // the execution is gone (engine restart)
            }
        }

        vgr::EngineWorker::onInterfaceThread(this, [this, id, next, active, gone] {
            // The answer belongs to the execution it was asked about: the pump
            // may have moved on (a new flow started) while it was in flight.
            if (id != executionId_)
                return;
            if (gone)
                executionId_.clear();
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
        });
    });
}

void FlowService::pumpTick()
{
    // The engine is externally-clocked: Poll() advances delay/timer nodes whose
    // deadline has passed (the Drain loop re-reads NowMs()). Engine work — on
    // the worker — and the state read that follows it goes there too.
    vgr::EngineWorker::instance().runOffThread([this] {
        (void)fa::FlowEngine::Instance().Poll();
        vgr::EngineWorker::onInterfaceThread(this, [this] {
            refreshExecution();
            // A completed/stopped execution clears itself through refreshExecution.
            if (executionId_.isEmpty() && pump_)
                pump_->stop();
        });
    });
}

void FlowService::shutdown()
{
    if (pump_)
        pump_->stop();
}
