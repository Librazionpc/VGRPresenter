#pragma once

// RecordingTypes (docs/specs/28): the canonical Phase 16 model. Recording is a
// first-class output consumer of the Phase 15 production graph — anything that
// is a valid signal (source, bus, virtual source, scene, output) can be
// recorded with its own profile, tap point, format, quality and lifecycle.
// Pure data; logic lives in RecordingEngine.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace bps::recording {

// Recording queue states (user brief §39).
enum class RecordingState { Preparing, Recording, Paused, Finalizing, Completed, Recovering, Failed, Archived };

// Where the recording taps the signal (user brief §8-§10).
enum class TapPoint { PreProcessing, PostProcessing, PreOutput, PostOutput };

// Encoder hardware/software kinds (user brief §13).
enum class EncoderKind { Auto, NVENC, AMF, QuickSync, Software };

// Video codecs (user brief §12).
enum class VideoCodec { H264, HEVC, AV1, VP9, None };
// Audio codecs (user brief §12).
enum class AudioCodec { PCM, AAC, Opus, FLAC, None };

// Container formats (user brief §11).
enum class ContainerKind { MKV, MP4, MOV, WebM, WAV, FLAC };

// Smart quality presets (user brief §14).
enum class QualityPreset { Proxy, Low, Standard, High, Broadcast, Master, Lossless, Custom };

// Replay modes (user brief §28).
enum class ReplayMode { Normal, SlowMotion, Fast, Reverse };

// Recording resource priority (user brief §15) — Recording protects Critical
// (Program) first and sheds Optional first.
enum class RecordingPriority { Critical, High, Medium, Low, Optional };

// Storage policy actions (user brief §19).
enum class StorageAction { None, Warn, StopOptional, StopAll };

// A recording profile: format + quality + resource policy for one recording.
struct RecordingProfile {
    std::string id;
    std::string name;
    ContainerKind container = ContainerKind::MKV;
    VideoCodec videoCodec = VideoCodec::H264;
    AudioCodec audioCodec = AudioCodec::AAC;
    int width = 1920;
    int height = 1080;
    double fps = 60.0;
    int videoBitrateKbps = 12000;
    int audioChannels = 2;
    EncoderKind encoderPreference = EncoderKind::Auto;   // Auto = best available
    QualityPreset quality = QualityPreset::Standard;
    int64_t segmentDurationMs = 0;    // 0 = single segment (no splitting)
    RecordingPriority priority = RecordingPriority::Medium;
    StorageAction storagePolicy = StorageAction::Warn;
    std::string directory;            // output directory ("" = engine default)
    bool multitrack = false;          // per-source tracks (user brief §6)
};

// A marker on a recording timeline (user brief §24).
struct RecordingMarker {
    int64_t timeMs = 0;
    std::string label;
};

// Semantic + technical metadata (user brief §23, §25).
struct RecordingMetadata {
    std::string production;
    std::string date;
    std::string time;
    int64_t durationMs = 0;
    std::string resolution;
    double fps = 0.0;
    std::string videoCodec;
    std::string audioCodec;
    std::vector<std::string> sources;
    std::vector<std::string> scenes;
    std::vector<std::string> outputs;
    std::string bus;
    std::string event;
    std::string speaker;
    std::string song;
    std::string scripture;
    std::map<std::string, std::string, std::less<>> extra;
};

// Live recording state (user brief §40).
struct RecordingStateView {
    std::string recordingId;
    std::string profileId;
    std::string nodeId;            // graph node being recorded
    TapPoint tapPoint = TapPoint::PostProcessing;
    RecordingState state = RecordingState::Preparing;
    size_t segmentIndex = 0;
    int64_t startedAtMs = 0;
    int64_t durationMs = 0;
    int64_t sizeBytes = 0;
    int droppedFrames = 0;
    int encoderLoad = 0;           // 0-100
    bool diskHealthy = true;
    bool audioHealthy = true;
    std::string filePath;
    std::vector<RecordingMarker> markers;
    std::string encoder;           // selected encoder name
};

// Journal entry for crash recovery (user brief §22).
struct JournalEntry {
    enum class Kind { Started, SourceAttached, SegmentCreated, EncoderInitialized,
                      Marker, SegmentClosed, Finalized, Failed };
    Kind kind = Kind::Started;
    int64_t timeMs = 0;
    std::string recordingId;
    std::string detail;
};

