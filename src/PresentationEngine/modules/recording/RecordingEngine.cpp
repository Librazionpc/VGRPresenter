#include "modules/recording/RecordingEngine.hpp"

#include "core/events/EventBus.hpp"
#include "core/logging/Logger.hpp"
#include "modules/audio/AudioEngine.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/recording/AudioFileContainer.hpp"
#include "modules/recording/MatroskaContainer.hpp"
#include "modules/recording/MediaFoundationMp4.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderOutputs.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <thread>

namespace bps::recording {
struct RecordingEngine::Pump {
    struct AudioBlock {
        std::vector<float> samples;
        uint32_t channels = 0;
        uint32_t rateHz = 0;
        double timestampMs = 0.0;
    };

    std::string recordingId;
    std::string bufferName;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    uint64_t audioTap = 0;
    uint64_t audioOrigin = 0;           // the mix's sample position when the recording began
    uint64_t lastFrame = 0;             // the buffer's frame count already written
    std::atomic<bool> stop{false};
    std::thread thread;
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::mutex writeMutex;              // one writer at a time: the pump, a rotation, a stop
    std::mutex audioMutex;              // guards audio
    std::deque<AudioBlock> audio;

    ~Pump() {
        if (!thread.joinable())
            return;
        // The last reference can drop on the pump's own thread (a failed recording).
        if (thread.get_id() == std::this_thread::get_id())
            thread.detach();
        else
            thread.join();
    }
};


namespace {

constexpr const char* kModule = "RecordingEngine";

// The Media Foundation encoders the MP4 container writes with. Initialize succeeds
// only when Media Foundation has the encoder; the encoder selected is the one the
// system provides (hardware where present, the Microsoft software encoder otherwise).
class MediaFoundationVideoEncoder final : public IVideoEncoder {
public:
    const char* Codec() const noexcept override { return "H.264"; }
    EncoderKind Kind() const noexcept override { return EncoderKind::Software; }
    Result<void> Initialize(const RecordingProfile&) override {
        auto probe = ProbeH264Encoder();
        if (!probe.ok()) return probe;
        initialized_ = true;
        return Ok();
    }
    Result<void> Shutdown() override { initialized_ = false; return Ok(); }
    bool Initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

class MediaFoundationAudioEncoder final : public IAudioEncoder {
public:
    const char* Codec() const noexcept override { return "AAC"; }
    Result<void> Initialize(const RecordingProfile&) override {
        auto probe = ProbeAacEncoder();
        if (!probe.ok()) return probe;
        initialized_ = true;
        return Ok();
    }
    Result<void> Shutdown() override { initialized_ = false; return Ok(); }
    bool Initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};


const char* EncoderKindName(EncoderKind k) {
    switch (k) {
        case EncoderKind::NVENC: return "nvenc";
        case EncoderKind::AMF: return "amf";
        case EncoderKind::QuickSync: return "quicksync";
        case EncoderKind::Software: return "software";
        case EncoderKind::Auto: return "auto";
    }
    return "auto";
}

const char* ContainerName(ContainerKind c) {
    switch (c) {
        case ContainerKind::MKV: return "mkv";
        case ContainerKind::MP4: return "mp4";
        case ContainerKind::MOV: return "mov";
        case ContainerKind::WebM: return "webm";
        case ContainerKind::WAV: return "wav";
        case ContainerKind::FLAC: return "flac";
        case ContainerKind::M4A: return "m4a";
        case ContainerKind::MP3: return "mp3";
    }
    return "mkv";
}

const char* StateName(RecordingState s) { return ToString(s); }

std::string Sanitize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '\t') out += '_';
        else out += c;
    }
    return out;
}

// Storage policy severity order (docs/specs/28 §19).
int SeverityRank(StorageAction a) {
    switch (a) {
        case StorageAction::None: return 0;
        case StorageAction::Warn: return 1;
        case StorageAction::StopOptional: return 2;
        case StorageAction::StopAll: return 3;
    }
    return 0;
}

} // namespace

RecordingEngine& RecordingEngine::Instance() {
    static RecordingEngine inst;
    return inst;
}

// --- Lifecycle ---

Result<void> RecordingEngine::Initialize() {
    std::string dir;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (initialized_.load()) return Ok();
        if (defaultDir_.empty()) defaultDir_ = "recordings";
        dir = defaultDir_;
    }
    // Ensure the recording directory exists (docs/specs/28 §18); failures are
    // non-fatal — individual recordings report their own storage errors. This
    // is a PAL call, so it runs with mutex_ released.
    { std::error_code ec; (void)std::filesystem::create_directories(dir, ec); }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (initialized_.load()) return Ok();   // lost the race; other caller finished

    // Default providers (docs/specs/28 §11-§13). MP4 (Media Foundation) and MKV (the
    // Matroska writer) are the containers this build writes; any other format is not
    // registered, so a profile that asks for one is refused rather than silently written
    // as something else. The hardware encoders are registered by the host through
    // RegisterHardwareEncoders — only the ones the machine really has.
    containers_.emplace(ContainerName(ContainerKind::MP4),
                        []() -> std::shared_ptr<IRecordingContainer> {
                            return std::make_shared<MediaFoundationMp4>();
                        });
    containers_.emplace(ContainerName(ContainerKind::MKV),
                        []() -> std::shared_ptr<IRecordingContainer> {
                            return std::make_shared<MatroskaContainer>();
                        });
    // The audio-only formats an Audio preset records to. MP3 only where the machine has an
    // MP3 encoder, so the list a preset can pick from is the list that works.
    for (const ContainerKind audioKind : {ContainerKind::WAV, ContainerKind::M4A, ContainerKind::MP3}) {
        if (audioKind == ContainerKind::MP3 && !Mp3EncoderAvailable()) continue;
        containers_.emplace(ContainerName(audioKind),
                            [audioKind]() -> std::shared_ptr<IRecordingContainer> {
                                return std::make_shared<AudioFileContainer>(audioKind);
                            });
    }
    videoEncoders_.emplace(EncoderKindName(EncoderKind::Software),
                           []() -> std::shared_ptr<IVideoEncoder> {
                               return std::make_shared<MediaFoundationVideoEncoder>();
                           });
    audioEncoders_.emplace("AAC", []() -> std::shared_ptr<IAudioEncoder> {
        return std::make_shared<MediaFoundationAudioEncoder>();
    });

    // Default master profile (docs/specs/28 §2: Output Recording Profiles).
    if (!profiles_.count("master")) {
        RecordingProfile master;
        master.id = "master";
        master.name = "Master Recording";
        master.container = ContainerKind::MP4;
        master.videoCodec = VideoCodec::H264;
        master.audioCodec = AudioCodec::AAC;
        master.segmentDurationMs = 0;  // single segment by default
        profiles_.emplace(master.id, std::move(master));
    }

    // Default storage policy (docs/specs/28 §19): warn below 20%, stop
    // optional below 10%, protect production below 5%.
    storagePolicy_[20] = StorageAction::Warn;
    storagePolicy_[10] = StorageAction::StopOptional;
    storagePolicy_[5] = StorageAction::StopAll;

    initialized_.store(true);
    WireEvents();
    return Ok();
}

Result<void> RecordingEngine::Start() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_.load()) return Error::Make(Err::Recording_InvalidState, kModule,
                                                 "Initialize() first");
    if (running_.load()) return Ok();
    running_.store(true);
    return Ok();
}

