// Unit tests: Extended Production Engine (docs/specs/27).
// Split so a single phase can run alone:  ./bps_unit_tests production
#include "TestHarness.hpp"

#include "modules/production/ProductionEngine.hpp"
#include "modules/production/ProductionGraph.hpp"
#include "modules/production/AudioMixer.hpp"

#include <algorithm>

namespace pr = bps::production;

namespace {
// Event counters shared by the engine suite.
struct ProdEvents {
    int outputs = 0;
    int scenes = 0;
    int controls = 0;
    int sources = 0;
    int virtuals = 0;
    int emergencies = 0;
    std::vector<Subscription> subs;

    void Wire() {
        subs.push_back(EventBus::Instance().Subscribe<events::OutputChanged>(
            [&](const events::OutputChanged&) { ++outputs; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::SceneStateChanged>(
            [&](const events::SceneStateChanged&) { ++scenes; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::ControlSignalReceived>(
            [&](const events::ControlSignalReceived&) { ++controls; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::SourceFailed>(
            [&](const events::SourceFailed&) { ++sources; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::VirtualSourceCreated>(
            [&](const events::VirtualSourceCreated&) { ++virtuals; }, 0));
        subs.push_back(EventBus::Instance().Subscribe<events::ProductionEmergency>(
            [&](const events::ProductionEmergency&) { ++emergencies; }, 0));
    }
    void Unwire() {
        for (auto& s : subs) (void)EventBus::Instance().Unsubscribe(s);
        subs.clear();
    }
};

// A capture tap whose frames are always ready, so the mixer's gain staging can
// be read straight off Render()'s output with no sound device anywhere near the
// test. Only the four pure enumeration virtuals plus the two tap virtuals are
// needed; everything else keeps IAudio's own defaults.
class StubTapAudio final : public bps::platform::IAudio {
public:
    std::vector<bps::platform::AudioDeviceInfo> Enumerate() const override { return {}; }
    Result<bps::platform::AudioDeviceInfo> DefaultOutput() const override
    {
        return Error::Make(Err::NotFound, "Audio", "the stub has no output device");
    }
    Result<bps::platform::AudioDeviceInfo> DefaultInput() const override
    {
        return Error::Make(Err::NotFound, "Audio", "the stub has no input device");
    }
    std::string Fingerprint() const override { return "stub"; }

    // Full-scale on BOTH channels: a hot stereo source.
    bps::platform::IAudio::TapAudio TapFormat(uint32_t) override
    {
        return bps::platform::IAudio::TapAudio{2, 48000};
    }
    size_t ReadTapAudio(uint32_t, float *dst, size_t frames) override
    {
        for (size_t f = 0; f < frames; ++f) {
            dst[f * 2] = 1.0f;
            dst[f * 2 + 1] = 1.0f;
        }
        return frames;
    }
};
} // namespace

// One bus takes any number of sources, and a source feeds any number of
// buses. A video bus used to hold exactly ONE provider, so routing a second
// camera/media row into it was refused — and that refusal reached the board as
// a drag that simply did not connect, with nothing on screen saying why ("two
// video sources cant be conected to one bus").
//
// The one bus rule that remains is the signal-type one: an audio-only bus has
// no video plane to feed. Everything else the matcher rejects (a cycle, a
// type mismatch) is checked before it, and stays.
void TestProductionRoutingPolicy() {
    pr::ProductionGraph g;
    CHECK(g.AddBus("bus:1#a", "Program", pr::BusRole::Custom, pr::SignalType::Audio).ok());
    CHECK(g.AddBus("bus:1#v", "Program", pr::BusRole::Custom, pr::SignalType::Video).ok());
    CHECK(g.SetMeta("bus:1#a", "uiBusType", "both").ok());
    CHECK(g.SetMeta("bus:1#v", "uiBusType", "both").ok());
    CHECK(g.AddBus("bus:2#a", "Audio only", pr::BusRole::Custom, pr::SignalType::Audio).ok());
    CHECK(g.AddBus("bus:2#v", "Audio only", pr::BusRole::Custom, pr::SignalType::Video).ok());
    CHECK(g.SetMeta("bus:2#a", "uiBusType", "audio").ok());
    CHECK(g.SetMeta("bus:2#v", "uiBusType", "audio").ok());
    CHECK(g.AddBus("bus:3#v", "Preview", pr::BusRole::Custom, pr::SignalType::Video).ok());
    CHECK(g.AddSource("vsrc:v1", "Camera", pr::SignalType::Video).ok());
    CHECK(g.AddSource("vsrc:v2", "Media", pr::SignalType::Video).ok());
    CHECK(g.AddSource("asrc:a1", "Mic", pr::SignalType::Audio).ok());

    // TWO video sources into ONE video bus — the reported refusal. The board's
    // drop gate asks CanConnect() first, so both halves have to agree.
    CHECK(g.CanConnect("vsrc:v1", "bus:1#v", pr::SignalType::Video));
    CHECK(g.Connect("vsrc:v1", "bus:1#v", pr::SignalType::Video).ok());
    CHECK(g.CanConnect("vsrc:v2", "bus:1#v", pr::SignalType::Video));
    CHECK(g.Connect("vsrc:v2", "bus:1#v", pr::SignalType::Video).ok());
    CHECK(g.Upstream("bus:1#v").size() == 2);

    // ...and ONE source feeding TWO buses (the fan-out that always worked).
    CHECK(g.CanConnect("vsrc:v1", "bus:3#v", pr::SignalType::Video));
    CHECK(g.Connect("vsrc:v1", "bus:3#v", pr::SignalType::Video).ok());
    CHECK(g.Upstream("bus:3#v").size() == 1);

    // Audio into a bus is untouched by any of this.
    CHECK(g.Connect("asrc:a1", "bus:1#a", pr::SignalType::Audio).ok());
    CHECK(g.Upstream("bus:1#a").size() == 1);

    // The audio-only bus still refuses video, both in the preview answer and
    // in the mutation itself (the UI's promise and the engine's rule agree).
    CHECK(!g.CanConnect("vsrc:v2", "bus:2#v", pr::SignalType::Video));
    CHECK(!g.Connect("vsrc:v2", "bus:2#v", pr::SignalType::Video).ok());
    CHECK(g.Upstream("bus:2#v").empty());

    // A cycle is still refused, and disconnecting one video source leaves the
    // bus's other provider in place.
    CHECK(!g.Connect("bus:1#v", "vsrc:v1", pr::SignalType::Video).ok());
    CHECK(g.Disconnect("vsrc:v1", "bus:1#v").ok());
    CHECK(g.Upstream("bus:1#v").size() == 1);
}

// Full-graph persistence: Serialize -> Restore -> Serialize produces the
// same document (round-trip), and a restored graph behaves like the original
// (routing, volume, meta, edges). Also covers auto-save firing on mutation.
void TestProductionPersistence() {
    pr::ProductionGraph g;

    // Build a representative graph: sources, bus pair, edge, volume, meta.
    CHECK(g.AddSource("cam1", "Camera 1", pr::SignalType::Video).ok());
    CHECK(g.AddSource("ain:0", "Audio Input 1", pr::SignalType::Audio).ok());
    CHECK(g.AddBus("bus:1#a", "Master", pr::BusRole::Custom, pr::SignalType::Audio).ok());
    CHECK(g.AddBus("bus:1#v", "Master", pr::BusRole::Custom, pr::SignalType::Video).ok());
    CHECK(g.Connect("ain:0", "bus:1#a", pr::SignalType::Audio).ok());
    CHECK(g.Connect("cam1", "bus:1#v", pr::SignalType::Video).ok());
    const pr::VolumeControl vol{-12.5, true, false, 0.25, 0.0};
    CHECK(g.SetVolume("bus:1#a", vol).ok());
    CHECK(g.SetMeta("bus:1#a", "uiBusType", "both").ok());
    CHECK(g.SetMeta("bus:1#v", "uiBusType", "both").ok());

    auto doc = g.Serialize();
    CHECK(doc.ok());
    const std::string blob = doc.value().ToString();
    CHECK(blob.find("bus:1#a") != std::string::npos);

    // Round-trip: a fresh graph restores to the same serialized form.
    pr::ProductionGraph g2;
    auto parsed = bps::json::Parse(blob);
    CHECK(parsed.ok());
    CHECK(g2.Restore(parsed.value()).ok());
    auto doc2 = g2.Serialize();
    CHECK(doc2.ok());
    CHECK(doc2.value().ToString() == blob);   // stable round-trip

    // Restored state behaves like the original.
    CHECK(g2.HasNode("bus:1#a") && g2.HasNode("bus:1#v"));
    auto v = g2.Volume("bus:1#a");
    CHECK(v.ok() && v.value().gainDb == vol.gainDb && v.value().mute);
    CHECK(g2.Upstream("bus:1#a").size() == 1);
    CHECK(g2.Upstream("bus:1#v").size() == 1);
    auto node = g2.GetNode("bus:1#a");
    CHECK(node.ok() && node.value().meta.at("uiBusType") == "both");

    // Auto-save: mutations invoke the installed saver.
    int saves = 0;
    g2.InstallSaver([&](bps::json::Value) { ++saves; });
    CHECK(g2.RenameNode("bus:1#a", "Master Rename").ok());
    CHECK(saves == 1);
    CHECK(g2.RemoveNode("bus:1#v").ok());
    CHECK(saves == 2);
}

// A bus built the way the UI builds one must reach the DISK on its own — with
// no clean shutdown, no explicit flush, and no PersistGate in sight.
//
// This is the reported bug ("my buses are not persisted, they do not survive a
// restart"): the QML roster models mutate this graph through Graph() directly,
// which requests a save but leaves it in the kernel's in-memory store, because
// the one thing that consumed that request — PersistGate — is private to
// ProductionEngine and only the engine's own methods construct one. The write
// therefore happened only at DatabaseManager::Shutdown, i.e. only a clean exit
// ever persisted a bus; a crash or a force-quit lost the whole session's buses.
void TestProductionGraphSavedToDisk() {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string dir = "/tmp/bps_production_durable";
    (void)fs.RemoveAll(dir);

    auto& db = DatabaseManager::Instance();
    CHECK(db.Open(dir + "/kernel.json").ok());

    auto& prod = pr::ProductionEngine::Instance();
    CHECK(prod.Initialize().ok());   // installs the saver on the graph

    // Exactly what BusListModel::createBusRow does: graph primitives, no gate,
    // no facade call — the path that used to leave nothing on disk.
    auto& g = prod.Graph();
    CHECK(g.AddBus("bus:7#a", "Durable", pr::BusRole::Custom, pr::SignalType::Audio).ok());
    CHECK(g.AddBus("bus:7#v", "Durable", pr::BusRole::Custom, pr::SignalType::Video).ok());
    CHECK(g.SetMeta("bus:7#a", "uiBusType", "audio").ok());

    // Deliberately NO Shutdown() and no flush: the deferred write (one task per
    // burst of edits) has to land by itself. Bounded wait, then the assertion.
    const std::string path = dir + "/kernel.json";
    bool onDisk = false;
    for (int i = 0; i < 60 && !onDisk; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto text = fs.ReadText(path);
        onDisk = text.ok() && text.value().find("bus:7#a") != std::string::npos;
    }
    CHECK(onDisk);
    CHECK(fs.Exists(path));
    // The document is a real database file, not a stray fragment: it carries the
    // collection the engine asked for.
    auto raw = fs.ReadText(path);
    CHECK(raw.ok() && raw.value().find("\"production\"") != std::string::npos);
    CHECK(raw.value().find("bus:7#v") != std::string::npos);

    // Coalescing: a burst of edits does not queue one write per mutation. After
    // the pending write lands, the scheduler holds no leftover flush task for
    // this engine.
    const size_t idle = TaskScheduler::Instance().ScheduledCount();
    for (int i = 0; i < 20; ++i)
        CHECK(g.SetVolume("bus:7#a", pr::VolumeControl{-3.0, false, false, 0.0, 0.0}).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto finalDoc = fs.ReadText(path);
    CHECK(finalDoc.ok() && finalDoc.value().find("-3") != std::string::npos);   // the burst saved
    CHECK(TaskScheduler::Instance().ScheduledCount() <= idle);                  // and drained

    (void)prod.Shutdown();
    (void)db.Open(std::string{});   // detach the test file from the shared store
    (void)fs.RemoveAll(dir);
}

void TestProductionGraph() {
    pr::ProductionGraph g;

    // Node creation + auto-generated ids.
    auto s1 = g.AddSource("cam1", "Camera 1", pr::SignalType::Video, {60, 15, 2048, 512});
    CHECK(s1.ok() && s1.value() == "cam1");
    auto s2 = g.AddSource("", "Mic", pr::SignalType::Audio);
    CHECK(s2.ok() && s2.value() == "src1");
    auto b1 = g.AddBus("worship", "Worship", pr::BusRole::Group, pr::SignalType::Audio);
    auto b2 = g.AddBus("pvb", "Program Video", pr::BusRole::Program, pr::SignalType::Video);
    CHECK(b1.ok() && b2.ok());
    CHECK(g.AddBus("worship", "X", pr::BusRole::Group, pr::SignalType::Audio)
              .error().code == Err::Production_AlreadyExists);

    // Routing + cycle rejection + signal mismatch.
    CHECK(g.Connect("cam1", "pvb", pr::SignalType::Video).ok());
    CHECK(g.Connect("src1", "worship", pr::SignalType::Audio).ok());
    CHECK(g.Connect("src1", "pvb", pr::SignalType::Audio).error().code ==
          Err::Production_SignalMismatch);
    auto b3 = g.AddBus("group", "Group", pr::BusRole::Group, pr::SignalType::Audio);
    CHECK(g.Connect("worship", "group", pr::SignalType::Audio).ok());
    CHECK(!g.Connect("group", "src1", pr::SignalType::Audio).ok());
    CHECK(g.Connect("group", "src1", pr::SignalType::Audio).error().code ==
          Err::Production_CircularRoute);
    CHECK(g.WouldCreateCycle("group", "src1"));
    CHECK(!g.WouldCreateCycle("cam1", "pvb"));

    // Topological order respects dependencies (src1 -> worship -> group).
    auto order = g.TopologicalOrder();
    auto pos = [&](const std::string& id) {
        auto it = std::find(order.begin(), order.end(), id);
        return static_cast<size_t>(it - order.begin());
    };
    CHECK(pos("src1") < pos("worship") && pos("worship") < pos("group"));

    // Volume + processing.
    CHECK(g.SetVolume("src1", {-3.0, false, false, 0.0, 0.0}).ok());
    CHECK(g.Volume("src1").value().gainDb == -3.0);
    CHECK(g.AddProcessing("src1", {pr::ProcessKind::Gate, 1.0, {}}).ok());
    CHECK(g.GetNode("src1").value().processing.size() == 1);
    CHECK(g.SetEnabled("src1", false).ok());
    CHECK(!g.GetNode("src1").value().enabled);

    // Outputs are complete signal destinations (video bus + audio bus).
    pr::OutputConfig cfg;
    cfg.priority = pr::OutputPriority::Critical;
    auto out = g.AddOutput("tv", "TV", pr::SignalType::Video, cfg);
    CHECK(out.ok());
    CHECK(!g.AssignOutputBuses("tv", "pvb", "pvb").ok());  // audio must be an audio bus
    CHECK(g.AssignOutputBuses("tv", "pvb", "worship").ok());
    CHECK(g.GetOutputConfig("tv").value().audioBusId == "worship");
    CHECK(g.SetOutputPriority("tv", pr::OutputPriority::High).ok());
    CHECK(g.GetOutputConfig("tv").value().priority == pr::OutputPriority::High);
    CHECK(g.SetOutputFailover("tv", {"nope"}).error().code == Err::Production_OutputNotFound);

    // Bus scenes: apply restores the saved input enablement.
    CHECK(g.SetEnabled("src1", true).ok());
    CHECK(g.SaveBusScene("worship", "sunday").ok());
    CHECK(g.BusSceneNames("worship").size() == 1);
    CHECK(g.SetEnabled("src1", false).ok());
    CHECK(g.ApplyBusScene("worship", "sunday").ok());
    CHECK(g.GetNode("src1").value().enabled);

    // Production snapshot round-trip.
    CHECK(g.AssignOutputBuses("tv", "pvb", "group").ok());
    CHECK(g.SaveSnapshot("pre").ok());
    CHECK(g.SnapshotNames().size() == 1);
    CHECK(g.AssignOutputBuses("tv", "pvb", "worship").ok());
    CHECK(g.RestoreSnapshot("pre").ok());
    CHECK(g.GetOutputConfig("tv").value().audioBusId == "group");

    // Atomic edit: a commit that breaks validation is rejected and rolled back.
    CHECK(g.BeginEdit().ok());
    CHECK(g.RemoveNode("pvb").ok());  // tv references pvb -> broken
    CHECK(!g.CommitEdit().ok());
    // Rollback restores nodes AND edges (regression: edges were lost before).
    CHECK(g.HasNode("pvb"));
    CHECK(g.Downstream("cam1") == (std::vector<std::string>{"pvb"}));
    CHECK(g.BeginEdit().ok());
    CHECK(g.SetOutputGroup("tv", "broadcast").ok());
    CHECK(g.CommitEdit().ok());
    CHECK(g.GetOutputConfig("tv").value().group == "broadcast");
    CHECK(g.BeginEdit().ok());
    g.RollbackEdit();
    CHECK(g.GetOutputConfig("tv").value().group == "broadcast");

    // Inspector + validation.
    CHECK(!g.Inspect().empty());
    CHECK(g.NodeCount() >= 5);
    size_t errs = 0;
    for (const auto& i : g.Validate())
        if (i.severity == pr::ValidationIssue::Severity::Error) ++errs;
    CHECK(errs == 0);
    // Remove the output's audio bus reference by removing the bus node.
    CHECK(g.RemoveNode("group").ok());
    for (const auto& i : g.Validate())
        if (i.code == "output_no_bus") errs++;
    CHECK(errs > 0);
}

void TestProductionEngine() {
    auto& eng = pr::ProductionEngine::Instance();
    auto& g = eng.Graph();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());

    ProdEvents ev;
    ev.Wire();

    // Sources + bus hierarchy (mic/music -> worship -> program).
    auto mic1 = g.AddSource("mic1", "Mic 1", pr::SignalType::Audio, {5, 3, 0, 0});
    auto mic2 = g.AddSource("mic2", "Mic 2", pr::SignalType::Audio, {5, 3, 0, 0});
    auto music = g.AddSource("music", "Music", pr::SignalType::Audio, {10, 5, 0, 64});
    auto cam = g.AddSource("cam1", "Camera 1", pr::SignalType::Video, {60, 15, 2048, 512});
    auto worship = eng.CreateBus("worship", "Worship", pr::BusRole::Group, pr::SignalType::Audio);
    auto program = eng.CreateBus("program", "Program", pr::BusRole::Program, pr::SignalType::Audio);
    auto pvb = eng.CreateBus("pvb", "Program Video", pr::BusRole::Program, pr::SignalType::Video);
    CHECK(mic1.ok() && mic2.ok() && music.ok() && cam.ok());
    CHECK(worship.ok() && program.ok() && pvb.ok());
    (void)g.Connect("mic1", "worship", pr::SignalType::Audio);
    (void)g.Connect("mic2", "worship", pr::SignalType::Audio);
    (void)g.Connect("music", "worship", pr::SignalType::Audio);
    (void)g.Connect("worship", "program", pr::SignalType::Audio);
    (void)g.Connect("cam1", "pvb", pr::SignalType::Video);

    // Virtual source: a bus becomes a routable source.
    auto vs = eng.CreateVirtualSource("worship");
    CHECK(vs.ok());
    CHECK(g.HasNode(vs.value()));
    CHECK(g.Downstream("worship").size() >= 1);
    CHECK(ev.virtuals == 1);

    // Output with independent audio + video buses.
    pr::OutputConfig cfg;
    auto tv = g.AddOutput("tv", "TV", pr::SignalType::Video, cfg);
    CHECK(tv.ok());
    CHECK(eng.AssignOutputBuses("tv", "pvb", "program").ok());
    CHECK(ev.outputs == 1);
    CHECK(g.GetOutputConfig("tv").value().audioBusId == "program");

    // Mixing: gain/mute/solo/pan.
    CHECK(eng.SetGain("mic1", -6.0).ok());
    CHECK(eng.SetMute("music", true).ok());
    CHECK(eng.SetSolo("mic2", true).ok());
    CHECK(eng.SetPan("mic1", 0.5).ok());
    CHECK(g.Volume("mic1").value().gainDb == -6.0);
    CHECK(g.Volume("mic1").value().pan == 0.5);
    CHECK(g.Volume("music").value().mute);
    CHECK(g.Volume("mic2").value().solo);

    // Processing per source.
    CHECK(eng.AddProcessing("mic1", {pr::ProcessKind::Gate, 1.0, {}}).ok());
    CHECK(g.GetNode("mic1").value().processing.size() == 1);

    // Meters.
    pr::MeterLevels m;
    m.peak = -3.0; m.rms = -12.0; m.lufs = -18.0; m.headroom = 9.0; m.clipping = false;
    m.channels = {-3.0, -3.0};
    CHECK(eng.UpdateMeters("mic1", m).ok());
    CHECK(eng.GetMeters("mic1").value().peak == -3.0);
    CHECK(eng.GetMeters("nope").error().code == Err::Production_NodeNotFound);

    // Ducking.
    CHECK(eng.Duck("program", "mic1", 6.0).ok());
    CHECK(eng.DuckDepth("program") == 6.0);
    CHECK(eng.ReleaseDuck("program", "mic1").ok());
    CHECK(eng.DuckDepth("program") == 0.0);

    // Fallback.
    CHECK(eng.SetSourceFallback("mic1", "mic2").ok());
    CHECK(eng.TriggerFallback("mic1", "signal lost").ok());
    CHECK(g.GetNode("mic1").value().sourceState == pr::SourceState::Failed);
    CHECK(!g.GetNode("mic1").value().enabled);
    CHECK(g.GetNode("mic2").value().enabled);
    CHECK(ev.sources == 1);

    // Scene states.
    auto sc = g.AddScene("sermon", "Sermon", pr::SignalType::Video);
    CHECK(sc.ok());
    CHECK(eng.SetSceneState("sermon", pr::SceneState::Program).ok());
    CHECK(ev.scenes == 1);
    CHECK(g.GetNode("sermon").value().sceneState == pr::SceneState::Program);

    // Clock + sync.
    eng.TickClock(1000);
    CHECK(eng.Clock().masterMs == 1000);
    CHECK(eng.SetClockOffset("audio", -20).ok());
    CHECK(eng.Clock().audioMs == 980);
    // Offsets persist across ticks (regression: they were reset to master).
    eng.TickClock(100);
    CHECK(eng.Clock().masterMs == 1100);
    CHECK(eng.Clock().audioMs == 1080);
    CHECK(eng.SetClockSync(false).ok());
    CHECK(!eng.Clock().synced);

    // Control signals are first-class.
    CHECK(eng.SendControl("midi", "note-on C3").ok());
    CHECK(ev.controls == 1);
    CHECK(eng.ControlSignalCount() == 1);

    // Macro recording.
    CHECK(eng.StartMacroRecording("start_worship").ok());
    CHECK(eng.SetGain("mic1", -9.0).ok());
    CHECK(eng.SetMute("music", false).ok());
    auto rec = eng.StopMacroRecording();
    CHECK(rec.ok() && rec.value().size() == 2);
    CHECK(eng.MacroNames() == (std::vector<std::string>{"start_worship"}));

    // Planning.
    auto plan = eng.PlanProduction({100, 100, 8192, 16384});
    CHECK(plan.feasible);
    CHECK(plan.bottleneck == "gpu");   // camera dominates
    CHECK(plan.encoder == "hardware");
    auto tight = eng.PlanProduction({10, 10, 64, 32});
    CHECK(!tight.feasible);
    CHECK(!tight.bottleneck.empty());

    // Validation + health.
    size_t errs = 0;
    for (const auto& i : eng.ValidateProduction())
        if (i.severity == pr::ValidationIssue::Severity::Error) ++errs;
    CHECK(errs == 0);
    auto health = eng.CheckProduction();
    CHECK(health.score > 0);

    // Simulation (dry run) + frame-accurate cues.
    CHECK(eng.Simulate().ok());
    CHECK(eng.ScheduleCue(5000, "control", "osc /mix/fader 0.5").ok());
    CHECK(eng.CueCount() == 1);
    eng.TickClock(1000);                     // master 2000 — cue not due
    CHECK(eng.ControlSignalCount() == 1);
    eng.TickClock(4000);                     // master 6000 — cue fires
    CHECK(eng.ControlSignalCount() == 2);
    CHECK(eng.CueCount() == 1);

    // Emergency mode.
    CHECK(eng.EmergencyMode("fire alarm drill").ok());
    CHECK(ev.emergencies == 1);
    CHECK(g.GetNode("sermon").value().sceneState == pr::SceneState::Emergency);
    CHECK(g.GetNode("mic2").value().volume.mute);   // non-critical sources muted

    // AI-ready validated commands.
    CHECK(eng.ApplyCommand("set_scene sermon standby").ok());
    CHECK(g.GetNode("sermon").value().sceneState == pr::SceneState::Standby);
    CHECK(eng.ApplyCommand("gain music -12").ok());
    CHECK(g.Volume("music").value().gainDb == -12.0);
    CHECK(eng.ApplyCommand("mute music 1").ok());
    CHECK(g.Volume("music").value().mute);
    CHECK(eng.ApplyCommand("duck program mic1 4").ok());
    CHECK(eng.DuckDepth("program") == 4.0);
    CHECK(eng.ApplyCommand("release_duck program mic1").ok());
    CHECK(eng.ApplyCommand("send_control api sync").ok());
    CHECK(!eng.ApplyCommand("frobnicate x").ok());              // unknown verb
    CHECK(!eng.ApplyCommand("set_scene missing program").ok()); // missing scene
    // Malformed numbers must error, never throw (00 §1 no-exceptions rule).
    CHECK(!eng.ApplyCommand("gain music abc").ok());
    CHECK(!eng.ApplyCommand("duck program mic1 xyz").ok());

    // Atomic live changes through the engine.
    CHECK(eng.BeginEdit().ok());
    CHECK(eng.AssignOutputBuses("tv", "", "worship").ok());
    CHECK(eng.CommitEdit().ok());
    CHECK(g.GetOutputConfig("tv").value().audioBusId == "worship");
    CHECK(eng.BeginEdit().ok());
    CHECK(g.RemoveNode("worship").ok());   // tv references worship -> broken
    CHECK(!eng.CommitEdit().ok());
    CHECK(g.HasNode("worship"));

    // Snapshots.
    CHECK(eng.SaveProductionSnapshot("sunday").ok());
    CHECK(eng.AssignOutputBuses("tv", "", "program").ok());
    CHECK(eng.RestoreProductionSnapshot("sunday").ok());
    CHECK(g.GetOutputConfig("tv").value().audioBusId == "worship");

    // Display hot-plug events feed output health.
    (void)EventBus::Instance().Publish(events::DisplayDeviceConnected{"d1", "Stage"});
    CHECK(eng.DisplaysConnected() == 1);
    (void)EventBus::Instance().Publish(events::DisplayDeviceDisconnected{"d1"});
    CHECK(eng.DisplaysConnected() == 0);

    ev.Unwire();
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}

// ---------------------------------------------------------------------------
// AudioMixer — the render-path half of the production graph. The mixer is
// the piece that consumes the bus topology into real playout frames; these
// checks pin the gain staging (dB→linear), mute, pan and the unbound-source
// gate WITHOUT a sound device (the tap source stays null; the gain law is
// exercised through the platform's own unit-test seam).
void TestProductionAudioMixer() {
    pr::ProductionGraph graph;
    CHECK(graph.AddBus("bus1", "Main", pr::BusRole::Program, pr::SignalType::Audio).ok());
    CHECK(graph.AddSource("src1", "Mic", pr::SignalType::Audio).ok());
    CHECK(graph.Connect("src1", "bus1", pr::SignalType::Audio).ok());

    pr::AudioMixer mixer;
    mixer.Attach(&graph);

    float buf[8] = {};

    // No tap source bound: nothing renders (there is no fake carrier).
    CHECK(!mixer.Render(buf, 2, 2, 48000));
    for (float s : buf) CHECK(s == 0.0f);

    // Bus mute silences the branch — and honestly reports "nothing to
    // render" (a muted graph IS silence; the platform emits it).
    pr::VolumeControl muted;
    muted.mute = true;
    (void)graph.SetVolume("bus1", muted);
    CHECK(!mixer.Render(buf, 2, 2, 48000));
    CHECK(buf[0] == 0.0f && buf[1] == 0.0f);

    // A detached mixer answers "nothing to render".
    pr::AudioMixer bare;
    CHECK(!bare.Render(buf, 2, 2, 48000));

    // ---- the L/R channel trim must REACH THE ENGINE'S OUTPUT -------------
    // The channel buttons a source card shows write uiChannelGain<channel> on
    // the source node (AudioInputListModel::pushEffectsToGraph). This is the
    // behavioural proof that the trim travels all the way down into the mixed
    // frames AND into the program meter the output tiles render — closing one
    // side really silences that side of the output, on EITHER side.
    (void)graph.SetVolume("bus1", pr::VolumeControl{});   // un-mute the bus above
    StubTapAudio taps;
    pr::AudioMixer live;
    live.Attach(&graph);
    live.AttachTaps(&taps);
    live.BindSourceDevice("src1", 7);

    (void)graph.SetMeta("src1", "uiChannelGain0", "1");
    (void)graph.SetMeta("src1", "uiChannelGain1", "0");   // the card's R button
    float rightShut[8] = {};
    CHECK(live.Render(rightShut, 4, 2, 48000));
    CHECK(rightShut[0] > 0.5f);          // the open side passes
    CHECK(rightShut[1] == 0.0f);         // the closed side is silent
    const auto rightLevel = live.ProgramLevel();
    CHECK(rightLevel.channelCount == 2);
    CHECK(rightLevel.peaks[0] > 0.5f);   // the output meter shows the live side
    CHECK(rightLevel.peaks[1] == 0.0f);  // ...and its closed side stays dark

    // The mirror: close the LEFT instead.
    (void)graph.SetMeta("src1", "uiChannelGain0", "0");
    (void)graph.SetMeta("src1", "uiChannelGain1", "1");
    float leftShut[8] = {};
    CHECK(live.Render(leftShut, 4, 2, 48000));
    CHECK(leftShut[0] == 0.0f);
    CHECK(leftShut[1] > 0.5f);

    // Both sides closed: nothing to hear, and the meter goes dark with it
    // rather than freezing on the last peak.
    (void)graph.SetMeta("src1", "uiChannelGain1", "0");
    float allShut[8] = {};
    CHECK(!live.Render(allShut, 4, 2, 48000));
    CHECK(allShut[0] == 0.0f && allShut[1] == 0.0f);
    const auto darkLevel = live.ProgramLevel();
    CHECK(darkLevel.peaks[0] == 0.0f && darkLevel.peaks[1] == 0.0f);

    // ---- the BUS's own L/R trim must reach the output too -----------------
    // A bus card's L/R buttons write the SAME uiChannelGain<channel> key, on
    // the BUS plane node (BusListModel::setChannelGain). A bus is a summing
    // point, so this is the proof that closing one side of a BUS silences that
    // side of the bus's mix even while every source feeding it is wide open —
    // which is exactly what the bus card's pills claim to do, and what
    // "mute that side only, not the whole bus" has to mean in real audio.
    (void)graph.SetMeta("src1", "uiChannelGain0", "1");   // source wide open again
    (void)graph.SetMeta("src1", "uiChannelGain1", "1");

    (void)graph.SetMeta("bus1", "uiChannelGain0", "1");
    (void)graph.SetMeta("bus1", "uiChannelGain1", "0");   // the bus card's R pill
    float busRightShut[8] = {};
    CHECK(live.Render(busRightShut, 4, 2, 48000));
    CHECK(busRightShut[0] > 0.5f);        // the bus's open side still passes
    CHECK(busRightShut[1] == 0.0f);       // the bus's closed side is silent
    const auto busLevel = live.ProgramLevel();
    CHECK(busLevel.channelCount == 2);
    CHECK(busLevel.peaks[0] > 0.5f);      // the program meter agrees
    CHECK(busLevel.peaks[1] == 0.0f);

    // The mirror: the bus card's L pill instead.
    (void)graph.SetMeta("bus1", "uiChannelGain0", "0");
    (void)graph.SetMeta("bus1", "uiChannelGain1", "1");
    float busLeftShut[8] = {};
    CHECK(live.Render(busLeftShut, 4, 2, 48000));
    CHECK(busLeftShut[0] == 0.0f);
    CHECK(busLeftShut[1] > 0.5f);

    // Both sides of the bus closed: it contributes nothing at all (the same
    // early-out a fully-closed source takes).
    (void)graph.SetMeta("bus1", "uiChannelGain0", "0");
    (void)graph.SetMeta("bus1", "uiChannelGain1", "0");
    float busDark[8] = {};
    CHECK(!live.Render(busDark, 4, 2, 48000));
    CHECK(busDark[0] == 0.0f && busDark[1] == 0.0f);

    // ---- the ROUTING MATRIX's per-channel mask --------------------------
    // The matrix (AudioInputListModel's Routing… modal) routes a row's CHANNELS
    // to buses: each bus it names gets "uiRouteMask:<busNode>" with a bit per
    // routed channel, which is what lets one source feed its left channel to one
    // bus and its right to another. A bus with NO mask reads the whole source
    // (the old contract) — pinned here so the mask can never silently mute a
    // route the bus dialog made.
    {
        (void)graph.SetMeta("src1", "uiChannelGain0", "1");
        (void)graph.SetMeta("src1", "uiChannelGain1", "1");
        (void)graph.SetMeta("bus1", "uiChannelGain0", "1");
        (void)graph.SetMeta("bus1", "uiChannelGain1", "1");

        (void)graph.SetMeta("src1", "uiRouteMask:bus1", "1");   // L only
        float maskL[8] = {};
        CHECK(live.Render(maskL, 4, 2, 48000));
        CHECK(maskL[0] > 0.5f);        // the routed channel passes
        CHECK(maskL[1] == 0.0f);       // the unrouted one is silent

        (void)graph.SetMeta("src1", "uiRouteMask:bus1", "2");   // R only
        float maskR[8] = {};
        CHECK(live.Render(maskR, 4, 2, 48000));
        CHECK(maskR[0] == 0.0f);
        CHECK(maskR[1] > 0.5f);

        // No channel routed into this bus: the row contributes nothing here...
        (void)graph.SetMeta("src1", "uiRouteMask:bus1", "0");   // == no mask
        float noMask[8] = {};
        CHECK(live.Render(noMask, 4, 2, 48000));
        CHECK(noMask[0] > 0.5f && noMask[1] > 0.5f);   // 0 means WHOLE SOURCE

        // ...while a mask with an unrouted BUS id leaves THIS bus untouched (the
        // mask is per bus, not per source).
        (void)graph.SetMeta("src1", "uiRouteMask:bus9#a", "0");
        float otherBus[8] = {};
        CHECK(live.Render(otherBus, 4, 2, 48000));
        CHECK(otherBus[0] > 0.5f && otherBus[1] > 0.5f);

        // A mask that routes NEITHER side into this bus is real silence for it.
        (void)graph.SetMeta("src1", "uiRouteMask:bus1", "4");   // a channel bus1 has not
        float routedElsewhere[8] = {};
        CHECK(!live.Render(routedElsewhere, 4, 2, 48000));
        CHECK(routedElsewhere[0] == 0.0f && routedElsewhere[1] == 0.0f);
        (void)graph.SetMeta("src1", "uiRouteMask:bus1", "0");   // back to whole-source
    }

    // ---- a VIDEO row's own audio reaches the mix -------------------------
    // A bus is a PAIR of planes ("bus:N#a" audio, "bus:N#v" video — the ids
    // BusListModel derives). A video row routes to the VIDEO plane, so before
    // this the audio side of a bus whose only source was a media/NDI row summed
    // NOTHING: the card's pills and the bus's meters sat dark however loudly the
    // file played. A video row's audio (a media file's track, an NDI stream's
    // embedded audio) is real audio the bus owes its mix, so the audio plane's
    // feed set includes its paired video plane's sources.
    {
        // The ids are the real ones BusListModel builds for a bus: one number,
        // two plane suffixes ("bus:1#a" audio / "bus:1#v" video).
        pr::ProductionGraph paired;
        CHECK(paired.AddBus("bus:1#a", "Both", pr::BusRole::Custom, pr::SignalType::Audio).ok());
        CHECK(paired.AddBus("bus:1#v", "Both", pr::BusRole::Custom, pr::SignalType::Video).ok());
        // A fresh bus node starts muted (the UI's own default); open both so the
        // check reads the FEED rule, not the mute default.
        (void)paired.SetVolume("bus:1#a", pr::VolumeControl{});
        (void)paired.SetVolume("bus:1#v", pr::VolumeControl{});
        CHECK(paired.AddSource("vsrc:1", "Media", pr::SignalType::Video).ok());
        CHECK(paired.Connect("vsrc:1", "bus:1#v", pr::SignalType::Video).ok());

        StubTapAudio vtaps;
        pr::AudioMixer vmix;
        vmix.Attach(&paired);
        vmix.AttachTaps(&vtaps);

        // Unbound (the bridge has no tap for the row yet): silence — never a
        // synthetic carrier.
        float none[8] = {};
        CHECK(!vmix.Render(none, 4, 2, 48000));
        CHECK(none[0] == 0.0f && none[1] == 0.0f);

        // Bound (the bridge's stream tap for the media/NDI row): the bus's audio
        // plane sums it, exactly as it sums a device-fed audio source.
        vmix.BindSourceDevice("vsrc:1", bps::platform::IAudio::kStreamTapBase);
        float mixed[8] = {};
        CHECK(vmix.Render(mixed, 4, 2, 48000));
        CHECK(mixed[0] > 0.5f);
        CHECK(mixed[1] > 0.5f);
        const auto vlevel = vmix.ProgramLevel();
        CHECK(vlevel.channelCount == 2);
        CHECK(vlevel.peaks[0] > 0.5f);   // the bus's own meters read it too

        // ...and the paired rule must not resurrect a source the route dropped:
        // with the video edge cut the same binding contributes nothing.
        CHECK(paired.Disconnect("vsrc:1", "bus:1#v").ok());
        float afterCut[8] = {};
        CHECK(!vmix.Render(afterCut, 4, 2, 48000));
        CHECK(afterCut[0] == 0.0f && afterCut[1] == 0.0f);
    }
}

// ---------------------------------------------------------------------------
// Dispatch scope: a bus dispatch runs with the engine lock RELEASED.
//
// This is the behavioural proof the lock-scope guard only approximates by
// reading source: the guard can tell that the `Publish` line sits outside a
// lock scope, but not that the lock is genuinely free while a subscriber runs.
// Here the subscriber PARKS, and a second thread uses the engine (every one of
// those calls needs the engine's mutex); if the dispatch still ran under the
// lock the second thread could not make progress until the subscriber
// returned. The regression this pins down is real, not theoretical: a
// subscriber that takes a device, a renderer or a disk write inside the lock
// stalls every other thread that needs that engine.
// ---------------------------------------------------------------------------
void TestProductionDispatchOutsideLock() {
    auto& eng = pr::ProductionEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());

    std::atomic<bool> inHandler{false};
    std::atomic<bool> otherThreadDone{false};
    std::atomic<bool> reachedEngineWhileParked{false};
    // The subscriber PARKS inside the dispatch, and the park is bounded on
    // purpose: whoever sets the release flag has to be a different thread than
    // the one that is parked here (this call is synchronous), so an unbounded
    // spin would wedge the whole test — the failure mode has to be a failed
    // CHECK, never a hang.
    Subscription sub = EventBus::Instance().Subscribe<events::ControlSignalReceived>(
        [&](const events::ControlSignalReceived&) {
            inHandler.store(true);
            for (int i = 0; i < 3000 && !otherThreadDone.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // Recorded BEFORE returning: did the other thread get through the
            // engine's own lock while this dispatch was still in flight?
            reachedEngineWhileParked.store(otherThreadDone.load());
        }, 0);

    std::thread other([&] {
        for (int i = 0; i < 5000 && !inHandler.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!inHandler.load()) return;   // the dispatch never happened
        (void)eng.Clock();                // every one of these takes the engine's lock
        (void)eng.ValidateProduction();
        (void)eng.MetricsSnapshot();
        otherThreadDone.store(true);
    });

    // Dispatches synchronously, so this call parks INSIDE the subscriber.
    CHECK(eng.SendControl("scope", "probe").ok());
    other.join();
    CHECK(inHandler.load());   // the subscriber really ran
    // The point of the hoist: with the lock held across the dispatch, none of
    // the other thread's calls could have completed while this one was parked.
    CHECK(reachedEngineWhileParked.load());

    (void)EventBus::Instance().Unsubscribe(sub);
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}
