#include "modules/automation/FlowEngine.hpp"

#include "core/config/Json.hpp"
#include "core/logging/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <format>

namespace bps::automation {

namespace {

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string GetStr(const json::Value* v) {
    return v ? std::string(v->asString()) : std::string();
}

// --- Built-in actions (docs/specs/26 §Actions) -------------------------------
class NoopAction final : public IAction {
public:
    const char* Type() const noexcept override { return "noop"; }
    Result<ActionOutcome> Execute(AutomationContext&) override {
        return ActionOutcome{ActionOutcome::Kind::Completed, {}, {}};
    }
};

class SendEventAction final : public IAction {
public:
    const char* Type() const noexcept override { return "send_event"; }
    Result<ActionOutcome> Execute(AutomationContext& ctx) override {
        std::string topic = ctx.Substitute(ctx.node ? ctx.node->actionPayload : std::string_view{});
        if (topic.empty())
            return ActionOutcome{ActionOutcome::Kind::Failed, {}, "send_event: empty topic"};
        // Deferred dispatch — never re-entrant; the engine processes it at the
        // end of the current execution step (docs/specs/26 §EventBus Integration).
        (void)ctx.engine->RequestDispatch(ctx.executionId, topic);
        return ActionOutcome{ActionOutcome::Kind::Completed, {}, {}};
    }
};

class LogAction final : public IAction {
public:
    const char* Type() const noexcept override { return "log"; }
    Result<ActionOutcome> Execute(AutomationContext& ctx) override {
        std::string msg = ctx.Substitute(ctx.node ? ctx.node->actionPayload : std::string_view{});
        Logger::Instance().Info("[flow] " + msg, "FlowEngine");
        return ActionOutcome{ActionOutcome::Kind::Completed, {}, {}};
    }
};

// The Notification service consumes flow events; this action only marks intent
// (docs/specs/26 §Notification Integration — never hardcoded).
class NotifyAction final : public IAction {
public:
    const char* Type() const noexcept override { return "notify"; }
    Result<ActionOutcome> Execute(AutomationContext&) override {
        return ActionOutcome{ActionOutcome::Kind::Completed, {}, {}};
    }
};

// --- Built-in conditions (docs/specs/26 §Conditional Logic) -------------------
class TrueCondition final : public ICondition {
public:
    const char* Type() const noexcept override { return "true"; }
    Result<bool> Evaluate(AutomationContext&) override { return true; }
};

class FalseCondition final : public ICondition {
public:
    const char* Type() const noexcept override { return "false"; }
    Result<bool> Evaluate(AutomationContext&) override { return false; }
};

// payload: "NAME=VALUE"
class VariableEqualsCondition final : public ICondition {
public:
    const char* Type() const noexcept override { return "variable_equals"; }
    Result<bool> Evaluate(AutomationContext& ctx) override {
        std::string spec = ctx.node ? ctx.node->conditionPayload : std::string();
        size_t eq = spec.find('=');
        if (eq == std::string::npos || eq == 0)
            return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                               "variable_equals expects NAME=VALUE");
        std::string name = spec.substr(0, eq);
        std::string expect = spec.substr(eq + 1);
        auto it = ctx.variables->find(name);
        return it != ctx.variables->end() && it->second == expect;
    }
};

// payload: variable name; true when set and non-empty.
class VariableSetCondition final : public ICondition {
public:
    const char* Type() const noexcept override { return "variable_set"; }
    Result<bool> Evaluate(AutomationContext& ctx) override {
        std::string name = ctx.node ? ctx.node->conditionPayload : std::string();
        auto it = ctx.variables->find(name);
        return it != ctx.variables->end() && !it->second.empty();
    }
};

// payload: expected output id; compares the "OUTPUT" variable. The engine does
// not know displays — the operator/controller sets OUTPUT (docs/specs/26 §6).
class OutputIsCondition final : public ICondition {
public:
    const char* Type() const noexcept override { return "output_is"; }
    Result<bool> Evaluate(AutomationContext& ctx) override {
        auto it = ctx.variables->find("OUTPUT");
        if (it == ctx.variables->end()) return false;
        std::string expect = ctx.node ? ctx.node->conditionPayload : std::string();
        return ToLower(it->second) == ToLower(expect);
    }
};

// payload: "1"; checks the "GPU_AVAILABLE" variable (set by the Adaptive
// Runtime / operator) — decoupled from hardware introspection.
class GpuAvailableCondition final : public ICondition {
public:
    const char* Type() const noexcept override { return "gpu_available"; }
    Result<bool> Evaluate(AutomationContext& ctx) override {
        auto it = ctx.variables->find("GPU_AVAILABLE");
        if (it == ctx.variables->end()) return false;
        return it->second == "1" || ToLower(it->second) == "true";
    }
};

// --- Built-in triggers (docs/specs/26 §Triggers) ------------------------------
class ManualTrigger final : public ITrigger {
public:
    const char* Type() const noexcept override { return "manual"; }
    std::string WaitsFor(std::string_view) const override { return {}; }
};

class EventTrigger final : public ITrigger {
public:
    const char* Type() const noexcept override { return "event"; }
    std::string WaitsFor(std::string_view payload) const override {
        return std::string(payload);
    }
};

class TimeTrigger final : public ITrigger {
public:
    const char* Type() const noexcept override { return "time"; }
    std::string WaitsFor(std::string_view) const override { return {}; }
};

} // namespace

// ===========================================================================
// Small helpers
// ===========================================================================
std::string AutomationContext::Substitute(std::string_view text) const {
    return FlowEngine::Substitute(text, variables ? *variables : std::map<std::string, std::string, std::less<>>{});
}

int64_t FlowEngine::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string FlowEngine::Substitute(std::string_view text,
                                   const std::map<std::string, std::string, std::less<>>& vars) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '{') {
            size_t close = text.find('}', i + 1);
            if (close != std::string_view::npos) {
                std::string name(text.substr(i + 1, close - i - 1));
                auto it = vars.find(name);
                if (it != vars.end()) {
                    out += it->second;
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

const FlowNode* FlowEngine::FindNode(const Execution& exec, std::string_view id) {
    for (const auto& n : exec.order)
        if (n.id == id) return &n;
    return nullptr;
}

namespace {
size_t IndexOf(const std::vector<FlowNode>& order, std::string_view id) {
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i].id == id) return i;
    return order.size();
}

// Advances the program counter: an explicit cue-chain target (nextNodeId)
// wins; otherwise the linear successor (docs/specs/26 §Cue System, §Branching).
size_t AdvancePc(const std::vector<FlowNode>& order, size_t pc) {
    if (pc < order.size() && !order[pc].nextNodeId.empty()) {
        size_t target = IndexOf(order, order[pc].nextNodeId);
        if (target < order.size()) return target;
    }
    return pc + 1;
}
} // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================
Result<void> FlowEngine::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (initialized_.load()) return Ok();
    initialized_.store(true);

    // Built-in actions (docs/specs/26 §Actions). Domain actions (song, bible,
    // media, scene...) are registered by consumers/plugins through the same
    // registry — the engine core stays decoupled.
    actions_["noop"] = []() -> std::shared_ptr<IAction> { return std::make_shared<NoopAction>(); };
    actions_["send_event"] = []() -> std::shared_ptr<IAction> {
        return std::make_shared<SendEventAction>();
    };
    actions_["log"] = []() -> std::shared_ptr<IAction> { return std::make_shared<LogAction>(); };
    actions_["notify"] = []() -> std::shared_ptr<IAction> { return std::make_shared<NotifyAction>(); };

    conditions_["true"] = []() -> std::shared_ptr<ICondition> {
        return std::make_shared<TrueCondition>();
    };
    conditions_["false"] = []() -> std::shared_ptr<ICondition> {
        return std::make_shared<FalseCondition>();
    };
    conditions_["variable_equals"] = []() -> std::shared_ptr<ICondition> {
        return std::make_shared<VariableEqualsCondition>();
    };
    conditions_["variable_set"] = []() -> std::shared_ptr<ICondition> {
        return std::make_shared<VariableSetCondition>();
    };
    conditions_["output_is"] = []() -> std::shared_ptr<ICondition> {
        return std::make_shared<OutputIsCondition>();
    };
    conditions_["gpu_available"] = []() -> std::shared_ptr<ICondition> {
        return std::make_shared<GpuAvailableCondition>();
    };

    triggers_["manual"] = []() -> std::shared_ptr<ITrigger> {
        return std::make_shared<ManualTrigger>();
    };
    triggers_["event"] = []() -> std::shared_ptr<ITrigger> {
        return std::make_shared<EventTrigger>();
    };
    triggers_["time"] = []() -> std::shared_ptr<ITrigger> {
        return std::make_shared<TimeTrigger>();
    };

    WireEvents();
    return Ok();
}