Result<void> RecordingEngine::Stop() {
    std::vector<std::string> active;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!running_.load()) return Ok();
        // Stop all active recordings (docs/specs/28 §Recording Lifecycle).
        for (const auto& [id, s] : sessions_)
            if (s.state == RecordingState::Recording || s.state == RecordingState::Paused)
                active.push_back(id);
        running_.store(false);
    }
    // Each stop finalizes a provider container — call it with NO lock held.
    for (const auto& id : active) (void)StopRecording(id, "engine stop");
    return Ok();
}

Result<void> RecordingEngine::Shutdown() {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!initialized_.load()) return Ok();
    }
    // Stop() finalizes containers; give it a lock-free window.
    (void)Stop();
    StopAllPumps();
    // Bus work (unsubscribing), so it runs with NO engine lock held: a
    // subscriber that re-enters this engine would otherwise wait on mutex_.
    UnwireEvents();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sessions_.clear();
    replays_.clear();
    connectedDevices_.clear();
    scheduledStarts_.clear();
    scheduledStops_.clear();
    journal_.clear();
    profiles_.clear();
    templates_.clear();
    containers_.clear();
    videoEncoders_.clear();
    audioEncoders_.clear();
    captureFactories_.clear();
    initialized_.store(false);
    return Ok();
}

Result<void> RecordingEngine::Reload() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_.load()) return Error::Make(Err::Recording_InvalidState, kModule,
                                                 "Initialize() first");
    // Profiles/templates are the persistent contract; sessions re-resolve.
    return Ok();
}

Result<void> RecordingEngine::Reset() {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!initialized_.load()) return Ok();
    }
    (void)Stop();
    StopAllPumps();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sessions_.clear();
    replays_.clear();
    connectedDevices_.clear();
    scheduledStarts_.clear();
    scheduledStops_.clear();
    journal_.clear();
    clockMs_.store(0);
    recordingCount_.store(0);
    replayCount_.store(0);
    segmentCount_.store(0);
    errorCount_.store(0);
    return Ok();
}

HealthReport RecordingEngine::GetHealth() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    HealthReport hr;
    if (errorCount_.load() > 0) {
        hr.state = HealthState::Degraded;
        hr.detail = std::format("{} recording error(s)", errorCount_.load());
        hr.errorCount = errorCount_.load();
    } else if (!running_.load()) {
        hr.detail = "initialized, not running";
    } else {
        size_t active = 0;
        for (const auto& [id, s] : sessions_)
            if (s.state == RecordingState::Recording) ++active;
        hr.detail = std::format("{} active of {} recording(s)", active, sessions_.size());
    }
    return hr;
}

Metrics RecordingEngine::MetricsSnapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Metrics m;
    m.errorCount = errorCount_.load();
    m.health = GetHealth().state;
    m.queueLength = sessions_.size();
    return m;
}

// --- Profiles ---

Result<std::string> RecordingEngine::CreateProfile(const RecordingProfile& profile) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!initialized_.load()) return Error::Make(Err::Recording_InvalidState, kModule,
                                                 "Initialize() first");
    // Validate (docs/specs/28 §Recording Profiles).
    if (profile.width <= 0 || profile.height <= 0)
        return Error::Make(Err::Recording_ValidationFailed, kModule, "invalid resolution");
    if (profile.fps <= 0.0)
        return Error::Make(Err::Recording_ValidationFailed, kModule, "invalid fps");
    if (profile.videoBitrateKbps <= 0)
        return Error::Make(Err::Recording_ValidationFailed, kModule, "invalid bitrate");
    if (profile.audioChannels <= 0)
        return Error::Make(Err::Recording_ValidationFailed, kModule, "invalid audio channels");
    if (profile.segmentDurationMs < 0)
        return Error::Make(Err::Recording_ValidationFailed, kModule, "invalid segment duration");
    if (containers_.count(ContainerName(profile.container)) == 0)
        return Error::Make(Err::Recording_ContainerNotFound, kModule,
                           "container not registered: " + std::string(ContainerName(profile.container)));

    std::string id = profile.id.empty()
                         ? std::format("profile_{}", profiles_.size() + 1)
                         : profile.id;
    if (profiles_.count(id))
        return Error::Make(Err::Recording_AlreadyExists, kModule, "profile already exists");
    RecordingProfile p = profile;
    p.id = id;
    if (p.directory.empty()) p.directory = defaultDir_;
    profiles_.emplace(id, std::move(p));
    return id;
}

Result<RecordingProfile> RecordingEngine::GetProfile(std::string_view id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = profiles_.find(id);
    if (it == profiles_.end())
        return Error::Make(Err::Recording_ProfileNotFound, kModule, "profile not found");
    return it->second;
}

std::vector<std::string> RecordingEngine::ProfileIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(profiles_.size());
    for (const auto& [id, p] : profiles_) out.push_back(id);
    return out;
}

Result<void> RecordingEngine::RemoveProfile(std::string_view id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = profiles_.find(id);
    if (it == profiles_.end())
        return Error::Make(Err::Recording_ProfileNotFound, kModule, "profile not found");
    profiles_.erase(it);
    return Ok();
}

Result<std::string> RecordingEngine::CreateTemplate(std::string_view name,
                                                    const std::vector<std::string>& profileIds) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string key(name);
    if (templates_.count(key))
        return Error::Make(Err::Recording_AlreadyExists, kModule, "template already exists");
    for (const auto& pid : profileIds)
        if (!profiles_.count(pid))
            return Error::Make(Err::Recording_ProfileNotFound, kModule, "template references missing profile");
    templates_.emplace(key, profileIds);
    return key;
}

std::vector<std::string> RecordingEngine::TemplateNames() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(templates_.size());
    for (const auto& [n, ids] : templates_) out.push_back(n);
    return out;
}

// --- Encoder selection ---

std::vector<std::string> RecordingEngine::ContainerNames() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& entry : containers_) out.push_back(entry.first);
    return out;
}

std::vector<EncoderKind> RecordingEngine::DetectEncoders() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<EncoderKind> out;
    if (videoEncoders_.count("nvenc")) out.push_back(EncoderKind::NVENC);
    if (videoEncoders_.count("amf")) out.push_back(EncoderKind::AMF);
    if (videoEncoders_.count("quicksync")) out.push_back(EncoderKind::QuickSync);
    if (videoEncoders_.count("software")) out.push_back(EncoderKind::Software);
    return out;
}

Result<EncoderKind> RecordingEngine::SelectEncoder(const RecordingProfile& profile) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Hardware first, software fallback (docs/specs/28 §13).
    if (profile.encoderPreference != EncoderKind::Auto) {
        if (videoEncoders_.count(EncoderKindName(profile.encoderPreference)))
            return profile.encoderPreference;
        return Error::Make(Err::Recording_EncoderUnavailable, kModule,
                           "preferred encoder unavailable: " +
                               std::string(EncoderKindName(profile.encoderPreference)));
    }
    if (videoEncoders_.count("nvenc")) return EncoderKind::NVENC;
    if (videoEncoders_.count("amf")) return EncoderKind::AMF;
    if (videoEncoders_.count("quicksync")) return EncoderKind::QuickSync;
    if (videoEncoders_.count("software")) return EncoderKind::Software;
    return Error::Make(Err::Recording_EncoderUnavailable, kModule, "no video encoder registered");
}

