#pragma once

// FlowEngine (docs/specs/26): the Phase 14 facade. Owns the flow registry,
// the action/condition/trigger plugin registries, templates, macros, the
// deterministic executor, execution history, variables, and crash recovery.
// It coordinates the other engines through registered actions only — it never
// renders, plays media, or composes scenes itself.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/automation/AutomationPlugin.hpp"
#include "modules/automation/AutomationTypes.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace bps::automation {

class FlowEngine final : public IService {
public:
    static FlowEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "FlowEngine"; }

    // --- Plugin registries (docs/specs/26 §Plugin Support) ---
    Result<void> RegisterAction(std::string_view type, ActionFactory factory);
    Result<void> UnregisterAction(std::string_view type);
    std::vector<std::string> ActionNames() const;
    Result<void> RegisterCondition(std::string_view type, ConditionFactory factory);
    Result<void> UnregisterCondition(std::string_view type);
    std::vector<std::string> ConditionNames() const;
    Result<void> RegisterTrigger(std::string_view type, TriggerFactory factory);
    std::vector<std::string> TriggerNames() const;

    // --- Flow management (docs/specs/26 §Flow Model, §Templates, §.vgr) ---
    // Loads a flow document (json or vgr) into the registry and validates it.
    Result<std::string> Load(std::string_view source, std::string_view format,
                             std::string_view flowId = "");
    Result<std::string> Save(const Flow& flow, std::string_view format) const;
    Result<size_t> Validate(const Flow& flow) const;          // warnings
    Result<Flow> GetFlow(std::string_view flowId) const;
    std::vector<std::string> FlowIds() const;
    size_t FlowCount() const;
    Result<void> RemoveFlow(std::string_view flowId);
    Result<std::string> CreateTemplate(std::string_view name, const Flow& flow);
    std::vector<std::string> Templates() const;
    Result<Flow> ApplyTemplate(std::string_view templateId, std::string_view newId = "");

    // --- Macros (docs/specs/26 §Macros) ---
    Result<void> RegisterMacro(std::string_view name,
                               const std::vector<std::pair<std::string, std::string>>& actions);
    Result<void> RunMacro(std::string_view name,
                          const std::map<std::string, std::string, std::less<>>& vars = {});

    // --- Execution (docs/specs/26 §Manual Override, §Emergency Override) ---
    Result<std::string> Start(std::string_view flowId,
                              const std::map<std::string, std::string, std::less<>>& vars = {});
    Result<void> Pause(std::string_view executionId);
    Result<void> Resume(std::string_view executionId);
    Result<void> Stop(std::string_view executionId);
    Result<std::string> Restart(std::string_view executionId);      // new execution id
    Result<void> Skip(std::string_view executionId);                // skip current node
    Result<void> JumpTo(std::string_view executionId, std::string_view nodeId);
    Result<void> Override(std::string_view executionId,
                          std::string_view actionType, std::string_view payload);
    Result<void> EmergencyStop();
    Result<std::string> ExecuteAction(std::string_view actionType, std::string_view payload,
                                      const std::map<std::string, std::string, std::less<>>& vars = {});

    // --- State, history, debug (docs/specs/26 §Debug Mode, §Execution History) ---
    Result<ExecutionStateView> ExecutionState(std::string_view executionId) const;
    Result<std::vector<NodeRecord>> History(std::string_view executionId) const;
    Result<FlowSnapshot> Snapshot(std::string_view executionId) const;
    Result<std::string> Recover(const FlowSnapshot& snapshot);
    std::vector<std::string> ActiveExecutions() const;

    // --- Variables (docs/specs/26 §Variables, §Data Binding) ---
    Result<void> SetVariable(std::string_view executionId, std::string_view name,
                             std::string_view value);
    Result<std::string> GetVariable(std::string_view executionId, std::string_view name) const;
    std::vector<std::string> Variables(std::string_view executionId) const;

    // --- Event pump (docs/specs/26 §EventBus Integration) ---
    // Notifies all waiting executions that an event topic arrived. EventBus
    // subscriptions are forwarded here (WireEvents); actions use SendEvent.
    Result<size_t> Notify(std::string_view eventTopic, std::string_view executionId = "");
    // Deferred internal dispatch used by send_event actions (drained after the
    // current execution step — never re-entrant).
    Result<void> RequestDispatch(std::string_view executionId, std::string_view topic);
    // Advances any deadline-based waits (delays/timers) whose time has come.
    Result<void> Poll(std::string_view executionId = "");

    // --- Events ---
    void WireEvents();
    void UnwireEvents();
    void OnMediaStateChanged(const events::MediaStateChanged& e);
    void OnDisplayConnected(const events::DisplayDeviceConnected& e);
    void OnDisplayDisconnected(const events::DisplayDeviceDisconnected& e);
    void OnPresentationCompleted(const events::PresentationCompleted& e);
    void OnSceneComposed(const events::SceneComposed& e);

    // Substitutes {NAME} tokens from a variable map (docs/specs/26 §Variables).
    static std::string Substitute(std::string_view text,
                                  const std::map<std::string, std::string, std::less<>>& vars);

private:
    FlowEngine() = default;

    struct Execution {
        std::string executionId;
        std::string flowId;
        FlowState state = FlowState::Idle;
        std::vector<FlowNode> order;
        size_t pc = 0;
        std::map<std::string, std::string, std::less<>> variables;
        std::vector<NodeRecord> history;
        std::set<std::string> completedNodes;
        std::string waitingFor;         // event topic currently waited on
        std::string waitNodeId;         // node that owns the wait ("" = start trigger)
        int64_t waitDeadlineMs = 0;     // 0 = indefinite
        int64_t startedMs = 0;
        int64_t lastEventMs = 0;
        std::vector<std::string> activeConditions;
    };

    // Internal node execution: returns true when the flow advanced at least
    // one node (may loop), false when waiting/blocked.
    Result<size_t> Drain(Execution& exec);
    Result<void> ExecuteNode(Execution& exec, FlowNode& node);
    void CompleteNode(Execution& exec, FlowNode& node, NodeState state,
                      const std::string& result);
    Result<void> HandleFailure(Execution& exec, FlowNode& node, const std::string& message);
    void Dispatch(Execution& exec, std::string_view topic);
    void ProcessPendingDispatches();
    static const FlowNode* FindNode(const Execution& exec, std::string_view id);
    static int64_t NowMs();

    // Registries (built-ins registered in Initialize; plugins add more).
    std::map<std::string, ActionFactory, std::less<>> actions_;
    std::map<std::string, ConditionFactory, std::less<>> conditions_;
    std::map<std::string, TriggerFactory, std::less<>> triggers_;

    std::map<std::string, Flow, std::less<>> flows_;
    std::map<std::string, Flow, std::less<>> templates_;               // name -> template flow
    std::map<std::string, std::vector<std::pair<std::string, std::string>>, std::less<>>
        macros_;                                                       // name -> action list
    std::map<std::string, Execution, std::less<>> executions_;         // executionId
    std::vector<std::string> pendingDispatches_;
    std::vector<Subscription> subscriptions_;

    mutable std::recursive_mutex mutex_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> executionCount_{0};
    std::atomic<uint64_t> nodeCount_{0};
    uint64_t nextExecutionId_ = 1;
};

} // namespace bps::automation