Result<void> FlowEngine::Start() {
    running_.store(true);
    return Ok();
}

Result<void> FlowEngine::Stop() {
    running_.store(false);
    return Ok();
}

Result<void> FlowEngine::Shutdown() {
    if (!initialized_.load()) return Ok();
    UnwireEvents();
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        actions_.clear();
        conditions_.clear();
        triggers_.clear();
        flows_.clear();
        templates_.clear();
        macros_.clear();
        executions_.clear();
        pendingDispatches_.clear();
        initialized_.store(false);
    }
    return Ok();
}

Result<void> FlowEngine::Reload() {
    // Re-validate every loaded flow (docs/specs/26 §Recovery / validation).
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t failures = 0;
    for (auto& [id, flow] : flows_) {
        auto w = Validate(flow);
        if (!w.ok()) {
            ++failures;
            Logger::Instance().Warning("FlowEngine: re-validation failed for " + id +
                                           ": " + w.error().message,
                                       "FlowEngine");
        }
    }
    if (failures > 0) errorCount_.fetch_add(failures);
    return Ok();
}

Result<void> FlowEngine::Reset() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& [id, exec] : executions_)
        if (exec.state == FlowState::Running || exec.state == FlowState::Paused)
            exec.state = FlowState::Stopped;
    executions_.clear();
    flows_.clear();
    templates_.clear();
    macros_.clear();
    pendingDispatches_.clear();
    return Ok();
}

HealthReport FlowEngine::GetHealth() const {
    HealthReport h;
    h.state = errorCount_.load() > 0 ? HealthState::Degraded : HealthState::Healthy;
    h.errorCount = errorCount_.load();
    h.detail = std::format("{} flows, {} executions", flows_.size(), executions_.size());
    return h;
}

Metrics FlowEngine::MetricsSnapshot() const {
    Metrics m;
    m.errorCount = errorCount_.load();
    m.queueLength = nodeCount_.load();
    m.health = GetHealth().state;
    return m;
}

// ===========================================================================
// Plugin registries
// ===========================================================================
Result<void> FlowEngine::RegisterAction(std::string_view type, ActionFactory factory) {
    if (!factory) return Error::Make(Err::InvalidArgument, "FlowEngine", "null action factory");
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (actions_.count(std::string(type)))
        return Error::Make(Err::AlreadyExists, "FlowEngine", "action already registered: " + std::string(type));
    actions_[std::string(type)] = factory;
    return Ok();
}

Result<void> FlowEngine::UnregisterAction(std::string_view type) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return actions_.erase(std::string(type)) ? Ok()
                                             : Error::Make(Err::NotFound, "FlowEngine", "action not found: " + std::string(type));
}

std::vector<std::string> FlowEngine::ActionNames() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [k, v] : actions_) out.push_back(k);
    return out;
}

Result<void> FlowEngine::RegisterCondition(std::string_view type, ConditionFactory factory) {
    if (!factory) return Error::Make(Err::InvalidArgument, "FlowEngine", "null condition factory");
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (conditions_.count(std::string(type)))
        return Error::Make(Err::AlreadyExists, "FlowEngine", "condition already registered: " + std::string(type));
    conditions_[std::string(type)] = factory;
    return Ok();
}

Result<void> FlowEngine::UnregisterCondition(std::string_view type) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return conditions_.erase(std::string(type)) ? Ok()
                                                : Error::Make(Err::NotFound, "FlowEngine", "condition not found: " + std::string(type));
}

std::vector<std::string> FlowEngine::ConditionNames() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [k, v] : conditions_) out.push_back(k);
    return out;
}

Result<void> FlowEngine::RegisterTrigger(std::string_view type, TriggerFactory factory) {
    if (!factory) return Error::Make(Err::InvalidArgument, "FlowEngine", "null trigger factory");
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (triggers_.count(std::string(type)))
        return Error::Make(Err::AlreadyExists, "FlowEngine", "trigger already registered: " + std::string(type));
    triggers_[std::string(type)] = factory;
    return Ok();
}

std::vector<std::string> FlowEngine::TriggerNames() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [k, v] : triggers_) out.push_back(k);
    return out;
}