Result<void> RecordingEngine::SetPreferredEncoder(std::string_view profileId, EncoderKind kind) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = profiles_.find(profileId);
    if (it == profiles_.end())
        return Error::Make(Err::Recording_ProfileNotFound, kModule, "profile not found");
    if (kind != EncoderKind::Auto && !videoEncoders_.count(EncoderKindName(kind)))
        return Error::Make(Err::Recording_EncoderUnavailable, kModule,
                           "encoder not registered: " + std::string(EncoderKindName(kind)));
    it->second.encoderPreference = kind;
    return Ok();
}

// --- Plugin registries ---

Result<void> RecordingEngine::RegisterContainer(std::string_view name, ContainerFactory factory) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!factory) return Error::Make(Err::InvalidArgument, kModule, "null container factory");
    std::string key(name);
    if (containers_.count(key))
        return Error::Make(Err::Recording_AlreadyExists, kModule, "container already registered");
    containers_.emplace(key, std::move(factory));
    return Ok();
}

Result<void> RecordingEngine::RegisterVideoEncoder(EncoderKind kind, VideoEncoderFactory factory) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!factory) return Error::Make(Err::InvalidArgument, kModule, "null video encoder factory");
    const std::string key = EncoderKindName(kind);
    if (videoEncoders_.count(key))
        return Error::Make(Err::Recording_AlreadyExists, kModule, "encoder already registered");
    videoEncoders_.emplace(key, std::move(factory));
    return Ok();
}

Result<void> RecordingEngine::RegisterAudioEncoder(std::string_view codec, AudioEncoderFactory factory) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!factory) return Error::Make(Err::InvalidArgument, kModule, "null audio encoder factory");
    std::string key(codec);
    if (audioEncoders_.count(key))
        return Error::Make(Err::Recording_AlreadyExists, kModule, "audio encoder already registered");
    audioEncoders_.emplace(key, std::move(factory));
    return Ok();
}

// --- Recording lifecycle ---

Result<std::string> RecordingEngine::StartRecording(std::string_view profileId,
                                                    std::string_view nodeId,
                                                    TapPoint tapPoint) {
    // Disk check first (docs/specs/28 §17-§19): FreePercent() stats the
    // filesystem, a PAL call that must run with no engine lock held.
    if (FreePercent() < 5)
        return Error::Make(Err::Recording_DiskFull, kModule,
                           "free disk space below emergency threshold");

    RecordingProfile profile;
    ContainerFactory factory;
    std::string rid;
    std::string encoderName;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!initialized_.load())
            return Error::Make(Err::Recording_InvalidState, kModule, "Initialize() first");
        if (!running_.load()) return Error::Make(Err::Recording_InvalidState, kModule,
                                                 "Start() first");
        auto pit = profiles_.find(profileId);
        if (pit == profiles_.end())
            return Error::Make(Err::Recording_ProfileNotFound, kModule, "profile not found");

        // Recording taps the production graph — the node must exist there.
        // (docs/specs/28 §Architecture: recording consumes the Phase 15 graph.)
        auto& graph = production::ProductionEngine::Instance().Graph();   // lock-ok: Graph() takes no lock; HasNode is an in-memory read
        if (!graph.HasNode(nodeId))
            return Error::Make(Err::Recording_NodeNotFound, kModule,
                               "production node not found: " + std::string(nodeId));

        // One active recording per node.
        for (const auto& [id, s] : sessions_)
            if (s.nodeId == nodeId && (s.state == RecordingState::Recording ||
                                       s.state == RecordingState::Paused ||
                                       s.state == RecordingState::Preparing))
                return Error::Make(Err::Recording_AlreadyActive, kModule,
                                   "node already being recorded");

        // Encoder availability before we commit to a session.
        auto enc = SelectEncoder(pit->second);
        if (!enc.ok()) return enc.error();

        auto cit = containers_.find(ContainerName(pit->second.container));
        if (cit == containers_.end())
            return Error::Make(Err::Recording_ContainerNotFound, kModule,
                               "container not registered: " +
                                   std::string(ContainerName(pit->second.container)));
        factory = cit->second;
        profile = pit->second;
        encoderName = EncoderKindName(enc.value());

        rid = std::format("rec_{}", recordingCount_.fetch_add(1));
        Session s;
        s.profile = profile;
        s.nodeId = std::string(nodeId);
        s.tapPoint = tapPoint;
        s.state = RecordingState::Preparing;
        s.startedAtMs = clockMs_.load();
        s.encoder = encoderName;
        sessions_.emplace(rid, std::move(s));
        Journal(JournalEntry::Kind::Started, rid, std::string(nodeId));
        Journal(JournalEntry::Kind::SourceAttached, rid, std::string(nodeId));
        Journal(JournalEntry::Kind::EncoderInitialized, rid, encoderName);
    }

    // The first segment's container is created and opened with NO lock held:
    // a real encoder's Open() can block for a long time (docs/specs/28 §20).
    auto container = factory();
    if (!container) {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (auto it = sessions_.find(rid); it != sessions_.end()) {
                errorCount_.fetch_add(1);
                it->second.state = RecordingState::Failed;
                Journal(JournalEntry::Kind::Failed, rid, "container factory produced nothing");
            }
        }
        // Dispatched with mutex_ released: a subscriber is arbitrary code.
        (void)EventBus::Instance().Publish(events::RecordingFailed{
            rid, "container factory produced nothing"});
        return Error::Make(Err::Recording_ContainerNotFound, kModule,
                           "container factory produced nothing");
    }

    std::string path;
    size_t seg = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(rid);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        seg = it->second.segmentIndex + 1;
        path = MakePath(profile, defaultDir_, seg, clockMs_.load());
    }

    auto opened = container->Open(profile, path);
    const std::string openedPath = opened.ok() ? opened.value() : std::string();
    bool orphaned = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(rid);
        if (it == sessions_.end()) {
            orphaned = true;   // provider finalized outside the lock, below
        } else if (!opened.ok()) {
            errorCount_.fetch_add(1);
            it->second.state = RecordingState::Failed;
            Journal(JournalEntry::Kind::Failed, rid, opened.error().message);
        } else {
            Session& s = it->second;
            s.container = container;
            s.filePath = openedPath;
            s.segmentIndex = seg;
            s.state = RecordingState::Recording;
            Journal(JournalEntry::Kind::SegmentCreated, rid, openedPath);
            segmentCount_.fetch_add(1);
        }
    }
    if (orphaned) {
        // The session vanished while the provider was opening it: finalize the
        // container with no engine lock held.
        (void)container->Close(false);
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    }
    if (!opened.ok()) {
        (void)EventBus::Instance().Publish(
            events::RecordingFailed{rid, opened.error().message});
        return opened.error();
    }
    StartPump(rid);
    (void)EventBus::Instance().Publish(events::SegmentCreated{rid, seg, openedPath});
    (void)EventBus::Instance().Publish(
        events::RecordingStarted{rid, std::string(profileId), std::string(nodeId)});
    return rid;
}

Result<void> RecordingEngine::StopRecording(std::string_view recordingId,
                                            std::string_view reason) {
    std::shared_ptr<Pump> pump;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        Session& s = it->second;
        if (s.state != RecordingState::Recording && s.state != RecordingState::Paused)
            return Error::Make(Err::Recording_NotActive, kModule,
                               "recording is not active (state: " +
                                   std::string(StateName(s.state)) + ")");
        s.state = RecordingState::Finalizing;
        pump = std::move(s.pump);
        s.pump.reset();
    }
    // The pump writes into the container, so it stops before the container closes.
    StopPump(pump);

    std::shared_ptr<IRecordingContainer> container;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        // Detach the container so it can be finalized with no lock held.
        container = std::move(it->second.container);
        it->second.container.reset();
    }
    // Finalize the provider container with NO lock held.
    if (container) (void)container->Close(true);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        it->second.state = RecordingState::Completed;
        Journal(JournalEntry::Kind::Finalized, std::string(recordingId),
                std::string(reason.empty() ? "manual" : reason));
    }
    (void)EventBus::Instance().Publish(
        events::RecordingStopped{std::string(recordingId), std::string(reason)});
    return Ok();
}