// Storage snapshot (user brief §17, §18).
struct StorageInfo {
    int64_t freeBytes = 0;
    int64_t totalBytes = 0;
    int freePercent = 100;
    int64_t estimatedBytesPerHour = 0;
    double capacityHours = 0.0;
    bool healthy = true;
};

// Replay buffer / replay (user brief §27-§30).
struct ReplayBuffer {
    std::string replayId;
    std::string nodeId;            // graph node the buffer watches
    int64_t capacityMs = 0;
    int64_t bufferedMs = 0;
    bool ready = false;
    ReplayMode mode = ReplayMode::Normal;
    std::string virtualSourceId;   // production-graph node when created
};

// Encoder health (user brief §41).
struct EncoderHealth {
    int load = 0;
    size_t droppedFrames = 0;
    int64_t latencyMs = 0;
    size_t queueDepth = 0;
    int bitrateKbps = 0;
};

// Small to-string helpers.
inline const char* ToString(RecordingState s) {
    switch (s) {
        case RecordingState::Preparing: return "preparing";
        case RecordingState::Recording: return "recording";
        case RecordingState::Paused: return "paused";
        case RecordingState::Finalizing: return "finalizing";
        case RecordingState::Completed: return "completed";
        case RecordingState::Recovering: return "recovering";
        case RecordingState::Failed: return "failed";
        case RecordingState::Archived: return "archived";
    }
    return "?";
}
inline const char* ToString(EncoderKind k) {
    switch (k) {
        case EncoderKind::Auto: return "auto";
        case EncoderKind::NVENC: return "nvenc";
        case EncoderKind::AMF: return "amf";
        case EncoderKind::QuickSync: return "quicksync";
        case EncoderKind::Software: return "software";
    }
    return "?";
}
inline const char* ToString(VideoCodec c) {
    switch (c) {
        case VideoCodec::H264: return "H.264";
        case VideoCodec::HEVC: return "HEVC";
        case VideoCodec::AV1: return "AV1";
        case VideoCodec::VP9: return "VP9";
        case VideoCodec::None: return "none";
    }
    return "?";
}
inline const char* ToString(AudioCodec c) {
    switch (c) {
        case AudioCodec::PCM: return "PCM";
        case AudioCodec::AAC: return "AAC";
        case AudioCodec::Opus: return "Opus";
        case AudioCodec::FLAC: return "FLAC";
        case AudioCodec::None: return "none";
    }
    return "?";
}
inline const char* ToString(ContainerKind c) {
    switch (c) {
        case ContainerKind::MKV: return "mkv";
        case ContainerKind::MP4: return "mp4";
        case ContainerKind::MOV: return "mov";
        case ContainerKind::WebM: return "webm";
        case ContainerKind::WAV: return "wav";
        case ContainerKind::FLAC: return "flac";
    }
    return "?";
}
inline const char* ToString(QualityPreset q) {
    switch (q) {
        case QualityPreset::Proxy: return "proxy";
        case QualityPreset::Low: return "low";
        case QualityPreset::Standard: return "standard";
        case QualityPreset::High: return "high";
        case QualityPreset::Broadcast: return "broadcast";
        case QualityPreset::Master: return "master";
        case QualityPreset::Lossless: return "lossless";
        case QualityPreset::Custom: return "custom";
    }
    return "?";
}
inline const char* ToString(ReplayMode m) {
    switch (m) {
        case ReplayMode::Normal: return "normal";
        case ReplayMode::SlowMotion: return "slow_motion";
        case ReplayMode::Fast: return "fast";
        case ReplayMode::Reverse: return "reverse";
    }
    return "?";
}
inline const char* ToString(RecordingPriority p) {
    switch (p) {
        case RecordingPriority::Critical: return "critical";
        case RecordingPriority::High: return "high";
        case RecordingPriority::Medium: return "medium";
        case RecordingPriority::Low: return "low";
        case RecordingPriority::Optional: return "optional";
    }
    return "?";
}
inline const char* ToString(TapPoint t) {
    switch (t) {
        case TapPoint::PreProcessing: return "pre_processing";
        case TapPoint::PostProcessing: return "post_processing";
        case TapPoint::PreOutput: return "pre_output";
        case TapPoint::PostOutput: return "post_output";
    }
    return "?";
}

} // namespace bps::recording