// ===========================================================================
// Flow serialization (docs/specs/26 §.vgr, §Flow Model)
// ===========================================================================
namespace {

std::string KindStr(NodeKind k) {
    switch (k) {
        case NodeKind::Action: return "action";
        case NodeKind::Condition: return "condition";
        case NodeKind::Parallel: return "parallel";
        case NodeKind::Delay: return "delay";
        case NodeKind::Wait: return "wait";
        case NodeKind::Macro: return "macro";
        case NodeKind::End: return "end";
    }
    return "action";
}

NodeKind KindFromStr(std::string_view s) {
    if (s == "condition") return NodeKind::Condition;
    if (s == "parallel") return NodeKind::Parallel;
    if (s == "delay") return NodeKind::Delay;
    if (s == "wait") return NodeKind::Wait;
    if (s == "macro") return NodeKind::Macro;
    if (s == "end") return NodeKind::End;
    return NodeKind::Action;
}

std::string PolicyStr(FailurePolicy p) {
    switch (p) {
        case FailurePolicy::Retry: return "retry";
        case FailurePolicy::Skip: return "skip";
        case FailurePolicy::Fallback: return "fallback";
        case FailurePolicy::Pause: return "pause";
        case FailurePolicy::Notify: return "notify";
        case FailurePolicy::Abort: return "abort";
    }
    return "pause";
}

FailurePolicy PolicyFromStr(std::string_view s) {
    if (s == "retry") return FailurePolicy::Retry;
    if (s == "skip") return FailurePolicy::Skip;
    if (s == "fallback") return FailurePolicy::Fallback;
    if (s == "notify") return FailurePolicy::Notify;
    if (s == "abort") return FailurePolicy::Abort;
    return FailurePolicy::Pause;
}

json::Value FlowToJson(const Flow& flow) {
    json::Value::Object root;
    root["id"] = json::Value::String(flow.id);
    root["name"] = json::Value::String(flow.name);
    root["version"] = json::Value::String(flow.version);
    if (!flow.templateId.empty()) root["templateId"] = json::Value::String(flow.templateId);
    json::Value::Object trig;
    trig["kind"] = json::Value::String(flow.trigger.kind);
    if (!flow.trigger.payload.empty()) trig["payload"] = json::Value::String(flow.trigger.payload);
    root["trigger"] = json::Value(std::move(trig));

    json::Value::Object vars;
    for (const auto& [k, v] : flow.variables) vars[k] = json::Value::String(v);
    root["variables"] = json::Value(std::move(vars));
    json::Value::Object meta;
    for (const auto& [k, v] : flow.metadata) meta[k] = json::Value::String(v);
    root["metadata"] = json::Value(std::move(meta));

    json::Value::Array nodes;
    for (const auto& n : flow.nodes) {
        json::Value::Object no;
        no["id"] = json::Value::String(n.id);
        no["label"] = json::Value::String(n.label);
        no["kind"] = json::Value::String(KindStr(n.kind));
        if (!n.actionType.empty()) no["actionType"] = json::Value::String(n.actionType);
        if (!n.actionPayload.empty()) no["actionPayload"] = json::Value::String(n.actionPayload);
        if (!n.conditionType.empty()) no["conditionType"] = json::Value::String(n.conditionType);
        if (!n.conditionPayload.empty())
            no["conditionPayload"] = json::Value::String(n.conditionPayload);
        if (!n.waitFor.empty()) no["waitFor"] = json::Value::String(n.waitFor);
        if (n.waitTimeoutMs != 0) no["waitTimeoutMs"] = json::Value::Number(static_cast<double>(n.waitTimeoutMs));
        if (n.timeOffsetMs != 0) no["timeOffsetMs"] = json::Value::Number(static_cast<double>(n.timeOffsetMs));
        if (!n.trueNodeId.empty()) no["trueNodeId"] = json::Value::String(n.trueNodeId);
        if (!n.falseNodeId.empty()) no["falseNodeId"] = json::Value::String(n.falseNodeId);
        if (!n.nextNodeId.empty()) no["nextNodeId"] = json::Value::String(n.nextNodeId);
        if (!n.fallbackNodeId.empty()) no["fallbackNodeId"] = json::Value::String(n.fallbackNodeId);
        no["onFailure"] = json::Value::String(PolicyStr(n.onFailure));
        if (n.retryLimit != 0) no["retryLimit"] = json::Value::Number(static_cast<double>(n.retryLimit));
        json::Value::Array par;
        for (const auto& p : n.parallelNodeIds) par.push_back(json::Value::String(p));
        no["parallelNodeIds"] = json::Value(std::move(par));
        json::Value::Object cues;
        if (!n.cues.beforeStart.empty()) cues["beforeStart"] = json::Value::String(n.cues.beforeStart);
        if (!n.cues.onStart.empty()) cues["onStart"] = json::Value::String(n.cues.onStart);
        if (!n.cues.during.empty()) cues["during"] = json::Value::String(n.cues.during);
        if (!n.cues.onComplete.empty()) cues["onComplete"] = json::Value::String(n.cues.onComplete);
        if (!n.cues.onError.empty()) cues["onError"] = json::Value::String(n.cues.onError);
        if (!n.cues.onCancel.empty()) cues["onCancel"] = json::Value::String(n.cues.onCancel);
        no["cues"] = json::Value(std::move(cues));
        nodes.push_back(json::Value(std::move(no)));
    }
    root["nodes"] = json::Value(std::move(nodes));
    return json::Value(std::move(root));
}

Flow FlowFromJson(const json::Value& v) {
    Flow flow;
    flow.id = GetStr(v.Find("id"));
    flow.name = GetStr(v.Find("name"));
    flow.version = GetStr(v.Find("version"));
    flow.templateId = GetStr(v.Find("templateId"));
    if (const auto* trig = v.Find("trigger")) {
        flow.trigger.kind = GetStr(trig->Find("kind"));
        flow.trigger.payload = GetStr(trig->Find("payload"));
    }
    if (const auto* vars = v.Find("variables")) {
        if (const auto* obj = vars->asObject())
            for (const auto& [k, val] : *obj) flow.variables[k] = std::string(val.asString());
    }
    if (const auto* meta = v.Find("metadata")) {
        if (const auto* obj = meta->asObject())
            for (const auto& [k, val] : *obj) flow.metadata[k] = std::string(val.asString());
    }
    if (const auto* nodes = v.Find("nodes")) {
        if (const auto* arr = nodes->asArray()) {
            for (const auto& nv : *arr) {
                FlowNode n;
                n.id = GetStr(nv.Find("id"));
                n.label = GetStr(nv.Find("label"));
                n.kind = KindFromStr(GetStr(nv.Find("kind")));
                n.actionType = GetStr(nv.Find("actionType"));
                n.actionPayload = GetStr(nv.Find("actionPayload"));
                n.conditionType = GetStr(nv.Find("conditionType"));
                n.conditionPayload = GetStr(nv.Find("conditionPayload"));
                n.waitFor = GetStr(nv.Find("waitFor"));
                n.waitTimeoutMs = nv.Find("waitTimeoutMs") ? nv.Find("waitTimeoutMs")->asInt() : 0;
                n.timeOffsetMs = nv.Find("timeOffsetMs") ? nv.Find("timeOffsetMs")->asInt() : 0;
                n.trueNodeId = GetStr(nv.Find("trueNodeId"));
                n.falseNodeId = GetStr(nv.Find("falseNodeId"));
                n.nextNodeId = GetStr(nv.Find("nextNodeId"));
                n.fallbackNodeId = GetStr(nv.Find("fallbackNodeId"));
                n.onFailure = PolicyFromStr(GetStr(nv.Find("onFailure")));
                n.retryLimit = nv.Find("retryLimit") ? nv.Find("retryLimit")->asInt() : 0;
                if (const auto* par = nv.Find("parallelNodeIds")) {
                    if (const auto* parArr = par->asArray())
                        for (const auto& p : *parArr) n.parallelNodeIds.push_back(std::string(p.asString()));
                }
                if (const auto* cues = nv.Find("cues")) {
                    n.cues.beforeStart = GetStr(cues->Find("beforeStart"));
                    n.cues.onStart = GetStr(cues->Find("onStart"));
                    n.cues.during = GetStr(cues->Find("during"));
                    n.cues.onComplete = GetStr(cues->Find("onComplete"));
                    n.cues.onError = GetStr(cues->Find("onError"));
                    n.cues.onCancel = GetStr(cues->Find("onCancel"));
                }
                // Author convenience: kind=action + actionType=delay/macro.
                if (n.kind == NodeKind::Action && n.actionType == "delay") n.kind = NodeKind::Delay;
                if (n.kind == NodeKind::Action && n.actionType == "macro") n.kind = NodeKind::Macro;
                if (!n.id.empty()) flow.nodes.push_back(std::move(n));
            }
        }
    }
    return flow;
}

} // namespace

Result<std::string> FlowEngine::Load(std::string_view source, std::string_view format,
                                     std::string_view flowId) {
    std::string fmt = ToLower(format);
    if (fmt != "json" && fmt != "vgr")
        return Error::Make(Err::Flow_UnsupportedFormat, "FlowEngine",
                           "unsupported flow format: " + std::string(format));
    auto parsed = json::Parse(std::string(source));
    if (!parsed.ok())
        return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                           "invalid JSON: " + parsed.error().message);
    // .vgr documents wrap the flow definition: {"type":"flow","flow":{...}}.
    const json::Value& root = parsed.value();
    const json::Value* doc = &root;
    if (const auto* f = root.Find("flow"))
        if (const auto* t = root.Find("type"); t && std::string(t->asString()) == "flow")
            doc = f;
    Flow flow = FlowFromJson(*doc);
    if (!flowId.empty()) flow.id = std::string(flowId);
    if (flow.id.empty())
        return Error::Make(Err::Flow_ValidationFailed, "FlowEngine", "flow id is missing");

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (flows_.count(flow.id))
        return Error::Make(Err::Flow_AlreadyExists, "FlowEngine",
                           "flow already loaded: " + flow.id);
    auto warnings = Validate(flow);
    if (!warnings.ok()) return warnings.error();
    const std::string result = flow.id;  // capture before move (std::string is moved-from below)
    flows_[flow.id] = std::move(flow);
    (void)EventBus::Instance().Publish(events::FlowValidated{result, warnings.value()});
    return result;
}