Result<void> RecordingEngine::PauseRecording(std::string_view recordingId) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        if (it->second.state != RecordingState::Recording)
            return Error::Make(Err::Recording_InvalidState, kModule,
                               "only a recording session can pause");
        it->second.state = RecordingState::Paused;
    }
    (void)EventBus::Instance().Publish(events::RecordingPaused{std::string(recordingId)});
    return Ok();
}

Result<void> RecordingEngine::ResumeRecording(std::string_view recordingId) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        if (it->second.state != RecordingState::Paused)
            return Error::Make(Err::Recording_InvalidState, kModule,
                               "only a paused session can resume");
        it->second.state = RecordingState::Recording;
    }
    (void)EventBus::Instance().Publish(events::RecordingResumed{std::string(recordingId)});
    return Ok();
}

Result<void> RecordingEngine::AddMarker(std::string_view recordingId,
                                        std::string_view label) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sessions_.find(recordingId);
    if (it == sessions_.end())
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    RecordingMarker m;
    m.timeMs = clockMs_.load() - it->second.startedAtMs;
    m.label = std::string(label);
    it->second.markers.push_back(m);
    Journal(JournalEntry::Kind::Marker, std::string(recordingId), m.label);
    return Ok();
}

Result<RecordingStateView> RecordingEngine::GetStatus(std::string_view recordingId) const {
    // Query storage BEFORE taking the lock (FreePercent may touch the PAL).
    const bool diskHealthy = FreePercent() >= 10;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sessions_.find(recordingId);
    if (it == sessions_.end())
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    const auto& s = it->second;
    RecordingStateView v;
    v.recordingId = std::string(recordingId);
    v.profileId = s.profile.id;
    v.nodeId = s.nodeId;
    v.tapPoint = s.tapPoint;
    v.state = s.state;
    v.segmentIndex = s.segmentIndex;
    v.startedAtMs = s.startedAtMs;
    v.durationMs = s.durationMs;
    v.sizeBytes = s.sizeBytes;
    v.droppedFrames = s.droppedFrames;
    v.encoderLoad = s.encoderLoad;
    v.diskHealthy = diskHealthy;
    v.audioHealthy = true;
    v.filePath = s.filePath;
    v.markers = s.markers;
    v.encoder = s.encoder;
    return v;
}

std::vector<std::string> RecordingEngine::RecordingIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_) out.push_back(id);
    return out;
}

std::vector<RecordingStateView> RecordingEngine::Recordings(RecordingState state) const {
    // Collect ids under the lock, then build each view outside it: GetStatus
    // queries storage, which must not run while mutex_ is held.
    std::vector<std::string> ids;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& [id, s] : sessions_)
            if (s.state == state) ids.push_back(id);
    }
    std::vector<RecordingStateView> out;
    for (const auto& id : ids) {
        auto v = GetStatus(id);
        if (v.ok()) out.push_back(v.value());
    }
    return out;
}

Result<void> RecordingEngine::DeleteRecording(std::string_view recordingId) {
    std::shared_ptr<Pump> pump;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        if (it->second.state == RecordingState::Recording ||
            it->second.state == RecordingState::Paused ||
            it->second.state == RecordingState::Preparing ||
            it->second.state == RecordingState::Finalizing)
            return Error::Make(Err::Recording_InvalidState, kModule,
                               "cannot delete an active recording");
        pump = it->second.pump;
    }
    // A failed recording's pump may still be finishing a pass; it has to end before
    // the session goes, and it cannot end while this thread holds the engine lock.
    StopPump(pump);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sessions_.find(recordingId);
    if (it == sessions_.end())
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    sessions_.erase(it);
    return Ok();
}

void RecordingEngine::StopAllPumps() {
    std::vector<std::shared_ptr<Pump>> pumps;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& [id, s] : sessions_)
            if (s.pump) pumps.push_back(s.pump);
    }
    for (const auto& pump : pumps) StopPump(pump);
}

Result<void> RecordingEngine::ArchiveRecording(std::string_view recordingId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sessions_.find(recordingId);
    if (it == sessions_.end())
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    if (it->second.state != RecordingState::Completed)
        return Error::Make(Err::Recording_InvalidState, kModule,
                           "only completed recordings can be archived");
    it->second.state = RecordingState::Archived;
    return Ok();
}

// --- Metadata ---

Result<void> RecordingEngine::SetMetadata(std::string_view recordingId, std::string_view key,
                                          std::string_view value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sessions_.find(recordingId);
    if (it == sessions_.end())
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    auto& md = it->second.metadata;
    const std::string k(key);
    if (k == "production") md.production = std::string(value);
    else if (k == "event") md.event = std::string(value);
    else if (k == "speaker") md.speaker = std::string(value);
    else if (k == "song") md.song = std::string(value);
    else if (k == "scripture") md.scripture = std::string(value);
    else if (k == "bus") md.bus = std::string(value);
    else md.extra[k] = std::string(value);
    return Ok();
}

Result<RecordingMetadata> RecordingEngine::GetMetadata(std::string_view recordingId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sessions_.find(recordingId);
    if (it == sessions_.end())
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    return it->second.metadata;
}

// --- Journal + recovery ---

void RecordingEngine::Journal(JournalEntry::Kind kind, std::string_view recordingId,
                              std::string_view detail) {
    JournalEntry e;
    e.kind = kind;
    e.timeMs = clockMs_.load();
    e.recordingId = std::string(recordingId);
    e.detail = std::string(detail);
    journal_.push_back(std::move(e));
}

std::vector<JournalEntry> RecordingEngine::Journal() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return journal_;
}

Result<size_t> RecordingEngine::Recover() {
    // Any session still in an active state was interrupted (docs/specs/28 §21):
    // finalize its open segment and mark recovered. Detach the containers under
    // the lock, then close them (a provider call) with NO lock held.
    std::vector<std::shared_ptr<IRecordingContainer>> closing;
    std::vector<std::string> recoveredIds;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (auto& [id, s] : sessions_) {
            if (s.state == RecordingState::Preparing || s.state == RecordingState::Recording ||
                s.state == RecordingState::Paused || s.state == RecordingState::Finalizing) {
                s.state = RecordingState::Recovering;
                closing.push_back(std::move(s.container));
                s.container.reset();
                recoveredIds.push_back(id);
                Journal(JournalEntry::Kind::SegmentClosed, id, "recovered");
                Journal(JournalEntry::Kind::Finalized, id, "recovered");
            }
        }
    }
    for (auto& c : closing)
        if (c) (void)c->Close(false);
    std::vector<std::pair<std::string, size_t>> published;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& id : recoveredIds) {
            auto it = sessions_.find(id);
            if (it == sessions_.end()) continue;
            it->second.state = RecordingState::Completed;
            published.emplace_back(id, it->second.segmentIndex);
        }
    }
    for (const auto& [id, seg] : published)
        (void)EventBus::Instance().Publish(events::RecordingRecovered{id, seg});
    return recoveredIds.size();
}

