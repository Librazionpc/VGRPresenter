#pragma once

// RecordingEngine (docs/specs/28): the Phase 16 facade. Recording is a
// first-class output consumer of the Phase 15 production graph — any valid
// signal (source, bus, virtual source, scene, output) can be recorded with its
// own profile, tap point, format and lifecycle. Owns profiles, the recording
// queue, segmentation, markers, metadata, crash recovery (journal), replay
// buffers, capture devices, encoder selection and disk-aware storage. It never
// renders or plays content — it consumes the graph.

#include "core/common/Common.hpp"
#include "core/events/EventBus.hpp"
#include "core/events/Events.hpp"
#include "interfaces/IService.hpp"
#include "modules/recording/RecordingPlugin.hpp"
#include "modules/recording/RecordingTypes.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace bps::recording {

class RecordingEngine final : public IService {
public:
    static RecordingEngine& Instance();

    // --- Lifecycle (engine-wide contract) ---
    Result<void> Initialize() override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Shutdown() override;
    Result<void> Reload() override;
    Result<void> Reset() override;
    HealthReport GetHealth() const override;
    Metrics MetricsSnapshot() const override;
    const char* ServiceName() const noexcept override { return "RecordingEngine"; }

    // --- Profiles (docs/specs/28 §Core Model, §Recording Templates) ---
    Result<std::string> CreateProfile(const RecordingProfile& profile);  // id auto if empty
    Result<RecordingProfile> GetProfile(std::string_view id) const;
    std::vector<std::string> ProfileIds() const;
    Result<void> RemoveProfile(std::string_view id);
    Result<std::string> CreateTemplate(std::string_view name,
                                       const std::vector<std::string>& profileIds);
    std::vector<std::string> TemplateNames() const;

    // --- Encoder selection (docs/specs/28 §Encoder selection) ---
    std::vector<EncoderKind> DetectEncoders() const;
    Result<EncoderKind> SelectEncoder(const RecordingProfile& profile) const;
    Result<void> SetPreferredEncoder(std::string_view profileId, EncoderKind kind);

    // --- Plugin registries (docs/specs/28 §Format Abstraction, §Recording Plugins) ---
    // Containers, codecs and capture devices are registered by plugins — the
    // engine core never hard-codes formats. Software fallbacks are
    // pre-registered at Initialize.
    Result<void> RegisterContainer(std::string_view name, ContainerFactory factory);
    Result<void> RegisterVideoEncoder(EncoderKind kind, VideoEncoderFactory factory);
    Result<void> RegisterAudioEncoder(std::string_view codec, AudioEncoderFactory factory);

    // --- Recording lifecycle (docs/specs/28 §Recording Lifecycle & Queue) ---
    Result<std::string> StartRecording(std::string_view profileId, std::string_view nodeId,
                                       TapPoint tapPoint = TapPoint::PostProcessing);
    Result<void> StopRecording(std::string_view recordingId, std::string_view reason = "");
    Result<void> PauseRecording(std::string_view recordingId);
    Result<void> ResumeRecording(std::string_view recordingId);
    Result<void> AddMarker(std::string_view recordingId, std::string_view label);
    Result<RecordingStateView> GetStatus(std::string_view recordingId) const;
    std::vector<std::string> RecordingIds() const;
    std::vector<RecordingStateView> Recordings(RecordingState state) const;
    Result<void> DeleteRecording(std::string_view recordingId);
    Result<void> ArchiveRecording(std::string_view recordingId);

    // --- Metadata (docs/specs/28 §Metadata) ---
    Result<void> SetMetadata(std::string_view recordingId, std::string_view key,
                             std::string_view value);
    Result<RecordingMetadata> GetMetadata(std::string_view recordingId) const;

    // --- Segmentation + crash recovery (docs/specs/28 §Recovery) ---
    std::vector<JournalEntry> Journal() const;
    Result<size_t> Recover();   // finalizes incomplete recordings; returns count

    // --- Replay / instant replay (docs/specs/28 §Replay) ---
    Result<std::string> CreateReplayBuffer(std::string_view nodeId, int64_t capacityMs);
    // Captures the rolling buffer as a replay and registers it as a virtual
    // video source in the production graph (ReplayCreated).
    Result<std::string> CreateReplay(std::string_view nodeId, ReplayMode mode,
                                     int64_t capacityMs = 30000);
    Result<void> RemoveReplay(std::string_view replayId);
    std::vector<std::string> ReplayIds() const;
    Result<ReplayBuffer> GetReplay(std::string_view replayId) const;