Result<std::string> FlowEngine::Save(const Flow& flow, std::string_view format) const {
    std::string fmt = ToLower(format);
    if (fmt != "json" && fmt != "vgr")
        return Error::Make(Err::Flow_UnsupportedFormat, "FlowEngine",
                           "unsupported flow format: " + std::string(format));
    json::Value root = FlowToJson(flow);
    if (fmt == "vgr") {
        // .vgr native marker: a flow document identifies itself as a
        // Flow/Automation document (docs/specs/26 §.vgr Integration).
        json::Value::Object top;
        top["type"] = json::Value::String("flow");
        top["flow"] = std::move(root);
        return json::Value(std::move(top)).ToString();
    }
    return root.ToString();
}

Result<size_t> FlowEngine::Validate(const Flow& flow) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t warnings = 0;
    std::map<std::string, int, std::less<>> ids;
    for (const auto& n : flow.nodes) {
        if (++ids[n.id] > 1)
            return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                               "duplicate node id: " + n.id);
        if (n.kind == NodeKind::Action) {
            if (!actions_.count(n.actionType))
                return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                                   "unknown action type '" + n.actionType + "' on node " + n.id);
        } else if (n.kind == NodeKind::Condition) {
            if (!conditions_.count(n.conditionType))
                return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                                   "unknown condition type '" + n.conditionType + "' on node " + n.id);
        } else if (n.kind == NodeKind::Macro) {
            if (!macros_.count(n.actionType))
                return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                                   "unknown macro '" + n.actionType + "' on node " + n.id);
        }
        if (!n.trueNodeId.empty()) {
            bool found = false;
            for (const auto& o : flow.nodes) if (o.id == n.trueNodeId) found = true;
            if (!found)
                return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                                   "node " + n.id + " true target missing: " + n.trueNodeId);
        }
        if (!n.falseNodeId.empty()) {
            bool found = false;
            for (const auto& o : flow.nodes) if (o.id == n.falseNodeId) found = true;
            if (!found)
                return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                                   "node " + n.id + " false target missing: " + n.falseNodeId);
        }
        if (!n.fallbackNodeId.empty()) {
            bool found = false;
            for (const auto& o : flow.nodes) if (o.id == n.fallbackNodeId) found = true;
            if (!found)
                return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                                   "node " + n.id + " fallback missing: " + n.fallbackNodeId);
        }
        if (!n.nextNodeId.empty()) {
            bool found = false;
            for (const auto& o : flow.nodes) if (o.id == n.nextNodeId) found = true;
            if (!found)
                return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                                   "node " + n.id + " next target missing: " + n.nextNodeId);
        }
        for (const auto& p : n.parallelNodeIds) {
            bool found = false;
            for (const auto& o : flow.nodes) if (o.id == p) found = true;
            if (!found)
                return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                                   "node " + n.id + " parallel member missing: " + p);
        }
        if (n.kind == NodeKind::Wait && n.waitFor.empty()) ++warnings;
    }
    if (flow.nodes.empty())
        return Error::Make(Err::Flow_ValidationFailed, "FlowEngine", "flow has no nodes");
    if (flow.name.empty()) ++warnings;
    return warnings;
}

Result<Flow> FlowEngine::GetFlow(std::string_view flowId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = flows_.find(std::string(flowId));
    if (it == flows_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine", "flow not found: " + std::string(flowId));
    return it->second;
}

std::vector<std::string> FlowEngine::FlowIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [id, f] : flows_) out.push_back(id);
    return out;
}

size_t FlowEngine::FlowCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return flows_.size();
}

Result<void> FlowEngine::RemoveFlow(std::string_view flowId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (flows_.erase(std::string(flowId)) == 0)
        return Error::Make(Err::Flow_NotFound, "FlowEngine", "flow not found: " + std::string(flowId));
    return Ok();
}

// ===========================================================================
// Templates & macros (docs/specs/26 §Templates, §Macros)
// ===========================================================================
Result<std::string> FlowEngine::CreateTemplate(std::string_view name, const Flow& flow) {
    if (std::string(name).empty())
        return Error::Make(Err::InvalidArgument, "FlowEngine", "template name is empty");
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (templates_.count(std::string(name)))
        return Error::Make(Err::AlreadyExists, "FlowEngine",
                           "template already exists: " + std::string(name));
    Flow t = flow;
    t.id.clear();
    t.templateId = std::string(name);
    t.name = !t.name.empty() ? t.name : std::string(name);
    templates_[std::string(name)] = std::move(t);
    return std::string(name);
}

std::vector<std::string> FlowEngine::Templates() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [name, f] : templates_) out.push_back(name);
    return out;
}

Result<Flow> FlowEngine::ApplyTemplate(std::string_view templateId, std::string_view newId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = templates_.find(std::string(templateId));
    if (it == templates_.end())
        return Error::Make(Err::Flow_TemplateNotFound, "FlowEngine",
                           "template not found: " + std::string(templateId));
    Flow f = it->second;
    f.id = !std::string(newId).empty() ? std::string(newId) : f.templateId + "-" + f.name;
    f.templateId = it->first;
    return f;
}

Result<void> FlowEngine::RegisterMacro(
    std::string_view name, const std::vector<std::pair<std::string, std::string>>& actions) {
    if (std::string(name).empty() || actions.empty())
        return Error::Make(Err::InvalidArgument, "FlowEngine", "macro needs a name and actions");
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    macros_[std::string(name)] = actions;
    return Ok();
}

Result<void> FlowEngine::RunMacro(std::string_view name,
                                  const std::map<std::string, std::string, std::less<>>& vars) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = macros_.find(std::string(name));
    if (it == macros_.end())
        return Error::Make(Err::Flow_MacroNotFound, "FlowEngine",
                           "macro not found: " + std::string(name));
    std::map<std::string, std::string, std::less<>> variables = vars;
    for (const auto& [type, payload] : it->second) {
        auto aIt = actions_.find(type);
        if (aIt == actions_.end())
            return Error::Make(Err::Flow_ActionNotFound, "FlowEngine",
                               "macro action not registered: " + type);
        FlowNode stub;
        stub.actionType = type;
        stub.actionPayload = payload;
        AutomationContext ctx{&stub, &variables, "macro:" + std::string(name), this};
        auto outcome = aIt->second()->Execute(ctx);
        if (outcome.ok() && outcome.value().kind == ActionOutcome::Kind::Failed)
            return Error::Make(Err::Flow_ActionFailed, "FlowEngine", outcome.value().error);
    }
    return Ok();
}