// --- Replay ---

Result<std::string> RecordingEngine::CreateReplayBuffer(std::string_view nodeId,
                                                        int64_t capacityMs) {
    if (capacityMs <= 0)
        return Error::Make(Err::InvalidArgument, kModule, "capacity must be positive");
    // Graph inspection runs with NO lock held: Graph() is unsynchronized and a
    // mutation would persist the whole graph to disk.
    auto& graph = production::ProductionEngine::Instance().Graph();
    if (!graph.HasNode(nodeId))
        return Error::Make(Err::Recording_NodeNotFound, kModule,
                           "production node not found: " + std::string(nodeId));
    const std::string id = std::format("replay_{}", replayCount_.fetch_add(1));
    ReplayBuffer b;
    b.replayId = id;
    b.nodeId = std::string(nodeId);
    b.capacityMs = capacityMs;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    replays_.emplace(id, std::move(b));
    return id;
}

Result<std::string> RecordingEngine::CreateReplay(std::string_view nodeId, ReplayMode mode,
                                                  int64_t capacityMs) {
    if (capacityMs <= 0)
        return Error::Make(Err::InvalidArgument, kModule, "capacity must be positive");
    // Graph access — including the mutation below, which persists the graph to
    // disk — runs with NO lock held.
    auto& graph = production::ProductionEngine::Instance().Graph();
    if (!graph.HasNode(nodeId))
        return Error::Make(Err::Recording_NodeNotFound, kModule,
                           "production node not found: " + std::string(nodeId));

    const std::string id = std::format("replay_{}", replayCount_.fetch_add(1));
    ReplayBuffer b;
    b.replayId = id;
    b.nodeId = std::string(nodeId);
    b.capacityMs = capacityMs;
    b.bufferedMs = capacityMs;
    b.ready = true;
    b.mode = mode;
    // The replay becomes a virtual video source in the production graph
    // (docs/specs/28 §29: Replay as a Production Source).
    auto vs = graph.AddVirtualSource(id, "Replay: " + std::string(nodeId),
                                     production::SignalType::Video);
    if (!vs.ok()) return vs.error();
    b.virtualSourceId = vs.value();
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        replays_.emplace(id, std::move(b));
    }
    (void)EventBus::Instance().Publish(events::ReplayCreated{
        id, std::string(nodeId), ToString(mode)});
    return id;
}

Result<void> RecordingEngine::RemoveReplay(std::string_view replayId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = replays_.find(replayId);
    if (it == replays_.end())
        return Error::Make(Err::Recording_ReplayNotFound, kModule, "replay not found");
    replays_.erase(it);
    return Ok();
}

std::vector<std::string> RecordingEngine::ReplayIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(replays_.size());
    for (const auto& [id, b] : replays_) out.push_back(id);
    return out;
}

Result<ReplayBuffer> RecordingEngine::GetReplay(std::string_view replayId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = replays_.find(replayId);
    if (it == replays_.end())
        return Error::Make(Err::Recording_ReplayNotFound, kModule, "replay not found");
    return it->second;
}

// --- Capture ---

Result<void> RecordingEngine::RegisterCaptureSource(CaptureSourceFactory factory) {
    if (!factory) return Error::Make(Err::InvalidArgument, kModule, "null capture factory");
    // Construct the probe device OUTSIDE the lock: a plugin factory may open a
    // driver or enumerate devices.
    auto dev = factory();
    if (!dev) return Error::Make(Err::InvalidArgument, kModule, "capture factory produced nothing");
    const std::string id = dev->DeviceId();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (captureFactories_.count(id))
        return Error::Make(Err::Recording_AlreadyExists, kModule,
                           "capture device already registered");
    captureFactories_.emplace(id, std::move(factory));
    return Ok();
}

std::vector<std::string> RecordingEngine::CaptureDeviceIds() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(captureFactories_.size());
    for (const auto& [id, f] : captureFactories_) out.push_back(id);
    return out;
}

Result<void> RecordingEngine::ConnectCaptureDevice(std::string_view deviceId) {
    const std::string devKey(deviceId);
    CaptureSourceFactory factory;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = captureFactories_.find(devKey);
        if (it == captureFactories_.end())
            return Error::Make(Err::Recording_CaptureNotFound, kModule,
                               "capture device not registered");
        if (connectedDevices_.count(devKey))
            return Error::Make(Err::Recording_AlreadyActive, kModule,
                               "capture device already connected");
        factory = it->second;
        // Reserve the slot so a concurrent connect for the same device loses.
        connectedDevices_.insert(devKey);
    }
    // Everything below runs with NO lock held: the capture provider start and
    // the production-graph mutation (which persists the graph) can both block.
    auto dev = factory();
    if (!dev) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        connectedDevices_.erase(devKey);
        return Error::Make(Err::Recording_CaptureNotFound, kModule,
                           "capture device unavailable");
    }
    if (auto r = dev->Start(); !r.ok()) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        connectedDevices_.erase(devKey);
        return r;
    }
    // Hot-plug: the device becomes a Source node in the production graph
    // (docs/specs/28 §31-§32).
    auto& graph = production::ProductionEngine::Instance().Graph();
    auto added = graph.AddSource(deviceId, dev->DisplayName(), dev->Signal());
    if (!added.ok()) {
        (void)dev->Stop();
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        connectedDevices_.erase(devKey);
        return added.error();
    }
    (void)EventBus::Instance().Publish(events::CaptureDeviceConnected{
        std::string(deviceId), dev->Kind()});
    return Ok();
}

Result<void> RecordingEngine::DisconnectCaptureDevice(std::string_view deviceId) {
    const std::string devKey(deviceId);
    CaptureSourceFactory factory;
    bool hadFactory = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = connectedDevices_.find(devKey);
        if (it == connectedDevices_.end())
            return Error::Make(Err::Recording_CaptureNotFound, kModule,
                               "capture device not connected");
        auto fit = captureFactories_.find(devKey);
        if (fit != captureFactories_.end()) {
            factory = fit->second;
            hadFactory = true;
        }
        // Commit the state change before the slow provider/graph teardown.
        connectedDevices_.erase(it);
    }
    // Provider stop and the graph-node removal run with NO lock held.
    if (hadFactory) {
        auto dev = factory();
        if (dev) (void)dev->Stop();
    }
    auto& graph = production::ProductionEngine::Instance().Graph();
    (void)graph.RemoveNode(devKey);
    (void)EventBus::Instance().Publish(
        events::CaptureDeviceDisconnected{devKey});
    return Ok();
}

std::vector<std::string> RecordingEngine::ConnectedDevices() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(connectedDevices_.size());
    for (const auto& id : connectedDevices_) out.push_back(id);
    return out;
}

// --- Storage ---

