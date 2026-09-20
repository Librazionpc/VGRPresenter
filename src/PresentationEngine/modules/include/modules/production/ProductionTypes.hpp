#pragma once

// ProductionTypes (docs/specs/27): the canonical Phase 15 model. Production is
// signals moving through a universal node graph — Sources → Processing →
// Virtual Sources → Buses → Processing → Buses → Outputs — for audio, video,
// data and control independently. This header is pure data; the graph logic
// lives in ProductionGraph and the facade in ProductionEngine.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bps::production {

// Everything is a signal (docs/specs/27 §Core Model).
enum class SignalType { Audio, Video, Data, Control };

// Universal node kinds (docs/specs/27 §Core Model).
enum class NodeKind {
    Source,
    Processor,
    Bus,
    Mixer,
    Scene,
    Output,
    VirtualSource,
    Data,
    Control,
};

// Bus roles (docs/specs/27 §Bus hierarchy + user brief §3).
enum class BusRole { Source, Group, Program, Monitor, Stream, Recording, Custom };

// Channel layouts — never hard-coded to stereo (user brief §16).
enum class ChannelLayout { Mono, Stereo, Surround21, Surround51, Surround71, Multi };

// Output priorities for safe degradation (user brief §12, §41).
enum class OutputPriority { Critical, High, Medium, Low };

// Scene states (user brief §20).
enum class SceneState { Preview, Program, Standby, Disabled, Emergency };

// Source health (user brief §17).
enum class SourceState { Connected, Stable, Degraded, Disconnected, Failed };

// Processing stages on sources, buses and outputs (user brief §9-§11).
enum class ProcessKind {
    Gate,
    Eq,
    Compressor,
    Limiter,
    ColorCorrect,
    Delay,
    Gain,
    Denoise,
    Custom,
};

// A signal travelling the graph (docs/specs/27 §Clock).
struct SignalInfo {
    SignalType type = SignalType::Audio;
    int64_t timestampMs = 0;   // MasterClock time
    int64_t durationMs = 0;
    std::string format;        // e.g. "PCM16", "RGBA", "H264"
    int sampleRate = 48000;    // audio
    double fps = 0.0;          // video
    int width = 0;             // video
    int height = 0;            // video
};

// Declared resource cost of a node (user brief §14).
struct ResourceCost {
    int gpu = 0;     // 0-100
    int cpu = 0;     // 0-100
    int vramMb = 0;
    int ramMb = 0;

    ResourceCost& operator+=(const ResourceCost& o) {
        gpu += o.gpu;
        cpu += o.cpu;
        vramMb += o.vramMb;
        ramMb += o.ramMb;
        return *this;
    }
    ResourceCost operator+(const ResourceCost& o) const {
        ResourceCost c = *this;
        return c += o;
    }
    int Total() const { return gpu + cpu; }
};

struct ProcessingStage {
    ProcessKind kind = ProcessKind::Gain;
    double amount = 1.0;       // 0..1 intensity (gain dB for Gain, seconds for Delay)
    std::string custom;        // for ProcessKind::Custom
};

// Volume control on every node (user brief §12).
struct VolumeControl {
    double gainDb = 0.0;
    bool mute = false;
    bool solo = false;
    double pan = 0.0;      // -1..1 (audio)
    double balance = 0.0;  // -1..1 (video)
};

// Meters: Peak/RMS/LUFS/clipping/headroom/channels (user brief §15).
struct MeterLevels {
    double peak = 0.0;    // dBFS
    double rms = 0.0;
    double lufs = 0.0;
    bool clipping = false;
    double headroom = 0.0;
    std::vector<double> channels;
};

struct OutputCapabilities {
    bool supportsVideo = true;
    bool supportsAudio = true;
    bool supportsSeparateAudio = true;
    bool supportsMultipleChannels = false;
};

// An output is a complete signal destination: video bus + audio bus +
// configuration + health + cost (user brief §9, docs/specs/27 §Outputs).
struct OutputConfig {
    std::string videoBusId;
    std::string audioBusId;
    OutputCapabilities caps;
    OutputPriority priority = OutputPriority::Medium;
    std::string group;                    // e.g. "broadcast", "stage"
    std::vector<std::string> failoverOrder;  // output ids to switch to on failure
    std::string encoder;                  // "hardware" | "software"
    std::string networkTarget;            // e.g. "rtmp://...", "srt://..."
};

// Every node in the graph.
struct NodeInfo {
    std::string id;
    NodeKind kind = NodeKind::Source;
    SignalType signalType = SignalType::Audio;
    BusRole role = BusRole::Custom;   // buses (preserved in bus scenes/snapshots)
    std::string displayName;
    bool enabled = true;
    bool active = false;          // in the current program path
    SourceState sourceState = SourceState::Connected;  // sources
    SceneState sceneState = SceneState::Standby;       // scenes
    ChannelLayout layout = ChannelLayout::Stereo;
    VolumeControl volume;
    std::vector<ProcessingStage> processing;
    ResourceCost cost;
    SignalInfo signal;
    std::string fallbackId;       // source fallback (user brief §18)
    // Free-form metadata carried with the node — used by persistence (the
    // serialized graph round-trips it verbatim) and by higher layers that
    // need to hang policy on a node (e.g. the UI's bus "type" routing
    // policy on bus plane pairs). Never interpreted by the graph itself.
    std::map<std::string, std::string, std::less<>> meta;
};