// ===========================================================================
// Execution (docs/specs/26 §Deterministic Execution)
// ===========================================================================
Result<std::string> FlowEngine::Start(
    std::string_view flowId, const std::map<std::string, std::string, std::less<>>& vars) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = flows_.find(std::string(flowId));
    if (it == flows_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "flow not found: " + std::string(flowId));

    Execution exec;
    exec.executionId = std::format("exec-{}", nextExecutionId_++);
    exec.flowId = it->first;
    exec.order = it->second.nodes;
    exec.variables = it->second.variables;
    for (const auto& [k, v] : vars) exec.variables[k] = v;
    exec.state = FlowState::Running;
    exec.startedMs = NowMs();
    std::string execId = exec.executionId;

    // Trigger (docs/specs/26 §Triggers): an event trigger holds the flow until
    // its topic arrives; manual/time triggers start immediately.
    std::string waitTopic;
    auto tIt = triggers_.find(it->second.trigger.kind);
    if (tIt != triggers_.end())
        waitTopic = tIt->second()->WaitsFor(it->second.trigger.payload);
    else if (it->second.trigger.kind == "event")
        waitTopic = it->second.trigger.payload;
    if (!waitTopic.empty()) {
        exec.waitingFor = waitTopic;   // waitNodeId empty => start trigger
    }

    executions_[execId] = std::move(exec);
    executionCount_.fetch_add(1);
    if (!waitTopic.empty()) return execId;

    auto& stored = executions_[execId];
    (void)EventBus::Instance().Publish(
        events::FlowStarted{std::string(flowId), execId, it->second.name});
    (void)Drain(stored);
    return execId;
}

Result<void> FlowEngine::Pause(std::string_view executionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    if (it->second.state != FlowState::Running)
        return Error::Make(Err::Flow_InvalidState, "FlowEngine", "execution is not running");
    it->second.state = FlowState::Paused;
    (void)EventBus::Instance().Publish(events::FlowPaused{std::string(executionId)});
    return Ok();
}

Result<void> FlowEngine::Resume(std::string_view executionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    if (it->second.state != FlowState::Paused)
        return Error::Make(Err::Flow_InvalidState, "FlowEngine", "execution is not paused");
    it->second.state = FlowState::Running;
    (void)EventBus::Instance().Publish(events::FlowResumed{std::string(executionId)});
    (void)Drain(it->second);
    return Ok();
}

Result<void> FlowEngine::Stop(std::string_view executionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    if (it->second.state != FlowState::Running && it->second.state != FlowState::Paused)
        return Error::Make(Err::Flow_InvalidState, "FlowEngine", "execution is not active");
    it->second.state = FlowState::Stopped;
    it->second.waitingFor.clear();
    (void)EventBus::Instance().Publish(events::FlowStopped{std::string(executionId)});
    (void)EventBus::Instance().Publish(
        events::AutomationCancelled{std::string(executionId), "operator stop"});
    return Ok();
}

Result<std::string> FlowEngine::Restart(std::string_view executionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    std::string flowId = it->second.flowId;
    auto vars = it->second.variables;
    it->second.state = FlowState::Stopped;
    (void)EventBus::Instance().Publish(events::FlowStopped{std::string(executionId)});
    // Reuse the flow's defaults plus the current variable values.
    return Start(flowId, vars);
}

Result<void> FlowEngine::Skip(std::string_view executionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    Execution& exec = it->second;
    if (exec.state != FlowState::Running && exec.state != FlowState::Paused)
        return Error::Make(Err::Flow_InvalidState, "FlowEngine", "execution is not active");
    if (exec.state == FlowState::Paused) exec.state = FlowState::Running;

    if (!exec.waitingFor.empty()) {
        // Skip a pending wait (or the start-trigger wait).
        std::string waited = exec.waitingFor;
        exec.waitingFor.clear();
        exec.waitDeadlineMs = 0;
        if (!exec.waitNodeId.empty()) {
            if (auto* w = const_cast<FlowNode*>(FindNode(exec, exec.waitNodeId))) {
                CompleteNode(exec, *w, NodeState::Skipped, "skipped by operator");
                exec.pc = AdvancePc(exec.order, exec.pc);
            }
            exec.waitNodeId.clear();
        } else {
            // Start-trigger skip == manual start.
            (void)EventBus::Instance().Publish(events::FlowStarted{exec.flowId, exec.executionId, ""});
        }
        (void)Drain(exec);
        return Ok();
    }
    if (exec.pc >= exec.order.size()) return Ok();
    FlowNode& cur = exec.order[exec.pc];
    CompleteNode(exec, cur, NodeState::Skipped, "skipped by operator");
    exec.pc = AdvancePc(exec.order, exec.pc);
    (void)Drain(exec);
    return Ok();
}

Result<void> FlowEngine::JumpTo(std::string_view executionId, std::string_view nodeId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    Execution& exec = it->second;
    if (exec.state == FlowState::Completed || exec.state == FlowState::Stopped ||
        exec.state == FlowState::EmergencyStopped)
        return Error::Make(Err::Flow_InvalidState, "FlowEngine", "execution is finished");
    size_t idx = IndexOf(exec.order, nodeId);
    if (idx >= exec.order.size())
        return Error::Make(Err::Flow_ValidationFailed, "FlowEngine",
                           "no such node: " + std::string(nodeId));
    exec.pc = idx;
    exec.waitingFor.clear();
    exec.waitDeadlineMs = 0;
    exec.waitNodeId.clear();
    if (exec.state == FlowState::Paused) exec.state = FlowState::Running;
    (void)Drain(exec);
    return Ok();
}

Result<void> FlowEngine::Override(std::string_view executionId, std::string_view actionType,
                                  std::string_view payload) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    auto aIt = actions_.find(std::string(actionType));
    if (aIt == actions_.end())
        return Error::Make(Err::Flow_ActionNotFound, "FlowEngine",
                           "action not registered: " + std::string(actionType));
    FlowNode stub;
    stub.id = "override";
    stub.label = "operator override";
    stub.actionType = std::string(actionType);
    stub.actionPayload = std::string(payload);
    AutomationContext ctx{&stub, &it->second.variables, it->first, this};
    auto outcome = aIt->second()->Execute(ctx);
    NodeRecord rec;
    rec.nodeId = "override";
    rec.label = stub.label;
    rec.state = outcome.ok() && outcome.value().kind == ActionOutcome::Kind::Completed
                    ? NodeState::Completed
                    : NodeState::Failed;
    rec.startedMs = rec.endedMs = NowMs();
    rec.result = rec.state == NodeState::Completed
                     ? "ok"
                     : (outcome.ok() ? outcome.value().error : outcome.error().message);
    it->second.history.push_back(std::move(rec));
    return rec.state == NodeState::Completed ? Ok()
                                             : Error::Make(Err::Flow_ActionFailed, "FlowEngine", rec.result);
}

Result<void> FlowEngine::EmergencyStop() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t n = 0;
    for (auto& [id, exec] : executions_) {
        if (exec.state == FlowState::Running || exec.state == FlowState::Paused) {
            exec.state = FlowState::EmergencyStopped;
            exec.waitingFor.clear();
            exec.waitDeadlineMs = 0;
            (void)EventBus::Instance().Publish(events::AutomationEmergencyStopped{id});
            (void)EventBus::Instance().Publish(
                events::FlowInterrupted{id, "emergency stop"});
            ++n;
        }
    }
    Logger::Instance().Warning(std::format("FlowEngine: emergency stop halted {} execution(s)",
                                           n),
                               "FlowEngine");
    return Ok();
}

Result<std::string> FlowEngine::ExecuteAction(
    std::string_view actionType, std::string_view payload,
    const std::map<std::string, std::string, std::less<>>& vars) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto aIt = actions_.find(std::string(actionType));
    if (aIt == actions_.end())
        return Error::Make(Err::Flow_ActionNotFound, "FlowEngine",
                           "action not registered: " + std::string(actionType));
    FlowNode stub;
    stub.id = "manual";
    stub.actionType = std::string(actionType);
    stub.actionPayload = std::string(payload);
    std::map<std::string, std::string, std::less<>> variables = vars;
    AutomationContext ctx{&stub, &variables, "manual", this};
    auto outcome = aIt->second()->Execute(ctx);
    if (!outcome.ok()) return outcome.error();
    if (outcome.value().kind == ActionOutcome::Kind::Failed)
        return Error::Make(Err::Flow_ActionFailed, "FlowEngine", outcome.value().error);
    return std::string("completed");
}