Result<StorageInfo> RecordingEngine::GetStorageInfo(std::string_view path) const {
    // Snapshot the inputs under the lock; the filesystem query below runs with
    // no lock held (see FreePercent).
    std::string dir;
    int64_t simulated = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        dir = path.empty() ? defaultDir_ : std::string(path);
        simulated = simulatedFreeBytes_;
    }
    StorageInfo info;

    if (simulated >= 0) {
        // Test hook: deterministic disk without touching the filesystem.
        // freeBytes carries the real byte figure (for capacity estimates);
        // freePercent is simulated directly (0-100).
        info.freeBytes = simulated;
        info.totalBytes = simulated * 2;
        info.freePercent = static_cast<int>(std::clamp<int64_t>(simulated, 0, 100));
    } else {
        std::error_code ec;
        const auto sp = std::filesystem::space(dir, ec);
        if (ec)
            return Error::Make(Err::Recording_ValidationFailed, kModule,
                               "cannot query storage: " + ec.message());
        info.freeBytes = static_cast<int64_t>(sp.available);
        info.totalBytes = static_cast<int64_t>(sp.capacity);
        info.freePercent = info.totalBytes > 0
                               ? static_cast<int>((info.freeBytes * 100) / info.totalBytes)
                               : 100;
    }
    // Estimate uses the default master profile (docs/specs/28 §17).
    RecordingProfile master;
    info.estimatedBytesPerHour = EstimatedBytesPerHour(master);
    info.capacityHours = info.estimatedBytesPerHour > 0
                             ? static_cast<double>(info.freeBytes) /
                                   static_cast<double>(info.estimatedBytesPerHour)
                             : 0.0;
    info.healthy = info.freePercent >= 10;
    return info;
}

Result<void> RecordingEngine::SetDefaultDirectory(std::string_view dir) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (dir.empty())
        return Error::Make(Err::InvalidArgument, kModule, "empty directory");
    defaultDir_ = std::string(dir);
    // Update profiles that inherited the previous default.
    for (auto& [id, p] : profiles_) p.directory = defaultDir_;
    return Ok();
}

std::string RecordingEngine::DefaultDirectory() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return defaultDir_;
}

Result<void> RecordingEngine::SetStoragePolicy(int freePercentBelow, StorageAction action) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (freePercentBelow <= 0 || freePercentBelow > 100)
        return Error::Make(Err::InvalidArgument, kModule, "threshold out of range");
    if (action == StorageAction::None) storagePolicy_.erase(freePercentBelow);
    else storagePolicy_[freePercentBelow] = action;
    return Ok();
}

int64_t RecordingEngine::FreePercent() const {
    // Snapshot the inputs under the lock, then query the filesystem with NO
    // lock held: std::filesystem::space is a PAL stat that can block (network
    // or removable media) and must not stall every other engine operation.
    std::string dir;
    int64_t simulated = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        simulated = simulatedFreeBytes_;
        dir = defaultDir_.empty() ? "." : defaultDir_;
    }
    if (simulated >= 0) return std::clamp<int64_t>(simulated, 0, 100);
    int64_t freeBytes = 0;
    int64_t totalBytes = 1;
    std::error_code ec;
    const auto sp = std::filesystem::space(dir, ec);
    if (!ec) {
        freeBytes = static_cast<int64_t>(sp.available);
        totalBytes = static_cast<int64_t>(sp.capacity) > 0
                         ? static_cast<int64_t>(sp.capacity)
                         : freeBytes + 1;
    } else {
        // Unknowable storage is treated as healthy — never fabricate a
        // disk-full emergency from a failed stat (docs/specs/28 §19).
        return 100;
    }
    return static_cast<int>((freeBytes * 100) / totalBytes);
}

int64_t RecordingEngine::EstimatedBytesPerHour(const RecordingProfile& p) const {
    // Video: bitrate kbps -> bytes/hour. Audio: assume 128 kbps when compressed,
    // PCM 16-bit at 48 kHz * channels otherwise.
    const int64_t videoBytes = static_cast<int64_t>(p.videoBitrateKbps) * 1000 / 8 * 3600;
    const int64_t audioBytes = p.audioCodec == AudioCodec::PCM
                                   ? static_cast<int64_t>(p.audioChannels) * 48000 * 2 * 3600
                                   : static_cast<int64_t>(p.audioChannels) * 128000 / 8 * 3600;
    return videoBytes + audioBytes;
}

std::string RecordingEngine::MakePath(const RecordingProfile& p, std::string_view base,
                                      size_t segment, int64_t stampMs) {
    const std::string dir = p.directory.empty() ? std::string(base) : p.directory;
    return std::format("{}/{}_{}_seg{}.{}", dir, Sanitize(p.name.empty() ? p.id : p.name),
                       stampMs, segment, ContainerName(p.container));
}

// --- Segments ---

// Route a recording to its next segment: the current container is closed and
// the next one opened with NO engine lock held (a real container's Close/Open
// can block for a long time — docs/specs/28 §20). State changes stay under the
// lock and re-validate the session afterwards, since it may have been stopped
// while the provider was working.
void RecordingEngine::RotateSegment(const std::string& recordingId) {
    std::shared_ptr<IRecordingContainer> closing;
    std::shared_ptr<Pump> pumpRef;
    ContainerFactory factory;
    RecordingProfile profile;
    std::string path;
    size_t seg = 0;
    bool ready = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end()) return;
        Session& s = it->second;
        // Re-check under the lock: the session may have been advanced/stopped.
        if (s.state != RecordingState::Recording) return;
        if (!(s.profile.segmentDurationMs > 0 &&
              s.durationMs / s.profile.segmentDurationMs >=
                  static_cast<int64_t>(s.segmentIndex)))
            return;
        auto cit = containers_.find(ContainerName(s.profile.container));
        if (cit == containers_.end()) {
            errorCount_.fetch_add(1);
            s.state = RecordingState::Failed;
            Journal(JournalEntry::Kind::Failed, recordingId, "container not registered");
            return;
        }
        factory = cit->second;
        profile = s.profile;
        seg = s.segmentIndex + 1;
        path = MakePath(profile, defaultDir_, seg, clockMs_.load());
        closing = std::move(s.container);
        s.container.reset();
        pumpRef = s.pump;
        ready = true;
    }
    if (!ready) return;

    // --- provider calls, no lock held ---
    if (closing) {
        // The pump may be writing into the outgoing segment: wait for its write to
        // finish, and it cannot write again until the new segment is in place.
        std::unique_lock<std::mutex> writing;
        if (pumpRef) writing = std::unique_lock<std::mutex>(pumpRef->writeMutex);
        (void)closing->Close(false);
    }
    auto container = factory();
    Result<std::string> opened =
        container ? container->Open(profile, path)
                  : Result<std::string>(Error::Make(Err::Recording_ContainerNotFound, kModule,
                                                    "container factory produced nothing"));

    std::string failure;
    bool discard = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end() || it->second.state != RecordingState::Recording) {
            discard = true;   // provider finalized outside the lock, below
        } else if (!opened.ok()) {
            failure = opened.error().message;
            errorCount_.fetch_add(1);
            it->second.state = RecordingState::Failed;
            Journal(JournalEntry::Kind::Failed, recordingId, failure);
        } else {
            Session& s = it->second;
            s.container = container;
            s.filePath = opened.value();
            s.segmentIndex = seg;
            Journal(JournalEntry::Kind::SegmentCreated, recordingId, opened.value());
            segmentCount_.fetch_add(1);
        }
    }
    if (discard) {
        if (container) (void)container->Close(false);
        return;
    }
    if (!failure.empty())
        (void)EventBus::Instance().Publish(events::RecordingFailed{recordingId, failure});
    else
        (void)EventBus::Instance().Publish(
            events::SegmentCreated{recordingId, seg, opened.value()});
}

// --- Scheduling + time pump ---

Result<void> RecordingEngine::ScheduleStart(std::string_view profileId,
                                            std::string_view nodeId, int64_t atMs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!profiles_.count(profileId))
        return Error::Make(Err::Recording_ProfileNotFound, kModule, "profile not found");
    scheduledStarts_[atMs] = {std::string(profileId), std::string(nodeId)};
    return Ok();
}