// A named bus scene (user brief §5, §6): switching scenes re-activates the
// saved input set and configuration without rebuilding the bus.
struct BusSnapshot {
    std::string id;
    std::string name;
    BusRole role = BusRole::Group;
    std::vector<std::string> enabledInputs;
    VolumeControl volume;
    std::vector<ProcessingStage> processing;
    std::vector<std::string> outputs;   // outputs fed by this bus
};

// Full production snapshot (user brief §49).
struct ProductionSnapshot {
    int64_t timestampMs = 0;
    std::string name;
    std::vector<BusSnapshot> buses;
    std::vector<std::pair<std::string, std::string>> outputAssignments;  // output -> bus
    std::map<std::string, std::string, std::less<>> metadata;
};

// Graph validation result (user brief §45).
struct ValidationIssue {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    std::string code;      // "circular_routing" | "missing_node" | ...
    std::string message;
};

// MasterClock: audio/video/network offsets around a master timeline
// (user brief §23, §32).
struct ClockSnapshot {
    int64_t masterMs = 0;
    int64_t audioMs = 0;
    int64_t videoMs = 0;
    int64_t networkMs = 0;
    bool synced = true;
    bool genlockReady = true;   // architecture leaves room for PTP/hardware clock
};

// Production health (user brief §42).
struct ProductionHealth {
    int score = 100;                                  // 0-100
    std::vector<std::pair<std::string, int>> checks;  // subsystem -> score 0-100
    std::string message;
};

// Planner output (user brief §15, §13).
struct ProductionPlan {
    ResourceCost total;
    ResourceCost budget;        // configured budget
    bool feasible = true;
    std::string bottleneck;     // "gpu" | "cpu" | "vram" | "ram"
    std::vector<std::pair<std::string, ResourceCost>> perNode;
    std::string encoder;        // selected encoder
};

// Graph inspector entry (user brief §44).
struct GraphInspectorEntry {
    std::string nodeId;
    NodeKind kind = NodeKind::Source;
    std::string upstream;                 // feeding bus/node
    ResourceCost cost;
    std::vector<std::string> feeds;       // downstream nodes
    bool active = false;
};

// Routing edge.
struct RoutingEdge {
    std::string from;
    std::string to;
    SignalType type = SignalType::Audio;
};

// Small to-string helpers (kept header-only so tests and logging can use them).
inline const char* ToString(SignalType t) {
    switch (t) {
        case SignalType::Audio: return "audio";
        case SignalType::Video: return "video";
        case SignalType::Data: return "data";
        case SignalType::Control: return "control";
    }
    return "?";
}
inline const char* ToString(NodeKind k) {
    switch (k) {
        case NodeKind::Source: return "source";
        case NodeKind::Processor: return "processor";
        case NodeKind::Bus: return "bus";
        case NodeKind::Mixer: return "mixer";
        case NodeKind::Scene: return "scene";
        case NodeKind::Output: return "output";
        case NodeKind::VirtualSource: return "virtual_source";
        case NodeKind::Data: return "data";
        case NodeKind::Control: return "control";
    }
    return "?";
}
inline const char* ToString(BusRole r) {
    switch (r) {
        case BusRole::Source: return "source";
        case BusRole::Group: return "group";
        case BusRole::Program: return "program";
        case BusRole::Monitor: return "monitor";
        case BusRole::Stream: return "stream";
        case BusRole::Recording: return "recording";
        case BusRole::Custom: return "custom";
    }
    return "?";
}
inline const char* ToString(SceneState s) {
    switch (s) {
        case SceneState::Preview: return "preview";
        case SceneState::Program: return "program";
        case SceneState::Standby: return "standby";
        case SceneState::Disabled: return "disabled";
        case SceneState::Emergency: return "emergency";
    }
    return "?";
}
inline const char* ToString(SourceState s) {
    switch (s) {
        case SourceState::Connected: return "connected";
        case SourceState::Stable: return "stable";
        case SourceState::Degraded: return "degraded";
        case SourceState::Disconnected: return "disconnected";
        case SourceState::Failed: return "failed";
    }
    return "?";
}
inline const char* ToString(OutputPriority p) {
    switch (p) {
        case OutputPriority::Critical: return "critical";
        case OutputPriority::High: return "high";
        case OutputPriority::Medium: return "medium";
        case OutputPriority::Low: return "low";
    }
    return "?";
}

} // namespace bps::production