// ===========================================================================
// The deterministic executor core (docs/specs/26 §Deterministic Execution)
// ===========================================================================
void FlowEngine::CompleteNode(Execution& exec, FlowNode& node, NodeState state,
                              const std::string& result) {
    exec.completedNodes.insert(node.id);
    for (auto& rec : exec.history)
        if (rec.nodeId == node.id) {
            rec.state = state;
            rec.endedMs = NowMs();
            rec.result = result;
        }
    switch (state) {
        case NodeState::Completed:
            (void)EventBus::Instance().Publish(events::NodeCompleted{exec.executionId, node.id});
            if (!node.cues.onComplete.empty()) {
                FlowNode cue;
                cue.actionType = node.cues.onComplete;
                AutomationContext ctx{&cue, &exec.variables, exec.executionId, this};
                auto aIt = actions_.find(cue.actionType);
                if (aIt != actions_.end()) (void)aIt->second()->Execute(ctx);
            }
            break;
        case NodeState::Skipped:
            (void)EventBus::Instance().Publish(events::NodeSkipped{exec.executionId, node.id});
            break;
        case NodeState::Failed:
            (void)EventBus::Instance().Publish(
                events::NodeFailed{exec.executionId, node.id, result});
            break;
        case NodeState::Cancelled:
            (void)EventBus::Instance().Publish(
                events::AutomationCancelled{exec.executionId, "node cancelled"});
            break;
        default:
            break;
    }
}

Result<void> FlowEngine::HandleFailure(Execution& exec, FlowNode& node,
                                       const std::string& message) {
    errorCount_.fetch_add(1);
    if (!node.cues.onError.empty()) {
        FlowNode cue;
        cue.actionType = node.cues.onError;
        AutomationContext ctx{&cue, &exec.variables, exec.executionId, this};
        auto aIt = actions_.find(cue.actionType);
        if (aIt != actions_.end()) (void)aIt->second()->Execute(ctx);
    }
    switch (node.onFailure) {
        case FailurePolicy::Retry:
            if (node.retries < node.retryLimit) {
                ++node.retries;   // stay on the same node; the loop re-runs it
                (void)EventBus::Instance().Publish(
                    events::NodeFailed{exec.executionId, node.id, message});
                return Ok();
            }
            CompleteNode(exec, node, NodeState::Failed, "retries exhausted: " + message);
            exec.state = FlowState::Stopped;
            (void)EventBus::Instance().Publish(events::FlowStopped{exec.executionId});
            (void)EventBus::Instance().Publish(
                events::AutomationCancelled{exec.executionId, "retries exhausted: " + message});
            break;
        case FailurePolicy::Skip:
            CompleteNode(exec, node, NodeState::Skipped, message);
            exec.pc = AdvancePc(exec.order, exec.pc);
            break;
        case FailurePolicy::Fallback:
            CompleteNode(exec, node, NodeState::Failed, message);
            if (!node.fallbackNodeId.empty()) exec.pc = IndexOf(exec.order, node.fallbackNodeId);
            else ++exec.pc;
            break;
        case FailurePolicy::Pause:
            CompleteNode(exec, node, NodeState::Failed, message);
            exec.state = FlowState::Paused;
            (void)EventBus::Instance().Publish(events::FlowPaused{exec.executionId});
            break;
        case FailurePolicy::Notify:
            CompleteNode(exec, node, NodeState::Failed, message);
            exec.pc = AdvancePc(exec.order, exec.pc);
            break;
        case FailurePolicy::Abort:
            CompleteNode(exec, node, NodeState::Failed, message);
            exec.state = FlowState::Stopped;
            (void)EventBus::Instance().Publish(events::FlowStopped{exec.executionId});
            (void)EventBus::Instance().Publish(events::AutomationCancelled{exec.executionId, message});
            break;
    }
    return Ok();
}