Result<void> RecordingEngine::ScheduleStop(std::string_view recordingId, int64_t atMs) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!sessions_.count(recordingId))
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    scheduledStops_[atMs] = std::string(recordingId);
    return Ok();
}

int RecordingEngine::MaxActiveEncoderLoad() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int max = 0;
    for (const auto& [id, s] : sessions_) {
        const bool occupies = s.state == RecordingState::Preparing ||
                              s.state == RecordingState::Recording ||
                              s.state == RecordingState::Paused ||
                              s.state == RecordingState::Finalizing ||
                              s.state == RecordingState::Recovering;
        if (occupies) max = std::max(max, s.encoderLoad);
    }
    return max;
}

Result<EncoderHealth> RecordingEngine::GetRecordingHealth(std::string_view recordingId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sessions_.find(recordingId);
    if (it == sessions_.end())
        return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
    const auto& s = it->second;
    EncoderHealth h;
    h.load = s.encoderLoad;
    h.droppedFrames = static_cast<size_t>(s.droppedFrames);
    h.queueDepth = 0;
    h.bitrateKbps = s.profile.videoBitrateKbps;
    return h;
}

Result<void> RecordingEngine::UpdateEncoderLoad(std::string_view recordingId, int loadPct) {
    bool overloaded = false;
    std::string encoder;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        it->second.encoderLoad = std::clamp(loadPct, 0, 100);
        if (it->second.encoderLoad >= 90) {
            overloaded = true;
            encoder = it->second.encoder;
        }
    }
    if (overloaded)
        (void)EventBus::Instance().Publish(events::EncoderOverload{
            std::string(recordingId), encoder});
    return Ok();
}

Result<void> RecordingEngine::UpdateDroppedFrames(std::string_view recordingId,
                                                  size_t count) {
    size_t delta = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end())
            return Error::Make(Err::Recording_NotFound, kModule, "recording not found");
        delta = count > static_cast<size_t>(it->second.droppedFrames)
                    ? count - static_cast<size_t>(it->second.droppedFrames)
                    : 0;
        it->second.droppedFrames = static_cast<int>(count);
    }
    if (delta > 0)
        (void)EventBus::Instance().Publish(events::DroppedFramesDetected{
            std::string(recordingId), delta});
    return Ok();
}

Result<void> RecordingEngine::Tick(int64_t advanceMs) {
    // The scope below holds the lock for session/replay/schedule state only.
    // Segment rotations and the disk monitor run AFTER it closes, because both
    // reach provider/PAL calls that must not run under the engine lock.
    std::vector<std::string> rotate;
    // Replay buffers that just became ready, published AFTER the lock closes.
    struct ReadyPing { std::string rid, nodeId; int64_t capacityMs; };
    std::vector<ReadyPing> replayReady;
    // Scheduled work that came due: taken out of the schedule under the lock and
    // RUN after it closes — StartRecording/StopRecording open and close provider
    // containers (docs/specs/28 §38).
    std::vector<std::pair<std::string, std::string>> dueStarts;
    std::vector<std::string> dueStops;
    {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!running_.load())
        return Error::Make(Err::Recording_InvalidState, kModule, "Start() first");
    if (advanceMs < 0)
        return Error::Make(Err::InvalidArgument, kModule, "negative advance");

    clockMs_.fetch_add(advanceMs);
    const int64_t now = clockMs_.load();

    // Drive active sessions: duration, size estimate, segmentation.
    for (auto& [id, s] : sessions_) {
        if (s.state == RecordingState::Recording) {
            s.durationMs += advanceMs;
            s.sizeBytes = EstimatedBytesPerHour(s.profile) * s.durationMs / 3600000;
            // Segment boundary (docs/specs/28 §20). The container close/open is
            // deferred until after the lock is released (see rotate, below).
            if (s.profile.segmentDurationMs > 0 &&
                s.durationMs / s.profile.segmentDurationMs >=
                    static_cast<int64_t>(s.segmentIndex))
                rotate.push_back(id);
        }
    }
    // Replay buffers fill (docs/specs/28 §27).
    for (auto& [rid, b] : replays_) {
        if (b.bufferedMs < b.capacityMs) {
            b.bufferedMs = std::min(b.bufferedMs + advanceMs, b.capacityMs);
            if (b.bufferedMs >= b.capacityMs && !b.ready) {
                b.ready = true;
                replayReady.push_back({rid, b.nodeId, b.capacityMs});
            }
        }
    }

    // Scheduled starts/stops (docs/specs/28 §38).
    auto sit = scheduledStarts_.begin();
    while (sit != scheduledStarts_.end() && sit->first <= now) {
        dueStarts.push_back(sit->second);
        sit = scheduledStarts_.erase(sit);
    }
    auto sop = scheduledStops_.begin();
    while (sop != scheduledStops_.end() && sop->first <= now) {
        dueStops.push_back(sop->second);
        sop = scheduledStops_.erase(sop);
    }
    }   // release mutex_ — provider calls below must not run under it

    // Scheduled work that came due: run here, with the lock released (both open
    // or close a provider container — real work, and re-entrant into this
    // engine, which is exactly what must not happen under mutex_).
    for (const auto& [pid, nid] : dueStarts) (void)StartRecording(pid, nid);
    for (const auto& id : dueStops) (void)StopRecording(id, "scheduled");

    // Replay-ready notices: dispatched here, with mutex_ released, like every
    // other bus dispatch in this engine.
    for (const auto& r : replayReady)
        (void)EventBus::Instance().Publish(
            events::ReplayBufferReady{r.rid, r.nodeId, r.capacityMs});

    // Segment rotations: container Close/Open with NO lock held.
    for (const auto& id : rotate) RotateSegment(id);
    RunDiskMonitor();
    return Ok();
}

void RecordingEngine::RunDiskMonitor() {
    // FreePercent queries the filesystem; it takes only a short internal snapshot
    // lock, so the PAL stat never runs while we hold mutex_ below.
    const int fp = FreePercent();
    // Highest-severity threshold that applies wins (docs/specs/28 §19).
    StorageAction action = StorageAction::None;
    std::vector<std::string> victims;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        for (const auto& [threshold, act] : storagePolicy_)
            if (fp < threshold && SeverityRank(act) > SeverityRank(action)) action = act;
        if (action == StorageAction::StopOptional) {
            for (const auto& [id, s] : sessions_)
                if (s.state == RecordingState::Recording &&
                    s.profile.priority == RecordingPriority::Optional)
                    victims.push_back(id);
        } else if (action == StorageAction::StopAll) {
            for (const auto& [id, s] : sessions_)
                if (s.state == RecordingState::Recording || s.state == RecordingState::Paused)
                    victims.push_back(id);
        }
    }

    if (action == StorageAction::Warn) {
        (void)EventBus::Instance().Publish(events::DiskSpaceWarning{fp, "warning"});
    } else if (action == StorageAction::StopOptional) {
        (void)EventBus::Instance().Publish(events::DiskSpaceWarning{fp, "critical"});
        for (const auto& id : victims) (void)StopRecording(id, "disk pressure");
    } else if (action == StorageAction::StopAll) {
        (void)EventBus::Instance().Publish(events::DiskSpaceWarning{fp, "emergency"});
        for (const auto& id : victims) (void)StopRecording(id, "disk emergency");
    }
}

// --- Events ---

void RecordingEngine::WireEvents() {
    // Consumes nothing today; schedules and automation drive Tick directly.
    // Plugins may subscribe here later without touching the core.
}