    // --- Capture (docs/specs/28 §Capture) ---
    Result<void> RegisterCaptureSource(CaptureSourceFactory factory);
    std::vector<std::string> CaptureDeviceIds() const;
    // Hot-plug: connect/disconnect a registered capture device. Connecting
    // creates a matching Source node in the production graph.
    Result<void> ConnectCaptureDevice(std::string_view deviceId);
    Result<void> DisconnectCaptureDevice(std::string_view deviceId);
    std::vector<std::string> ConnectedDevices() const;

    // --- Storage (docs/specs/28 §Storage & Resources) ---
    Result<StorageInfo> GetStorageInfo(std::string_view path = "") const;
    Result<void> SetDefaultDirectory(std::string_view dir);
    std::string DefaultDirectory() const;
    Result<void> SetStoragePolicy(int freePercentBelow, StorageAction action);
    // Test hook: override reported free bytes for disk-protection tests.
    void SetSimulatedFreeBytes(int64_t bytes) { simulatedFreeBytes_ = bytes; }

    // --- Scheduling (docs/specs/28 §Scheduling) ---
    Result<void> ScheduleStart(std::string_view profileId, std::string_view nodeId,
                               int64_t atMs);
    Result<void> ScheduleStop(std::string_view recordingId, int64_t atMs);

    // --- Health (docs/specs/28 §Health) ---
    Result<EncoderHealth> GetRecordingHealth(std::string_view recordingId) const;
    Result<void> UpdateEncoderLoad(std::string_view recordingId, int loadPct);
    Result<void> UpdateDroppedFrames(std::string_view recordingId, size_t count);

    // --- Time pump ---
    // Advances the recording clock; drives segments, schedules and the disk
    // monitor. Called by the CLI demo and tests; the scheduler may drive it.
    Result<void> Tick(int64_t advanceMs);
    int64_t ClockMs() const { return clockMs_.load(); }

    // --- Events ---
    void WireEvents();
    void UnwireEvents();

private:
    RecordingEngine() = default;

    struct Session {
        RecordingProfile profile;
        std::string nodeId;
        TapPoint tapPoint = TapPoint::PostProcessing;
        RecordingState state = RecordingState::Preparing;
        size_t segmentIndex = 0;
        int64_t startedAtMs = 0;
        int64_t durationMs = 0;
        int64_t sizeBytes = 0;
        int droppedFrames = 0;
        int encoderLoad = 0;
        std::string filePath;
        std::string encoder;
        std::vector<RecordingMarker> markers;
        RecordingMetadata metadata;
        std::shared_ptr<IRecordingContainer> container;
    };

    void Journal(JournalEntry::Kind kind, std::string_view recordingId,
                 std::string_view detail = "");
    Result<std::string> OpenSegment(std::string_view recordingId);
    void CloseSegment(Session& s, bool finalized);
    void RunDiskMonitor();
    int64_t FreePercent() const;
    int64_t EstimatedBytesPerHour(const RecordingProfile& p) const;
    static std::string MakePath(const RecordingProfile& p, std::string_view base,
                                size_t segment, int64_t stampMs);

    std::map<std::string, RecordingProfile, std::less<>> profiles_;
    std::map<std::string, std::vector<std::string>, std::less<>> templates_;
    std::map<std::string, Session, std::less<>> sessions_;
    std::vector<JournalEntry> journal_;
    std::vector<Subscription> subscriptions_;
    std::map<std::string, ReplayBuffer, std::less<>> replays_;
    std::map<std::string, CaptureSourceFactory, std::less<>> captureFactories_;
    std::set<std::string> connectedDevices_;

    std::map<std::string, ContainerFactory, std::less<>> containers_;
    std::map<std::string, VideoEncoderFactory, std::less<>> videoEncoders_;
    std::map<std::string, AudioEncoderFactory, std::less<>> audioEncoders_;

    std::map<int64_t, std::pair<std::string, std::string>, std::less<>> scheduledStarts_;
    std::map<int64_t, std::string, std::less<>> scheduledStops_;
    std::map<int, StorageAction, std::less<>> storagePolicy_;  // free% below -> action

    std::string defaultDir_;
    int64_t simulatedFreeBytes_ = -1;   // <0 = use real filesystem
    std::atomic<int64_t> clockMs_{0};
    std::atomic<uint64_t> recordingCount_{0};
    std::atomic<uint64_t> replayCount_{0};
    std::atomic<uint64_t> segmentCount_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> errorCount_{0};
    mutable std::recursive_mutex mutex_;
};

} // namespace bps::recording
