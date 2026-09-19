#pragma once

// Phase 14 plugin surface (docs/specs/26 §Plugin Support). New action,
// condition, and trigger types plug in through these registries without
// modifying the engine — implement, register, done.

#include "core/common/Common.hpp"
#include "modules/automation/AutomationTypes.hpp"

#include <map>
#include <memory>
#include <string>

namespace bps::automation {

class FlowEngine;

// Runtime context handed to actions/conditions/triggers. Deliberately small:
// variables, the node being executed, and a deferred event-dispatch hook.
struct AutomationContext {
    const FlowNode* node = nullptr;
    std::map<std::string, std::string, std::less<>>* variables = nullptr;
    std::string executionId;
    FlowEngine* engine = nullptr;

    // Substitutes {NAME} tokens from the variable map (docs/specs/26 §Variables).
    std::string Substitute(std::string_view text) const;
};

struct ActionOutcome {
    enum class Kind { Completed, WaitingFor, Failed };
    Kind kind = Kind::Completed;
    std::string topic;   // event topic waited on when kind == WaitingFor
    std::string error;
};

class IAction {
public:
    virtual ~IAction() = default;
    virtual const char* Type() const noexcept = 0;
    // Validate resolves the payload; failures surface at flow load time.
    virtual Result<void> Validate(std::string_view payload) const {
        (void)payload;
        return Ok();
    }
    virtual Result<ActionOutcome> Execute(AutomationContext& ctx) = 0;
};

class ICondition {
public:
    virtual ~ICondition() = default;
    virtual const char* Type() const noexcept = 0;
    virtual Result<bool> Evaluate(AutomationContext& ctx) = 0;
};

class ITrigger {
public:
    virtual ~ITrigger() = default;
    virtual const char* Type() const noexcept = 0;
    // Returns true when this trigger is satisfied right now (manual/time) or
    // the topic it waits for (event triggers: returns the topic).
    virtual std::string WaitsFor(std::string_view payload) const {
        (void)payload;
        return {};
    }
};

using ActionFactory = std::shared_ptr<IAction> (*)();
using ConditionFactory = std::shared_ptr<ICondition> (*)();
using TriggerFactory = std::shared_ptr<ITrigger> (*)();

} // namespace bps::automation