void RecordingEngine::UnwireEvents() {
    for (auto& s : subscriptions_) (void)EventBus::Instance().Unsubscribe(s);
    subscriptions_.clear();
}

// ---------------------------------------------------------------------------
// The pump: one thread per recording. Frames come from the render buffer the
// recording reads whenever it holds a new one; audio comes from a listener on the
// program mix, queued until the pump writes it.

namespace {

// The buffer a recording reads when its production node names none: the on-air
// feed (the same name LiveOutputController::kPreviewName carries).
constexpr const char* kOnAirBuffer = "__live_preview__";
// How often the pump looks for new frames and audio.
constexpr auto kPumpInterval = std::chrono::milliseconds(10);
// Bounded audio backlog: past this, the oldest queued block is dropped.
constexpr size_t kMaxQueuedAudioBlocks = 512;

double ElapsedMs(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

void RecordingEngine::StartPump(const std::string& recordingId) {
    auto pump = std::make_shared<Pump>();
    pump->recordingId = recordingId;
    pump->bufferName = kOnAirBuffer;
    std::string nodeId;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end()) return;
        nodeId = it->second.nodeId;
    }
    // A production node may name its own render buffer in its "recordBuffer" meta.
    const auto node = production::ProductionEngine::Instance().Graph().GetNode(nodeId);
    if (node.ok()) {
        const auto meta = node.value().meta.find("recordBuffer");
        if (meta != node.value().meta.end() && !meta->second.empty())
            pump->bufferName = meta->second;
    }
    // Frames already in the buffer belong to before this recording.
    if (auto out = rendering::RenderEngine::Instance().GetOutput(pump->bufferName); out.ok())
        if (auto* fb = dynamic_cast<rendering::FrameBufferOutput*>(out.value().get()))
            pump->lastFrame = fb->FramesReceived();

    // The audio listener copies each rendered chunk; the pump writes it later. Audio is
    // stamped on the mix's sample clock, counted from the moment this recording began.
    // The listener hears only the buses wired to this recording's output node. With no
    // output node there is nothing to hear: no tap, rather than the speaker mix (an
    // empty id means the speakers to the mixer).
    pump->audioOrigin = audio::AudioEngine::Instance().FramesRendered();
    pump->audioTap = nodeId.empty() ? uint64_t{0} : audio::AudioEngine::Instance().AddMixTap(
        nodeId,
        [raw = pump.get()](const float* samples, uint32_t frames, uint32_t channels,
                           uint32_t rateHz, uint64_t startFrame) {
            Pump::AudioBlock block;
            block.samples.assign(samples, samples + static_cast<size_t>(frames) * channels);
            block.channels = channels;
            block.rateHz = rateHz;
            const uint64_t fromOrigin = startFrame > raw->audioOrigin ? startFrame - raw->audioOrigin : 0;
            block.timestampMs = rateHz > 0 ? static_cast<double>(fromOrigin) * 1000.0 / rateHz : 0.0;
            std::lock_guard<std::mutex> lock(raw->audioMutex);
            if (raw->audio.size() >= kMaxQueuedAudioBlocks)
                raw->audio.pop_front();
            raw->audio.push_back(std::move(block));
        });

    pump->thread = std::thread([this, pump]() { PumpLoop(pump); });
    bool attached = false;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it != sessions_.end()) {
            it->second.pump = pump;
            attached = true;
        }
    }
    // Joined only with the engine lock released: the pump takes that lock to write.
    if (!attached) StopPump(pump);
}

void RecordingEngine::StopPump(const std::shared_ptr<Pump>& pump) {
    if (!pump) return;
    if (pump->audioTap != 0) {
        audio::AudioEngine::Instance().RemoveMixTap(pump->audioTap);
        pump->audioTap = 0;
    }
    pump->stop.store(true);
    pump->wake.notify_all();
    if (pump->thread.joinable() && pump->thread.get_id() != std::this_thread::get_id())
        pump->thread.join();
}

void RecordingEngine::PumpLoop(std::shared_ptr<Pump> pump) {
    while (!pump->stop.load()) {
        WriteVideoOnce(*pump);
        WriteAudioOnce(*pump);
        std::unique_lock<std::mutex> lock(pump->wakeMutex);
        pump->wake.wait_for(lock, kPumpInterval, [&pump] { return pump->stop.load(); });
    }
}

std::shared_ptr<IRecordingContainer> RecordingEngine::WritableContainer(std::string_view recordingId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = sessions_.find(recordingId);
    if (it == sessions_.end() || it->second.state != RecordingState::Recording)
        return nullptr;
    return it->second.container;
}

void RecordingEngine::WriteVideoOnce(Pump& pump) {
    auto out = rendering::RenderEngine::Instance().GetOutput(pump.bufferName);
    if (!out.ok()) return;
    auto* fb = dynamic_cast<rendering::FrameBufferOutput*>(out.value().get());
    if (!fb) return;
    const uint64_t received = fb->FramesReceived();
    if (received == pump.lastFrame) return;
    pump.lastFrame = received;
    const auto frame = fb->LastFrameSnapshot();
    if (!frame || frame->empty()) return;

    std::string failure;
    {
        std::lock_guard<std::mutex> writing(pump.writeMutex);
        auto container = WritableContainer(pump.recordingId);
        if (!container) return;
        VideoFrameIn in;
        in.pixels = frame->pixels.data();
        in.width = frame->width;
        in.height = frame->height;
        in.timestampMs = ElapsedMs(pump.start);
        if (auto written = container->WriteVideo(in); !written.ok())
            failure = written.error().message;
    }
    if (!failure.empty()) FailRecording(pump.recordingId, failure);
}

void RecordingEngine::WriteAudioOnce(Pump& pump) {
    std::deque<Pump::AudioBlock> blocks;
    {
        std::lock_guard<std::mutex> lock(pump.audioMutex);
        blocks.swap(pump.audio);
    }
    if (blocks.empty()) return;

    std::string failure;
    {
        std::lock_guard<std::mutex> writing(pump.writeMutex);
        // Paused or not writing: the audio is not kept, so a resume starts clean.
        auto container = WritableContainer(pump.recordingId);
        if (!container) return;
        for (const auto& block : blocks) {
            AudioBlockIn in;
            in.samples = block.samples.data();
            in.channels = block.channels;
            in.frames = block.channels > 0 ? block.samples.size() / block.channels : 0;
            in.rateHz = block.rateHz;
            in.timestampMs = block.timestampMs;
            if (auto written = container->WriteAudio(in); !written.ok()) {
                failure = written.error().message;
                break;
            }
        }
    }
    if (!failure.empty()) FailRecording(pump.recordingId, failure);
}

void RecordingEngine::FailRecording(const std::string& recordingId, const std::string& message) {
    std::shared_ptr<IRecordingContainer> container;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = sessions_.find(recordingId);
        if (it == sessions_.end() || it->second.state != RecordingState::Recording) return;
        errorCount_.fetch_add(1);
        it->second.state = RecordingState::Failed;
        Journal(JournalEntry::Kind::Failed, recordingId, message);
        // The pump ends with its recording: it must not outlive a failed session.
        if (it->second.pump) it->second.pump->stop.store(true);
        // Keep what was written: the container finalizes the file it has.
        container = std::move(it->second.container);
        it->second.container.reset();
    }
    if (container) (void)container->Close(true);
    (void)EventBus::Instance().Publish(events::RecordingFailed{recordingId, message});
}

} // namespace bps::recording