Result<void> FlowEngine::ExecuteNode(Execution& exec, FlowNode& node) {
    // Cues: beforeStart (then onStart right after the node begins).
    if (!node.cues.beforeStart.empty()) {
        FlowNode cue;
        cue.actionType = node.cues.beforeStart;
        AutomationContext ctx{&cue, &exec.variables, exec.executionId, this};
        auto aIt = actions_.find(cue.actionType);
        if (aIt != actions_.end()) (void)aIt->second()->Execute(ctx);
    }
    NodeRecord rec;
    rec.nodeId = node.id;
    rec.label = node.label;
    rec.state = NodeState::Running;
    rec.startedMs = NowMs();
    exec.history.push_back(std::move(rec));
    nodeCount_.fetch_add(1);
    (void)EventBus::Instance().Publish(events::NodeStarted{exec.executionId, node.id, node.label});
    if (!node.cues.onStart.empty()) {
        FlowNode cue;
        cue.actionType = node.cues.onStart;
        AutomationContext ctx{&cue, &exec.variables, exec.executionId, this};
        auto aIt = actions_.find(cue.actionType);
        if (aIt != actions_.end()) (void)aIt->second()->Execute(ctx);
    }

    switch (node.kind) {
        case NodeKind::Action: {
            auto aIt = actions_.find(node.actionType);
            if (aIt == actions_.end())
                return HandleFailure(exec, node, "unknown action type: " + node.actionType);
            AutomationContext ctx{&node, &exec.variables, exec.executionId, this};
            auto outcome = aIt->second()->Execute(ctx);
            if (!outcome.ok()) return HandleFailure(exec, node, outcome.error().message);
            if (outcome.value().kind == ActionOutcome::Kind::WaitingFor) {
                exec.waitingFor = outcome.value().topic;
                exec.waitNodeId = node.id;
                exec.waitDeadlineMs =
                    node.waitTimeoutMs > 0 ? NowMs() + node.waitTimeoutMs : 0;
                return Ok();
            }
            if (outcome.value().kind == ActionOutcome::Kind::Failed)
                return HandleFailure(exec, node, outcome.value().error);
            CompleteNode(exec, node, NodeState::Completed, "ok");
            exec.pc = AdvancePc(exec.order, exec.pc);
            return Ok();
        }
        case NodeKind::Condition: {
            auto cIt = conditions_.find(node.conditionType);
            if (cIt == conditions_.end())
                return HandleFailure(exec, node, "unknown condition type: " + node.conditionType);
            AutomationContext ctx{&node, &exec.variables, exec.executionId, this};
            exec.activeConditions.push_back(node.conditionType);
            auto r = cIt->second()->Evaluate(ctx);
            if (!r.ok()) return HandleFailure(exec, node, r.error().message);
            CompleteNode(exec, node, NodeState::Completed, r.value() ? "true" : "false");
            size_t target = r.value() ? IndexOf(exec.order, node.trueNodeId)
                                      : IndexOf(exec.order, node.falseNodeId);
            exec.pc = target >= exec.order.size() ? exec.pc + 1 : target;
            return Ok();
        }
        case NodeKind::Parallel: {
            // Burst-execute every member action (docs/specs/26 §Parallel). The
            // first member that starts waiting owns the parallel node's wait.
            size_t maxIdx = exec.pc;
            for (const auto& pid : node.parallelNodeIds) {
                FlowNode* member = const_cast<FlowNode*>(FindNode(exec, pid));
                if (!member) continue;
                size_t mi = IndexOf(exec.order, pid);
                if (mi < exec.order.size()) maxIdx = std::max(maxIdx, mi);
                auto aIt = actions_.find(member->actionType);
                if (aIt == actions_.end()) continue;
                AutomationContext ctx{member, &exec.variables, exec.executionId, this};
                auto outcome = aIt->second()->Execute(ctx);
                if (outcome.ok() && outcome.value().kind == ActionOutcome::Kind::WaitingFor) {
                    if (exec.waitingFor.empty()) {
                        exec.waitingFor = outcome.value().topic;
                        exec.waitNodeId = node.id;
                        exec.waitDeadlineMs = node.waitTimeoutMs > 0 ? NowMs() + node.waitTimeoutMs : 0;
                    }
                }
            }
            // Members were executed inline; skip past them so they are never
            // re-run by linear continuation.
            node.skipAfter = maxIdx + 1 < exec.order.size() ? static_cast<int64_t>(maxIdx + 1)
                                                            : -1;
            if (!exec.waitingFor.empty()) return Ok();   // parallel group waits
            CompleteNode(exec, node, NodeState::Completed, "ok");
            exec.pc = node.skipAfter >= 0 ? static_cast<size_t>(node.skipAfter)
                                          : AdvancePc(exec.order, exec.pc);
            return Ok();
        }
        case NodeKind::Delay: {
            // payload = milliseconds to wait (timeline/delay; deterministic).
            int64_t ms = 0;
            try {
                ms = std::stoll(node.actionPayload.empty() ? "0" : node.actionPayload);
            } catch (...) {
                ms = 0;
            }
            if (ms <= 0) {
                CompleteNode(exec, node, NodeState::Completed, "ok");
                exec.pc = AdvancePc(exec.order, exec.pc);
                return Ok();
            }
            exec.waitingFor = "timer:delay";
            exec.waitNodeId = node.id;
            exec.waitDeadlineMs = NowMs() + ms;
            return Ok();
        }
        case NodeKind::Wait: {
            exec.waitingFor = node.waitFor;
            exec.waitNodeId = node.id;
            exec.waitDeadlineMs = node.waitTimeoutMs > 0 ? NowMs() + node.waitTimeoutMs : 0;
            return Ok();
        }
        case NodeKind::Macro: {
            auto mIt = macros_.find(node.actionType);
            if (mIt == macros_.end())
                return HandleFailure(exec, node, "unknown macro: " + node.actionType);
            for (const auto& [type, payload] : mIt->second) {
                auto aIt = actions_.find(type);
                if (aIt == actions_.end()) continue;
                FlowNode stub;
                stub.id = node.id;
                stub.actionType = type;
                stub.actionPayload = payload;
                AutomationContext ctx{&stub, &exec.variables, exec.executionId, this};
                auto outcome = aIt->second()->Execute(ctx);
                if (outcome.ok() && outcome.value().kind == ActionOutcome::Kind::Failed)
                    return HandleFailure(exec, node, outcome.value().error);
            }
            CompleteNode(exec, node, NodeState::Completed, "ok");
            exec.pc = AdvancePc(exec.order, exec.pc);
            return Ok();
        }
        case NodeKind::End:
            CompleteNode(exec, node, NodeState::Completed, "ok");
            exec.state = FlowState::Completed;   // an End node terminates the flow
            (void)EventBus::Instance().Publish(
                events::FlowCompleted{exec.executionId, exec.history.size()});
            Logger::Instance().Info("Flow completed: " + exec.flowId, "FlowEngine");
            return Ok();
    }
    return Ok();
}

Result<size_t> FlowEngine::Drain(Execution& exec) {
    size_t steps = 0;
    while (exec.state == FlowState::Running) {
        // 1. Pending wait: satisfied by an event, or timed out.
        if (!exec.waitingFor.empty()) {
            if (exec.waitDeadlineMs > 0 && NowMs() >= exec.waitDeadlineMs) {
                bool isDelay = exec.waitingFor.starts_with("timer:");
                std::string topic = exec.waitingFor;
                exec.waitingFor.clear();
                exec.waitDeadlineMs = 0;
                if (exec.waitNodeId.empty()) {
                    exec.waitDeadlineMs = 0;   // start-trigger never auto-times out
                    break;
                }
                FlowNode* wnode = const_cast<FlowNode*>(FindNode(exec, exec.waitNodeId));
                exec.waitNodeId.clear();
                if (wnode) {
                    if (isDelay) {
                        CompleteNode(exec, *wnode, NodeState::Completed, "ok");
                        exec.pc = wnode->skipAfter >= 0 ? static_cast<size_t>(wnode->skipAfter)
                                                         : AdvancePc(exec.order, exec.pc);
                    } else {
                        (void)HandleFailure(exec, *wnode, "wait timeout for " + topic);
                    }
                }
                ++steps;
                continue;
            }
            break;   // still waiting on an event
        }
        // 2. Finished the node list.
        if (exec.pc >= exec.order.size()) {
            exec.state = FlowState::Completed;
            (void)EventBus::Instance().Publish(
                events::FlowCompleted{exec.executionId, exec.history.size()});
            Logger::Instance().Info("Flow completed: " + exec.flowId, "FlowEngine");
            break;
        }
        // 3. Execute the current node.
        FlowNode& node = exec.order[exec.pc];
        auto r = ExecuteNode(exec, node);
        if (!r.ok()) return r.error();
        ++steps;
    }
    ProcessPendingDispatches();
    return steps;
}

void FlowEngine::Dispatch(Execution& exec, std::string_view topic) {
    (void)exec;
    pendingDispatches_.push_back(std::string(topic));
}

Result<void> FlowEngine::RequestDispatch(std::string_view executionId, std::string_view topic) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    pendingDispatches_.push_back(std::string(topic));
    (void)executionId;
    return Ok();
}

void FlowEngine::ProcessPendingDispatches() {
    while (!pendingDispatches_.empty()) {
        std::vector<std::string> batch;
        batch.swap(pendingDispatches_);
        for (const auto& topic : batch) (void)Notify(topic, "");
    }
}

// ===========================================================================
// Event pump (docs/specs/26 §EventBus Integration)
// ===========================================================================
Result<size_t> FlowEngine::Notify(std::string_view eventTopic, std::string_view executionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    size_t satisfied = 0;
    std::vector<std::string> toDrain;
    for (auto& [id, exec] : executions_) {
        if (!executionId.empty() && id != executionId) continue;
        if (exec.state != FlowState::Running || exec.waitingFor != eventTopic) continue;
        exec.waitingFor.clear();
        exec.lastEventMs = NowMs();
        if (exec.waitNodeId.empty()) {
            // Start-trigger wait resolved -> the flow officially starts.
            auto fit = flows_.find(exec.flowId);
            (void)EventBus::Instance().Publish(events::FlowStarted{
                exec.flowId, id, fit != flows_.end() ? fit->second.name : ""});
        } else {
            FlowNode* wnode = const_cast<FlowNode*>(FindNode(exec, exec.waitNodeId));
            exec.waitNodeId.clear();
            exec.waitDeadlineMs = 0;
            if (wnode) {
                CompleteNode(exec, *wnode, NodeState::Completed, "ok");
                exec.pc = wnode->skipAfter >= 0 ? static_cast<size_t>(wnode->skipAfter)
                                                 : AdvancePc(exec.order, exec.pc);
            }
        }
        ++satisfied;
        toDrain.push_back(id);
    }
    for (const auto& id : toDrain) {
        auto it = executions_.find(id);
        if (it != executions_.end()) (void)Drain(it->second);
    }
    return satisfied;
}

