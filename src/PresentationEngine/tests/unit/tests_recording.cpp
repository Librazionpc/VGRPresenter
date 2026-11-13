// Unit tests: Recording, Replay & Media Capture Engine (docs/specs/28).
// Split so a single phase can run alone:  ./bps_unit_tests recording
#include "TestHarness.hpp"

#include "modules/audio/AudioEngine.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/recording/QtRecordingContainer.hpp"
#include "modules/recording/RecordingEngine.hpp"
#include "modules/recording/RecordingPlugin.hpp"
#include "modules/recording/WaveAudioContainer.hpp"

#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioFormat>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <thread>

namespace rk = bps::recording;
namespace pr = bps::production;

namespace {
// Event counters shared by the recording suite.
struct RecEvents {
    int started = 0;
    int stopped = 0;
    int paused = 0;
    int resumed = 0;
    int failed = 0;
    int recovered = 0;
    int segments = 0;
    int disk = 0;
    int encoder = 0;
    int dropped = 0;
    int replayReady = 0;
    int replayCreated = 0;
    int captureOn = 0;
    int captureOff = 0;
    std::vector<Subscription> subs;

    void Wire() {
        subs.push_back(EventBus::Instance().Subscribe<events::RecordingStarted>(
            [&](const events::RecordingStarted&) { ++started; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::RecordingStopped>(
            [&](const events::RecordingStopped&) { ++stopped; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::RecordingPaused>(
            [&](const events::RecordingPaused&) { ++paused; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::RecordingResumed>(
            [&](const events::RecordingResumed&) { ++resumed; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::RecordingFailed>(
            [&](const events::RecordingFailed&) { ++failed; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::RecordingRecovered>(
            [&](const events::RecordingRecovered&) { ++recovered; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::SegmentCreated>(
            [&](const events::SegmentCreated&) { ++segments; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::DiskSpaceWarning>(
            [&](const events::DiskSpaceWarning&) { ++disk; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::EncoderOverload>(
            [&](const events::EncoderOverload&) { ++encoder; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::DroppedFramesDetected>(
            [&](const events::DroppedFramesDetected&) { ++dropped; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::ReplayBufferReady>(
            [&](const events::ReplayBufferReady&) { ++replayReady; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::ReplayCreated>(
            [&](const events::ReplayCreated&) { ++replayCreated; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::CaptureDeviceConnected>(
            [&](const events::CaptureDeviceConnected&) { ++captureOn; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::CaptureDeviceDisconnected>(
            [&](const events::CaptureDeviceDisconnected&) { ++captureOff; }, 0));
    }
    void Unwire() {
        for (auto& s : subs) (void)EventBus::Instance().Unsubscribe(s);
        subs.clear();
    }
};

// A capture device for hot-plug tests.
class TestCam final : public rk::ICaptureSource {
public:
    explicit TestCam(std::string id) : id_(std::move(id)) {}
    const char* DeviceId() const noexcept override { return id_.c_str(); }
    const char* Kind() const noexcept override { return "camera"; }
    pr::SignalType Signal() const noexcept override { return pr::SignalType::Video; }
    Result<void> Start() override { started_ = true; return Ok(); }
    Result<void> Stop() override { started_ = false; return Ok(); }
    bool Started() const { return started_; }

private:
    std::string id_;
    bool started_ = false;
};
} // namespace

void TestRecordingProfiles() {
    auto& rec = rk::RecordingEngine::Instance();
    CHECK(rec.Initialize().ok());
    CHECK(rec.Start().ok());

    // Default engine profile exists.
    CHECK(rec.ProfileIds().size() >= 1);

    // Profile creation + validation.
    rk::RecordingProfile master;
    master.id = "sunday";
    master.name = "Sunday Service";
    master.container = rk::ContainerKind::MP4;
    master.videoCodec = rk::VideoCodec::H264;
    master.audioCodec = rk::AudioCodec::AAC;
    master.segmentDurationMs = 60000;
    CHECK(rec.CreateProfile(master).ok());
    CHECK(rec.CreateProfile(master).error().code == Err::Recording_AlreadyExists);

    rk::RecordingProfile bad;
    bad.id = "bad";
    bad.width = 0;
    CHECK(rec.CreateProfile(bad).error().code == Err::Recording_ValidationFailed);

    // Unregistered container is rejected: a plugin-added container that has
    // not been registered yet cannot be used by a profile.
    rk::RecordingProfile exotic;
    exotic.id = "exotic";
    CHECK(rec.RegisterContainer("nut",
                                []() -> std::shared_ptr<rk::IRecordingContainer> { return nullptr; })
              .ok());
    CHECK(rec.CreateProfile(exotic).ok());           // mkv default, registered
    (void)rec.RemoveProfile("exotic");

    auto got = rec.GetProfile("sunday");
    CHECK(got.ok() && got.value().name == "Sunday Service");
    CHECK(rec.GetProfile("nope").error().code == Err::Recording_ProfileNotFound);

    // Templates reference existing profiles.
    CHECK(rec.CreateTemplate("service", {"sunday"}).ok());
    CHECK(rec.CreateTemplate("service", {}).error().code == Err::Recording_AlreadyExists);
    CHECK(rec.CreateTemplate("bad", {"ghost"}).error().code ==
          Err::Recording_ProfileNotFound);
    CHECK(rec.TemplateNames().size() == 1);

    // Remove profile + template cleanup.
    CHECK(rec.RemoveProfile("sunday").ok());
    CHECK(rec.RemoveProfile("sunday").error().code == Err::Recording_ProfileNotFound);
}

void TestRecordingEncoders() {
    auto& rec = rk::RecordingEngine::Instance();

    // Software encoder present by default; hardware is registered by plugins.
    auto kinds = rec.DetectEncoders();
    CHECK(std::find(kinds.begin(), kinds.end(), rk::EncoderKind::Software) != kinds.end());

    auto p = rec.GetProfile("master");
    CHECK(p.ok());
    CHECK(rec.SelectEncoder(p.value()).value() == rk::EncoderKind::Software);

    // Explicit preference for an unavailable encoder errors.
    CHECK(rec.SetPreferredEncoder("master", rk::EncoderKind::NVENC).error().code ==
          Err::Recording_EncoderUnavailable);
    // ... but a plugin can register one.
    CHECK(rec.RegisterVideoEncoder(rk::EncoderKind::NVENC,
                                   []() -> std::shared_ptr<rk::IVideoEncoder> {
                                       struct N : rk::IVideoEncoder {
                                           const char* Codec() const noexcept override { return "H.264"; }
                                           rk::EncoderKind Kind() const noexcept override {
                                               return rk::EncoderKind::NVENC;
                                           }
                                           Result<void> Initialize(const rk::RecordingProfile&) override { return Ok(); }
                                           Result<void> Shutdown() override { return Ok(); }
                                       };
                                       return std::make_shared<N>();
                                   })
                     .ok());
    CHECK(rec.RegisterVideoEncoder(rk::EncoderKind::NVENC,
                                   []() -> std::shared_ptr<rk::IVideoEncoder> { return nullptr; })
              .error().code == Err::Recording_AlreadyExists);
    // Auto selection now prefers hardware.
    auto p2 = rec.GetProfile("master");
    CHECK(p2.ok());
    CHECK(rec.SelectEncoder(p2.value()).value() == rk::EncoderKind::NVENC);
    CHECK(rec.SetPreferredEncoder("master", rk::EncoderKind::NVENC).ok());
    CHECK(rec.GetProfile("master").value().encoderPreference == rk::EncoderKind::NVENC);

    // Container/codec registries are plugin seams (a second, distinct name).
    CHECK(rec.RegisterContainer("nut2",
                                []() -> std::shared_ptr<rk::IRecordingContainer> { return nullptr; })
              .ok());
    CHECK(rec.RegisterContainer("nut2",
                                []() -> std::shared_ptr<rk::IRecordingContainer> { return nullptr; })
              .error().code == Err::Recording_AlreadyExists);
    CHECK(rec.RegisterAudioEncoder("Opus",
                                   []() -> std::shared_ptr<rk::IAudioEncoder> { return nullptr; })
              .ok());
}

void TestRecordingLifecycle() {
    auto& rec = rk::RecordingEngine::Instance();
    auto& prod = pr::ProductionEngine::Instance();
    auto& g = prod.Graph();
    CHECK(prod.Initialize().ok());
    CHECK(prod.Start().ok());

    auto pvb = prod.CreateBus("pvb2", "Program Video", pr::BusRole::Program,
                              pr::SignalType::Video);
    auto prog = prod.CreateBus("prog2", "Program Audio", pr::BusRole::Program,
                               pr::SignalType::Audio);
    CHECK(pvb.ok() && prog.ok());

    // A segmented profile (docs/specs/28 §20: long recordings split).
    rk::RecordingProfile seg;
    seg.id = "segmented";
    seg.name = "Segmented";
    seg.segmentDurationMs = 60000;
    CHECK(rec.CreateProfile(seg).ok());

    RecEvents ev;
    ev.Wire();

    // Recording a missing graph node fails.
    CHECK(rec.StartRecording("master", "ghost").error().code ==
          Err::Recording_NodeNotFound);

    // Start master recording on the video bus.
    auto rid = rec.StartRecording("segmented", "pvb2", rk::TapPoint::PostProcessing);
    CHECK(rid.ok());
    if (!rid.ok()) std::printf("[recording] StartRecording failed: %s\n", rid.error().message.c_str());
    const std::string id = rid.value();
    CHECK(ev.started == 1);

    // Duplicate active recording on the same node is rejected.
    CHECK(rec.StartRecording("master", "pvb2").error().code ==
          Err::Recording_AlreadyActive);

    // Status + metadata.
    auto st = rec.GetStatus(id);
    CHECK(st.ok() && st.value().state == rk::RecordingState::Recording);
    CHECK(st.value().tapPoint == rk::TapPoint::PostProcessing);
    CHECK(!st.value().filePath.empty());
    CHECK(seg.container == rk::ContainerKind::MP4);
    CHECK(st.value().filePath.find(".mp4") != std::string::npos);
    CHECK(rec.SetMetadata(id, "event", "Sunday Service").ok());
    CHECK(rec.SetMetadata(id, "speaker", "John").ok());
    CHECK(rec.SetMetadata(id, "scripture", "John 3:16").ok());
    auto md = rec.GetMetadata(id);
    CHECK(md.ok() && md.value().event == "Sunday Service");
    CHECK(md.value().speaker == "John");
    CHECK(md.value().scripture == "John 3:16");

    // Markers.
    CHECK(st.ok() && st.value().markers.empty());  // captured before the marker
    CHECK(rec.AddMarker(id, "Sermon Started").ok());
    auto st2 = rec.GetStatus(id);
    CHECK(st2.ok() && st2.value().markers.size() == 1);
    CHECK(st2.value().markers[0].label == "Sermon Started");

    // Segmentation driven by Tick.
    CHECK(rec.Tick(60000).ok());   // 1 min -> segment boundary
    auto st3 = rec.GetStatus(id);
    CHECK(st3.ok() && st3.value().segmentIndex >= 2);
    CHECK(st3.value().durationMs == 60000);
    CHECK(ev.segments >= 1);

    // Pause / resume.
    CHECK(rec.PauseRecording(id).ok());
    CHECK(ev.paused == 1);
    CHECK(rec.GetStatus(id).value().state == rk::RecordingState::Paused);
    CHECK(rec.PauseRecording(id).error().code == Err::Recording_InvalidState);
    CHECK(rec.ResumeRecording(id).ok());
    CHECK(ev.resumed == 1);
    CHECK(rec.GetStatus(id).value().state == rk::RecordingState::Recording);

    // Encoder + frame health.
    CHECK(rec.UpdateEncoderLoad(id, 95).ok());
    CHECK(ev.encoder == 1);
    CHECK(rec.GetRecordingHealth(id).value().load == 95);
    CHECK(rec.UpdateDroppedFrames(id, 7).ok());
    CHECK(ev.dropped == 1);
    CHECK(rec.GetRecordingHealth(id).value().droppedFrames == 7);
    CHECK(rec.UpdateDroppedFrames(id, 7).ok());  // no new delta -> no event
    CHECK(ev.dropped == 1);

    // ISO recording on a second node (isolated camera).
    auto cam = g.AddSource("cam2", "Camera 2", pr::SignalType::Video, {60, 15, 2048, 512});
    CHECK(cam.ok());
    auto iso = rec.StartRecording("master", "cam2", rk::TapPoint::PreProcessing);
    CHECK(iso.ok());
    CHECK(rec.RecordingIds().size() == 2);
    CHECK(rec.Recordings(rk::RecordingState::Recording).size() == 2);

    // Stop the ISO normally before the disk emergency below.
    CHECK(rec.StopRecording(iso.value(), "done").ok());
    CHECK(ev.stopped == 1);
    CHECK(rec.DeleteRecording(iso.value()).ok());  // completed -> deletable

    // Storage simulation: emergency disk stops remaining recordings
    // (docs/specs/28 §19: StopAll below 5% protects the live production).
    rec.SetSimulatedFreeBytes(2);  // ~2% free of a tiny disk
    CHECK(rec.Tick(1000).ok());
    CHECK(ev.disk >= 1);
    auto st4 = rec.GetStatus(id);
    CHECK(st4.ok() && st4.value().state == rk::RecordingState::Completed);
    CHECK(ev.stopped >= 2);
    rec.SetSimulatedFreeBytes(kTestFreeBytes);

    CHECK(rec.StopRecording(id, "again").error().code == Err::Recording_NotActive);

    // Journal + archive + delete.
    CHECK(rec.Journal().size() >= 4);
    CHECK(rec.ArchiveRecording(id).ok());
    CHECK(rec.GetStatus(id).value().state == rk::RecordingState::Archived);
    CHECK(rec.DeleteRecording(id).ok());
    CHECK(rec.GetStatus(id).error().code == Err::Recording_NotFound);

    ev.Unwire();
}

void TestRecordingRecovery() {
    auto& rec = rk::RecordingEngine::Instance();
    auto& prod = pr::ProductionEngine::Instance();
    auto& g = prod.Graph();
    auto node = g.AddSource("cam3", "Camera 3", pr::SignalType::Video, {60, 15, 2048, 512});
    CHECK(node.ok());

    RecEvents ev;
    ev.Wire();

    // Crash-simulated: session left in Recording state, then Recover() finalizes.
    auto rid = rec.StartRecording("master", "cam3");
    CHECK(rid.ok());
    CHECK(rec.GetStatus(rid.value()).value().state == rk::RecordingState::Recording);
    auto n = rec.Recover();
    CHECK(n.ok() && n.value() >= 1);
    CHECK(ev.recovered >= 1);
    auto st = rec.GetStatus(rid.value());
    CHECK(st.ok() && st.value().state == rk::RecordingState::Completed);
    CHECK(rec.StopRecording(rid.value()).error().code == Err::Recording_NotActive);

    ev.Unwire();
}

void TestRecordingReplay() {
    auto& rec = rk::RecordingEngine::Instance();
    auto& prod = pr::ProductionEngine::Instance();
    auto& g = prod.Graph();

    RecEvents ev;
    ev.Wire();

    // Rolling buffer fills over time.
    auto buf = rec.CreateReplayBuffer("pvb2", 30000);
    CHECK(buf.ok());
    CHECK(rec.GetReplay(buf.value()).value().capacityMs == 30000);
    CHECK(!rec.GetReplay(buf.value()).value().ready);
    CHECK(rec.Tick(30000).ok());
    CHECK(ev.replayReady == 1);
    CHECK(rec.GetReplay(buf.value()).value().ready);

    // Instant replay becomes a production virtual source.
    auto replay = rec.CreateReplay("pvb2", rk::ReplayMode::Normal, 30000);
    CHECK(replay.ok());
    CHECK(ev.replayCreated == 1);
    auto rb = rec.GetReplay(replay.value());
    CHECK(rb.ok() && !rb.value().virtualSourceId.empty());
    CHECK(g.HasNode(rb.value().virtualSourceId));  // Replay as a production source

    // Replay on a missing node fails.
    CHECK(rec.CreateReplay("ghost", rk::ReplayMode::Normal, 30000).error().code ==
          Err::Recording_NodeNotFound);

    CHECK(rec.RemoveReplay(replay.value()).ok());
    CHECK(rec.RemoveReplay(replay.value()).error().code == Err::Recording_ReplayNotFound);
    // The rolling buffer is a distinct object and survives replay removal.
    CHECK(rec.GetReplay(buf.value()).ok());

    ev.Unwire();
}

void TestRecordingCapture() {
    auto& rec = rk::RecordingEngine::Instance();
    auto& prod = pr::ProductionEngine::Instance();
    auto& g = prod.Graph();

    RecEvents ev;
    ev.Wire();

    // Register + connect a capture device; it becomes a graph source (hot-plug).
    CHECK(rec.RegisterCaptureSource([]() -> std::shared_ptr<rk::ICaptureSource> {
              return std::make_shared<TestCam>("usb0");
          })
              .ok());
    CHECK(rec.RegisterCaptureSource([]() -> std::shared_ptr<rk::ICaptureSource> {
              return std::make_shared<TestCam>("usb0");
          })
              .error().code == Err::Recording_AlreadyExists);
    CHECK(rec.CaptureDeviceIds().size() == 1);

    CHECK(rec.ConnectCaptureDevice("usb0").ok());
    CHECK(ev.captureOn == 1);
    CHECK(rec.ConnectedDevices().size() == 1);
    CHECK(g.HasNode("usb0"));
    CHECK(rec.ConnectCaptureDevice("usb0").error().code == Err::Recording_AlreadyActive);
    CHECK(rec.ConnectCaptureDevice("ghost").error().code == Err::Recording_CaptureNotFound);

    CHECK(rec.DisconnectCaptureDevice("usb0").ok());
    CHECK(ev.captureOff == 1);
    CHECK(rec.ConnectedDevices().empty());
    CHECK(rec.DisconnectCaptureDevice("usb0").error().code == Err::Recording_CaptureNotFound);

    ev.Unwire();
}

void TestRecordingStorageAndScheduling() {
    auto& rec = rk::RecordingEngine::Instance();
    auto& prod = pr::ProductionEngine::Instance();

    // Storage estimate + simulated disk (10 GB at ~5 GB/hr gives > 1 hour).
    rec.SetSimulatedFreeBytes(static_cast<int64_t>(10) * 1000 * 1000 * 1000);
    auto info = rec.GetStorageInfo();
    CHECK(info.ok());
    CHECK(info.value().freeBytes == static_cast<int64_t>(10) * 1000 * 1000 * 1000);
    CHECK(info.value().estimatedBytesPerHour > 0);
    CHECK(info.value().capacityHours > 1.0);

    // Policy thresholds.
    CHECK(rec.SetStoragePolicy(15, rk::StorageAction::Warn).ok());
    CHECK(rec.SetStoragePolicy(15, rk::StorageAction::None).ok());  // remove
    rec.SetSimulatedFreeBytes(kTestFreeBytes);

    // Default directory round-trip.
    const std::string oldDir = rec.DefaultDirectory();
    CHECK(rec.SetDefaultDirectory("/tmp/vgr-rec").ok());
    CHECK(rec.DefaultDirectory() == "/tmp/vgr-rec");
    CHECK(rec.SetDefaultDirectory(oldDir).ok());

    for (const auto& r : rec.RecordingIds())
        (void)rec.DeleteRecording(r);
    auto node = prod.Graph().AddSource("cam4", "Camera 4", pr::SignalType::Video,
                                       {60, 15, 2048, 512});
    CHECK(node.ok());
    const int64_t base = rec.ClockMs();
    CHECK(rec.ScheduleStart("master", "cam4", base + 10000).ok());
    CHECK(rec.ScheduleStop("nope", base + 20000).error().code == Err::Recording_NotFound);
    CHECK(rec.Tick(9000).ok());
    CHECK(rec.RecordingIds().empty());          // not yet started
    CHECK(rec.Tick(2000).ok());                 // crosses +10s -> starts
    CHECK(rec.RecordingIds().size() == 1);
    auto rid = rec.RecordingIds().front();
    CHECK(rec.ScheduleStop(rid, base + 20000).ok());
    CHECK(rec.Tick(11000).ok());                // crosses +20s -> stops
    CHECK(rec.GetStatus(rid).value().state == rk::RecordingState::Completed);
}

void TestRecordingEngine() {
    TestRecordingProfiles();
    TestRecordingEncoders();
    TestRecordingLifecycle();
    TestRecordingRecovery();
    TestRecordingReplay();
    TestRecordingCapture();
    TestRecordingStorageAndScheduling();

    // Tear down engines in reverse boot order.
    auto& rec = rk::RecordingEngine::Instance();
    CHECK(rec.Stop().ok());
    CHECK(rec.Shutdown().ok());
    auto& prod = pr::ProductionEngine::Instance();
    CHECK(prod.Stop().ok());
    CHECK(prod.Shutdown().ok());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestRecordingDispatchOutsideLock() {
    auto& rec = rk::RecordingEngine::Instance();
    auto& prod = pr::ProductionEngine::Instance();
    CHECK(prod.Initialize().ok());
    CHECK(prod.Start().ok());
    CHECK(rec.Initialize().ok());
    CHECK(rec.Start().ok());

    // A node to hang the replay buffer on (no container/profile needed — the
    // buffer is filled by Tick, which is what dispatches the notice).
    auto bus = prod.CreateBus("dispatchscope", "Dispatch Scope", pr::BusRole::Program,
                              pr::SignalType::Video);
    CHECK(bus.ok());
    auto buf = rec.CreateReplayBuffer("dispatchscope", 1000);
    CHECK(buf.ok());

    std::atomic<bool> inHandler{false};
    std::atomic<bool> otherThreadDone{false};
    std::atomic<bool> reachedEngineWhileParked{false};
    Subscription sub = EventBus::Instance().Subscribe<events::ReplayBufferReady>(
        [&](const events::ReplayBufferReady&) {
            inHandler.store(true);
            for (int i = 0; i < 3000 && !otherThreadDone.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            reachedEngineWhileParked.store(otherThreadDone.load());
        }, 0);

    std::thread other([&] {
        for (int i = 0; i < 5000 && !inHandler.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!inHandler.load()) return;   // the dispatch never happened
        (void)rec.RecordingIds();                       // takes the engine's lock
        (void)rec.GetRecordingHealth("no-such-id");     // ...and again
        otherThreadDone.store(true);
    });

    // Fills the buffer to capacity; Tick dispatches the ready notice after its
    // locked scope ends, so this call parks INSIDE the subscriber.
    CHECK(rec.Tick(1000).ok());
    other.join();
    CHECK(inHandler.load());   // the subscriber really ran
    // With the lock held across the dispatch, neither call could have completed
    // while this one was parked.
    CHECK(reachedEngineWhileParked.load());

    (void)EventBus::Instance().Unsubscribe(sub);
    if (buf.ok()) (void)rec.RemoveReplay(buf.value());
    CHECK(rec.Stop().ok());
    CHECK(rec.Shutdown().ok());
    CHECK(prod.Stop().ok());
    CHECK(prod.Shutdown().ok());
}

// ---- Real files, real encoders ---------------------------------------------------------
// Every file below is written through the SAME container the app records with (Qt Multimedia, on every
// OS) and then judged two ways: its structure is walked box by box / element by element (a file that
// "exists" but was never finalized fails here), and Qt's own media player opens it, decodes it to the end
// and reports what came out (pictures, audio samples, duration, loudest sample). A file the player cannot
// play is a corrupted file, whatever its size.
namespace {
struct MediaFileInfo {
    bool opened = false;        // the player accepted the file
    bool hasVideo = false;
    bool hasAudio = false;
    int width = 0;
    int height = 0;
    int videoSamples = 0;       // pictures delivered while it played to the end
    int audioSamples = 0;       // audio frames delivered while it played to the end
    double audioPeak = -1.0;    // loudest decoded audio sample, 0..1 (-1 = nothing decoded)
    double seconds = 0.0;       // the file's own duration
    double lastPictureSeconds = 0.0;   // presentation time of the last picture decoded
    double firstPictureSeconds = -1.0; // ... and of the first
    std::string error;
};

MediaFileInfo InspectMediaFile(const std::string& path, bool wantAudio) {
    MediaFileInfo info;
    if (!QCoreApplication::instance()) {
        info.error = "no Qt application";
        return info;
    }
    QMediaPlayer player;
    QVideoSink sink;
    QAudioBufferOutput audioOut;
    player.setVideoSink(&sink);
    if (wantAudio)
        player.setAudioBufferOutput(&audioOut);
    QObject::connect(&sink, &QVideoSink::videoFrameChanged, &sink, [&](const QVideoFrame& frame) {
        if (!frame.isValid()) return;
        ++info.videoSamples;
        if (info.firstPictureSeconds < 0 && frame.startTime() >= 0)
            info.firstPictureSeconds = static_cast<double>(frame.startTime()) / 1e6;
        if (frame.startTime() > 0)
            info.lastPictureSeconds = std::max(info.lastPictureSeconds, static_cast<double>(frame.startTime()) / 1e6);
        if (info.width == 0) {
            info.width = frame.width();
            info.height = frame.height();
        }
    });
    QObject::connect(&audioOut, &QAudioBufferOutput::audioBufferReceived, &audioOut, [&](const QAudioBuffer& buffer) {
        if (!buffer.isValid()) return;
        info.audioSamples += buffer.frameCount();
        const QAudioFormat f = buffer.format();
        const int channels = f.channelCount();
        const int total = buffer.frameCount() * channels;
        if (f.sampleFormat() == QAudioFormat::Float) {
            const float* d = buffer.constData<float>();
            for (int i = 0; i < total; ++i) info.audioPeak = std::max(info.audioPeak, static_cast<double>(std::fabs(d[i])));
        } else if (f.sampleFormat() == QAudioFormat::Int16) {
            const qint16* d = buffer.constData<qint16>();
            for (int i = 0; i < total; ++i) info.audioPeak = std::max(info.audioPeak, std::fabs(d[i] / 32768.0));
        } else if (f.sampleFormat() == QAudioFormat::Int32) {
            const qint32* d = buffer.constData<qint32>();
            for (int i = 0; i < total; ++i) info.audioPeak = std::max(info.audioPeak, std::fabs(d[i] / 2147483648.0));
        }
    });
    QString playerError;
    QObject::connect(&player, &QMediaPlayer::errorOccurred, &player,
                     [&](QMediaPlayer::Error, const QString& text) { playerError = text; });

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia) loop.quit();
    });
    player.setSource(QUrl::fromLocalFile(QString::fromStdString(path)));
    player.setPlaybackRate(2.0);   // twice real time: the decode is what is being judged, not the wall clock
    player.play();
    timeout.start(30'000);
    loop.exec();

    info.opened = player.mediaStatus() == QMediaPlayer::EndOfMedia;
    info.hasVideo = player.hasVideo();
    info.hasAudio = player.hasAudio();
    info.seconds = static_cast<double>(player.duration()) / 1000.0;
    if (!info.opened)
        info.error = playerError.isEmpty() ? "did not play to the end" : playerError.toStdString();
    return info;
}

// The top-level boxes of an MP4/MOV/M4A file, in order. A file whose boxes do not tile the file exactly
// (a moov that was never written, an mdat whose size runs past the end) returns false.
bool WalkMp4Boxes(const std::vector<uint8_t>& bytes, std::vector<std::string>& names, int& trackCount) {
    names.clear();
    trackCount = 0;
    auto be32 = [&](size_t at) {
        return (static_cast<uint64_t>(bytes[at]) << 24) | (static_cast<uint64_t>(bytes[at + 1]) << 16)
             | (static_cast<uint64_t>(bytes[at + 2]) << 8) | static_cast<uint64_t>(bytes[at + 3]);
    };
    size_t at = 0;
    std::vector<std::pair<size_t, size_t>> moov;
    while (at + 8 <= bytes.size()) {
        uint64_t size = be32(at);
        const std::string name(reinterpret_cast<const char*>(&bytes[at + 4]), 4);
        size_t header = 8;
        if (size == 1) {
            if (at + 16 > bytes.size()) return false;
            size = (be32(at + 8) << 32) | be32(at + 12);
            header = 16;
        } else if (size == 0) {
            size = bytes.size() - at;   // runs to the end of the file
        }
        if (size < header || at + size > bytes.size()) return false;
        names.push_back(name);
        if (name == "moov") moov.emplace_back(at + header, at + static_cast<size_t>(size));
        at += static_cast<size_t>(size);
    }
    if (at != bytes.size()) return false;
    for (const auto& [from, to] : moov) {
        size_t p = from;
        while (p + 8 <= to) {
            const uint64_t size = be32(p);
            const std::string name(reinterpret_cast<const char*>(&bytes[p + 4]), 4);
            if (size < 8 || p + size > to) return false;
            if (name == "trak") ++trackCount;
            p += static_cast<size_t>(size);
        }
    }
    return true;
}

std::vector<uint8_t> ReadAll(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0) {
            bytes.resize(static_cast<size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        }
        std::fclose(f);
    }
    return bytes;
}

bool Has(const std::vector<std::string>& names, const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
} // namespace

void TestRecordingRealEncode() {
    namespace fs = std::filesystem;
    const auto supported = rk::QtSupportedContainers();
    if (supported.empty() || !QCoreApplication::instance()) {
        std::printf("[real-encode] the Qt media backend cannot encode here (no Qt application): skipped\n");
        return;
    }
    const fs::path dir = fs::temp_directory_path() / "bps_real_encode";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto can = [&](rk::ContainerKind k) { return std::find(supported.begin(), supported.end(), k) != supported.end(); };

    constexpr int kW = 640, kH = 360, kFrames = 90;
    constexpr double kFps = 30.0;
    constexpr uint32_t kRate = 48'000;
    std::vector<uint32_t> pixels(static_cast<size_t>(kW) * kH);

    for (const rk::ContainerKind containerKind : {rk::ContainerKind::MP4, rk::ContainerKind::MKV}) {
        if (!can(containerKind)) {
            std::printf("[real-encode] %s skipped: no encoder for it on this machine\n", rk::ToString(containerKind));
            continue;
        }
        rk::RecordingProfile p;
        p.id = "real";
        p.name = "real";
        p.container = containerKind;
        p.videoCodec = rk::VideoCodec::H264;
        p.audioCodec = rk::AudioCodec::AAC;
        p.width = kW;
        p.height = kH;
        p.fps = kFps;
        p.videoBitrateKbps = 4000;
        p.audioBitrateKbps = 128;
        p.audioChannels = 2;

        auto container = rk::MakeQtRecordingContainer(containerKind);
        const std::string ext = rk::ToString(containerKind);
        const std::string path = (dir / ("take." + ext)).string();

        auto opened = container->Open(p, path);
        CHECK(opened.ok());
        if (!opened.ok()) {
            std::printf("[real-encode] %s OPEN FAILED: %s\n", ext.c_str(), opened.error().message.c_str());
            continue;
        }

        // Pictures: a moving colour pattern so every frame differs, paced like the pump paces them.
        // Sound: a tone, in 10 ms blocks.
        const size_t toneFrames = kRate / 100;
        std::vector<float> tone(toneFrames * 2);
        double tonePhase = 0.0;
        double audioMs = 0.0;
        bool writeOk = true;
        for (int f = 0; f < kFrames; ++f) {
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x) {
                    const uint32_t r = static_cast<uint32_t>((x + f * 4) & 0xFF);
                    const uint32_t g = static_cast<uint32_t>((y * 255) / kH);
                    const uint32_t b = static_cast<uint32_t>((f * 255) / kFrames);
                    pixels[static_cast<size_t>(y) * kW + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
                }
            rk::VideoFrameIn in;
            in.pixels = pixels.data();
            in.width = kW;
            in.height = kH;
            in.timestampMs = f * 1000.0 / kFps;
            if (!container->WriteVideo(in).ok()) writeOk = false;
            // Three 10 ms blocks of tone per 33 ms picture.
            for (int blockIndex = 0; blockIndex < 3; ++blockIndex) {
                for (size_t i = 0; i < toneFrames; ++i) {
                    const float s = 0.25f * static_cast<float>(std::sin(tonePhase));
                    tonePhase += 2.0 * 3.14159265358979 * 440.0 / kRate;
                    tone[i * 2] = s;
                    tone[i * 2 + 1] = s;
                }
                rk::AudioBlockIn a;
                a.samples = tone.data();
                a.frames = toneFrames;
                a.channels = 2;
                a.rateHz = kRate;
                a.timestampMs = audioMs;
                audioMs += 10.0;
                if (!container->WriteAudio(a).ok()) writeOk = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        CHECK(writeOk);
        auto closed = container->Close(true);
        CHECK(closed.ok());
        if (!closed.ok())
            std::printf("[real-encode] %s CLOSE FAILED: %s\n", ext.c_str(), closed.error().message.c_str());

        std::error_code sizeError;
        const auto size = fs::file_size(path, sizeError);
        CHECK(!sizeError && size > 20000);

        // 1. The file's own bookkeeping.
        const std::vector<uint8_t> bytes = ReadAll(path);
        CHECK(bytes.size() == size);
        if (containerKind == rk::ContainerKind::MP4) {
            std::vector<std::string> boxes;
            int tracks = 0;
            const bool walked = WalkMp4Boxes(bytes, boxes, tracks);
            CHECK(walked);   // the boxes tile the file exactly: nothing cut off, nothing trailing
            CHECK(!boxes.empty() && boxes.front() == "ftyp");
            CHECK(Has(boxes, "moov"));   // the index: written when the file is finalized
            CHECK(Has(boxes, "mdat"));
            CHECK(tracks == 2);          // one picture track, one sound track
            std::printf("[real-encode] mp4 boxes:");
            for (const auto& b : boxes) std::printf(" %s", b.c_str());
            std::printf(" (tracks=%d)\n", tracks);
        } else {
            CHECK(bytes.size() > 64);
            CHECK(bytes[0] == 0x1A && bytes[1] == 0x45 && bytes[2] == 0xDF && bytes[3] == 0xA3);   // EBML magic
            const std::string head(bytes.begin(), bytes.begin() + 64);
            CHECK(head.find("matroska") != std::string::npos);
        }

        // 2. The file plays: it decodes to the end, with its pictures and its sound.
        const MediaFileInfo info = InspectMediaFile(path, true);
        std::printf("[real-encode] %-3s size=%llu KB opened=%d video=%d audio=%d %dx%d pictures=%d audioFrames=%d dur=%.2fs peak=%.3f%s%s\n",
                    ext.c_str(), static_cast<unsigned long long>(size / 1024), info.opened ? 1 : 0, info.hasVideo ? 1 : 0,
                    info.hasAudio ? 1 : 0, info.width, info.height, info.videoSamples, info.audioSamples, info.seconds,
                    info.audioPeak, info.error.empty() ? "" : " err=", info.error.c_str());
        CHECK(info.opened);
        CHECK(info.hasVideo);
        CHECK(info.hasAudio);
        CHECK(info.width == kW && info.height == kH);
        // Pictures come back out (the recorder drops a frame only when the encoder is saturated).
        CHECK(info.videoSamples >= kFrames * 6 / 10);
        // The last picture sits where the take ended, whatever the container's own duration field says.
        // MP4 (the default container) is held to it. KNOWN: Qt 6.11's FFmpeg backend on Windows writes the MKV's
        // pictures about a second AFTER its sound (first picture at ~1.0 s), so for MKV it is reported, not asserted.
        if (containerKind == rk::ContainerKind::MP4) {
            CHECK(info.firstPictureSeconds >= 0.0 && info.firstPictureSeconds < 0.2);
            CHECK(info.lastPictureSeconds > 2.5 && info.lastPictureSeconds < 3.5);
        }
        std::printf("[real-encode]     pictures from %.2fs to %.2fs\n", info.firstPictureSeconds, info.lastPictureSeconds);
        CHECK(info.seconds > 2.0 && info.seconds < (containerKind == rk::ContainerKind::MKV ? 6.0 : 4.5));
        CHECK(info.audioSamples > static_cast<int>(kRate));   // well over a second of sound
        CHECK(info.audioPeak > 0.05);
        fs::remove(path, ec);
    }

    // ---- AUDIO PRESETS: audio-only files (WAV, M4A, MP3, FLAC) ---------------------------
    // The portable WAV writer takes its rate from the output mix: run the mix for this part if nothing else has.
    auto& playout = bps::audio::AudioEngine::Instance();
    bool startedPlayout = false;
    if (playout.MixFormat().rateHz == 0 && playout.Initialize().ok() && playout.Start().ok())
        startedPlayout = true;
    for (int wait = 0; wait < 40 && playout.MixFormat().rateHz == 0; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint32_t mixRate = playout.MixFormat().rateHz;
    for (const rk::ContainerKind audioKind : {rk::ContainerKind::WAV, rk::ContainerKind::M4A, rk::ContainerKind::MP3,
                                              rk::ContainerKind::FLAC}) {
        const std::string ext = rk::ToString(audioKind);
        if (audioKind == rk::ContainerKind::WAV && mixRate == 0) {
            std::printf("[real-encode] wav skipped: the output mix is not running\n");
            continue;
        }
        if (audioKind != rk::ContainerKind::WAV && !can(audioKind)) {
            std::printf("[real-encode] %s skipped: this machine has no encoder for it\n", ext.c_str());
            continue;
        }
        rk::RecordingProfile p;
        p.id = "audio";
        p.name = "audio";
        p.container = audioKind;
        p.videoCodec = rk::VideoCodec::None;
        p.audioCodec = audioKind == rk::ContainerKind::WAV ? rk::AudioCodec::PCM
                     : audioKind == rk::ContainerKind::M4A ? rk::AudioCodec::AAC
                     : audioKind == rk::ContainerKind::FLAC ? rk::AudioCodec::FLAC : rk::AudioCodec::MP3;
        p.audioBitrateKbps = 128;
        p.audioChannels = 2;
        const std::string path = (dir / ("tone." + ext)).string();
        std::shared_ptr<rk::IRecordingContainer> container =
            audioKind == rk::ContainerKind::WAV ? std::shared_ptr<rk::IRecordingContainer>(std::make_shared<rk::WaveAudioContainer>())
                                                : rk::MakeQtRecordingContainer(audioKind);
        auto opened = container->Open(p, path);
        CHECK(opened.ok());
        if (!opened.ok()) {
            std::printf("[real-encode] %s OPEN FAILED: %s\n", ext.c_str(), opened.error().message.c_str());
            continue;
        }
        const size_t toneFrames = kRate / 100;
        std::vector<float> tone(toneFrames * 2);
        double phase = 0.0;
        bool writeOk = true;
        for (int block = 0; block < 300; ++block) {
            for (size_t i = 0; i < toneFrames; ++i) {
                const float s = 0.25f * static_cast<float>(std::sin(phase));
                phase += 2.0 * 3.14159265358979 * 440.0 / kRate;
                tone[i * 2] = s;
                tone[i * 2 + 1] = s;
            }
            rk::AudioBlockIn a;
            a.samples = tone.data();
            a.frames = toneFrames;
            a.channels = 2;
            a.rateHz = kRate;
            a.timestampMs = block * 10.0;
            if (!container->WriteAudio(a).ok()) writeOk = false;
        }
        CHECK(writeOk);
        auto closed = container->Close(true);
        CHECK(closed.ok());
        if (!closed.ok()) std::printf("[real-encode] %s CLOSE FAILED: %s\n", ext.c_str(), closed.error().message.c_str());

        std::error_code sizeError;
        const auto size = fs::file_size(path, sizeError);
        const MediaFileInfo info = InspectMediaFile(path, true);
        std::printf("[real-encode] audio %-4s size=%llu KB opened=%d audio=%d audioFrames=%d dur=%.2fs peak=%.3f%s%s\n",
                    ext.c_str(), static_cast<unsigned long long>(size / 1024), info.opened ? 1 : 0, info.hasAudio ? 1 : 0,
                    info.audioSamples, info.seconds, info.audioPeak, info.error.empty() ? "" : " err=", info.error.c_str());
        CHECK(!sizeError && size > 10000);
        CHECK(info.opened);
        CHECK(!info.hasVideo);
        CHECK(info.hasAudio);
        CHECK(info.audioSamples > 0);
        CHECK(info.seconds > 2.5 && info.seconds < 3.6);
        CHECK(info.audioPeak > 0.1 && info.audioPeak < 0.5);
        if (audioKind == rk::ContainerKind::WAV) {
            // Exactly the PCM that went in, behind a 44-byte header.
            CHECK(size == 44u + static_cast<uint64_t>(300) * toneFrames * 4u);
        }
        if (audioKind == rk::ContainerKind::M4A) {
            std::vector<std::string> boxes;
            int tracks = 0;
            CHECK(WalkMp4Boxes(ReadAll(path), boxes, tracks));
            CHECK(Has(boxes, "moov") && Has(boxes, "mdat") && tracks == 1);
        }
        fs::remove(path, ec);
    }
    fs::remove_all(dir, ec);
    if (startedPlayout) {
        (void)playout.Stop();
        (void)playout.Shutdown();
    }
}
