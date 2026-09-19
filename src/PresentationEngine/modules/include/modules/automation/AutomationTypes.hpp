#pragma once

// Service Flow, Playlist & Automation Engine (Phase 14, docs/specs/26).
// Canonical model: a Flow is an ordered, executable graph of nodes with
// triggers, conditions, actions, waits, branches, parallel groups, macros,
// templates, and variables. The engine is deliberately generic —
// Trigger → Condition → Action → Event → Next Action — so it never knows
// whether a node controls a song, a video, MIDI, or a camera.

#include "core/common/Common.hpp"

#include <map>
#include <string>
#include <vector>

namespace bps::automation {

enum class FlowState {
    Idle,
    Validating,
    Running,
    Paused,
    Stopped,
    EmergencyStopped,
    Completed,
};

enum class NodeState { Pending, Running, Completed, Failed, Skipped, Cancelled };

enum class NodeKind { Action, Condition, Parallel, Delay, Wait, Macro, End };

enum class FailurePolicy { Retry, Skip, Fallback, Pause, Notify, Abort };

// Cues (docs/specs/26 §Cue System): action types attached to node lifecycle.
struct FlowCues {
    std::string beforeStart;   // runs before the node body
    std::string onStart;       // runs when the node starts
    std::string during;        // informational only (recorded)
    std::string onComplete;    // runs after the node completes
    std::string onError;       // runs if the node fails
    std::string onCancel;      // runs if the node is cancelled
};

// A single executable step (docs/specs/26 §Flow Nodes).
struct FlowNode {
    std::string id;
    std::string label;
    NodeKind kind = NodeKind::Action;

    // Action / condition references through the plugin registries.
    std::string actionType;      // e.g. "noop", "delay", "send_event", "macro"
    std::string actionPayload;   // engine-neutral payload (may contain {VARS})
    std::string conditionType;   // e.g. "variable_equals"
    std::string conditionPayload;

    // Wait / synchronization (docs/specs/26 §Synchronization).
    std::string waitFor;             // event topic this node waits on
    int64_t waitTimeoutMs = 0;       // 0 = wait indefinitely

    // Timeline (docs/specs/26 §Timeline) — offset from flow start; the
    // engine enforces ordering deterministically.
    int64_t timeOffsetMs = 0;

    // Branching (docs/specs/26 §Branching).
    std::string trueNodeId;
    std::string falseNodeId;
    // Cue-chain: explicit next node when the target of this node is not the
    // linear successor (branch paths rejoin via nextNodeId = the join cue).
    std::string nextNodeId;

    // Parallel group (docs/specs/26 §Parallel Execution).
    std::vector<std::string> parallelNodeIds;

    // Failure policy (docs/specs/26 §Failure handling).
    FailurePolicy onFailure = FailurePolicy::Pause;
    std::string fallbackNodeId;   // for Fallback policy
    int retryLimit = 0;
    int retries = 0;              // runtime counter

    // Cues (docs/specs/26 §Cue System): action types attached to the node's
    // lifecycle. Strings name registered action types (empty = no cue).
    FlowCues cues;

    // Runtime-only: for parallel nodes, the linear index past the member set
    // (members run inline and must never be re-run by linear continuation).
    int64_t skipAfter = -1;
};

struct FlowTrigger {
    std::string kind = "manual";   // "manual" | "event" | "time" | plugin kinds
    std::string payload;           // event topic for "event" triggers
};

struct Flow {
    std::string id;
    std::string name;
    std::string version = "1.0";
    std::string templateId;
    FlowTrigger trigger;
    std::vector<FlowNode> nodes;
    std::map<std::string, std::string, std::less<>> variables;  // defaults
    std::map<std::string, std::string, std::less<>> metadata;
};

// One line in the execution history (docs/specs/26 §Execution History).
struct NodeRecord {
    std::string nodeId;
    std::string label;
    NodeState state = NodeState::Pending;
    int64_t startedMs = 0;
    int64_t endedMs = 0;
    std::string result;   // "ok" or the error message
};

// A live view of an execution (docs/specs/26 §Debug Mode).
struct ExecutionStateView {
    FlowState state = FlowState::Idle;
    std::string flowId;
    std::string currentNodeId;
    std::string nextNodeId;
    std::string waitingFor;
    int64_t startedMs = 0;
    std::vector<std::string> activeConditions;
    std::vector<std::string> variables;   // "NAME=value"
    std::vector<NodeRecord> history;
};

// Crash recovery payload (docs/specs/26 §Recovery).
struct FlowSnapshot {
    std::string executionId;
    std::string flowId;
    FlowState state = FlowState::Idle;
    size_t pc = 0;
    std::string currentNodeId;
    std::string waitingFor;
    std::string waitNodeId;
    int64_t waitDeadlineMs = 0;
    int64_t startedMs = 0;
    std::vector<std::string> completedNodes;
    std::map<std::string, std::string, std::less<>> variables;
};

} // namespace bps::automation