Result<void> FlowEngine::Poll(std::string_view executionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& [id, exec] : executions_) {
        if (!executionId.empty() && id != executionId) continue;
        (void)Drain(exec);
    }
    return Ok();
}

// ===========================================================================
// State / history / recovery (docs/specs/26 §Debug Mode, §Recovery)
// ===========================================================================
Result<ExecutionStateView> FlowEngine::ExecutionState(std::string_view executionId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    const Execution& exec = it->second;
    ExecutionStateView view;
    view.state = exec.state;
    view.flowId = exec.flowId;
    view.waitingFor = exec.waitingFor;
    view.startedMs = exec.startedMs;
    if (exec.pc < exec.order.size()) {
        view.currentNodeId = exec.order[exec.pc].id;
        if (exec.pc + 1 < exec.order.size()) view.nextNodeId = exec.order[exec.pc + 1].id;
    }
    view.activeConditions = exec.activeConditions;
    for (const auto& [k, v] : exec.variables) view.variables.push_back(k + "=" + v);
    view.history = exec.history;
    return view;
}

Result<std::vector<NodeRecord>> FlowEngine::History(std::string_view executionId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    return it->second.history;
}

Result<FlowSnapshot> FlowEngine::Snapshot(std::string_view executionId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    const Execution& exec = it->second;
    FlowSnapshot snap;
    snap.executionId = exec.executionId;
    snap.flowId = exec.flowId;
    snap.state = exec.state;
    snap.pc = exec.pc;
    if (exec.pc < exec.order.size()) snap.currentNodeId = exec.order[exec.pc].id;
    snap.waitingFor = exec.waitingFor;
    snap.waitNodeId = exec.waitNodeId;
    snap.waitDeadlineMs = exec.waitDeadlineMs;
    snap.startedMs = exec.startedMs;
    snap.completedNodes.assign(exec.completedNodes.begin(), exec.completedNodes.end());
    snap.variables = exec.variables;
    return snap;
}

Result<std::string> FlowEngine::Recover(const FlowSnapshot& snapshot) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto fit = flows_.find(snapshot.flowId);
    if (fit == flows_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "cannot recover: flow not found: " + snapshot.flowId);
    if (executions_.count(snapshot.executionId))
        return Error::Make(Err::Flow_AlreadyExists, "FlowEngine",
                           "execution already exists: " + snapshot.executionId);
    Execution exec;
    exec.executionId = snapshot.executionId;
    exec.flowId = snapshot.flowId;
    exec.order = fit->second.nodes;
    exec.pc = std::min(snapshot.pc, exec.order.size());
    exec.variables = snapshot.variables;
    exec.state = snapshot.state == FlowState::Paused ? FlowState::Paused : FlowState::Running;
    exec.waitingFor = snapshot.waitingFor;
    exec.waitNodeId = snapshot.waitNodeId;
    exec.waitDeadlineMs = snapshot.waitDeadlineMs;
    exec.startedMs = snapshot.startedMs;
    exec.completedNodes.insert(snapshot.completedNodes.begin(), snapshot.completedNodes.end());
    // Rebuild history from completed nodes for continuity.
    for (size_t i = 0; i < exec.order.size(); ++i) {
        if (i >= snapshot.pc) break;
        NodeRecord rec;
        rec.nodeId = exec.order[i].id;
        rec.label = exec.order[i].label;
        rec.state = NodeState::Completed;
        rec.startedMs = rec.endedMs = snapshot.startedMs;
        rec.result = "ok";
        exec.history.push_back(std::move(rec));
    }
    std::string execId = exec.executionId;
    executions_[execId] = std::move(exec);
    executionCount_.fetch_add(1);
    (void)EventBus::Instance().Publish(events::FlowRecovered{execId, snapshot.currentNodeId});
    if (executions_[execId].state == FlowState::Running) (void)Drain(executions_[execId]);
    return execId;
}

std::vector<std::string> FlowEngine::ActiveExecutions() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [id, exec] : executions_)
        if (exec.state == FlowState::Running || exec.state == FlowState::Paused) out.push_back(id);
    return out;
}

// ===========================================================================
// Variables (docs/specs/26 §Variables, §Data Binding)
// ===========================================================================
Result<void> FlowEngine::SetVariable(std::string_view executionId, std::string_view name,
                                     std::string_view value) {
    if (std::string(name).empty())
        return Error::Make(Err::InvalidArgument, "FlowEngine", "variable name is empty");
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    it->second.variables[std::string(name)] = std::string(value);
    (void)EventBus::Instance().Publish(events::VariableChanged{
        std::string(executionId), std::string(name), std::string(value)});
    return Ok();
}

Result<std::string> FlowEngine::GetVariable(std::string_view executionId,
                                            std::string_view name) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end())
        return Error::Make(Err::Flow_NotFound, "FlowEngine",
                           "execution not found: " + std::string(executionId));
    auto vIt = it->second.variables.find(std::string(name));
    if (vIt == it->second.variables.end())
        return Error::Make(Err::Flow_VariableNotFound, "FlowEngine",
                           "variable not found: " + std::string(name));
    return vIt->second;
}

std::vector<std::string> FlowEngine::Variables(std::string_view executionId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = executions_.find(std::string(executionId));
    if (it == executions_.end()) return {};
    std::vector<std::string> out;
    for (const auto& [k, v] : it->second.variables) out.push_back(k + "=" + v);
    return out;
}

// ===========================================================================
// EventBus wiring (docs/specs/26 §EventBus Integration)
// ===========================================================================
void FlowEngine::WireEvents() {
    if (!subscriptions_.empty()) return;
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::MediaStateChanged>(
        [this](const events::MediaStateChanged& e) { OnMediaStateChanged(e); }, 0));
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::DisplayDeviceConnected>(
        [this](const events::DisplayDeviceConnected& e) { OnDisplayConnected(e); }, 0));
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::DisplayDeviceDisconnected>(
        [this](const events::DisplayDeviceDisconnected& e) { OnDisplayDisconnected(e); }, 0));
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::PresentationCompleted>(
        [this](const events::PresentationCompleted& e) { OnPresentationCompleted(e); }, 0));
    subscriptions_.push_back(EventBus::Instance().Subscribe<events::SceneComposed>(
        [this](const events::SceneComposed& e) { OnSceneComposed(e); }, 0));
}

void FlowEngine::UnwireEvents() {
    for (const auto& s : subscriptions_) (void)EventBus::Instance().Unsubscribe(s);
    subscriptions_.clear();
}

void FlowEngine::OnMediaStateChanged(const events::MediaStateChanged&) {
    (void)Notify(events::MediaStateChanged::kTopic, "");
}

void FlowEngine::OnDisplayConnected(const events::DisplayDeviceConnected&) {
    (void)Notify(events::DisplayDeviceConnected::kTopic, "");
}

void FlowEngine::OnDisplayDisconnected(const events::DisplayDeviceDisconnected&) {
    (void)Notify(events::DisplayDeviceDisconnected::kTopic, "");
}

void FlowEngine::OnPresentationCompleted(const events::PresentationCompleted&) {
    (void)Notify(events::PresentationCompleted::kTopic, "");
}

void FlowEngine::OnSceneComposed(const events::SceneComposed&) {
    (void)Notify(events::SceneComposed::kTopic, "");
}

FlowEngine& FlowEngine::Instance() {
    static FlowEngine instance;
    return instance;
}

} // namespace bps::automation
