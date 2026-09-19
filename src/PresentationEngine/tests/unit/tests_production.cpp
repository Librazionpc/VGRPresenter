// Unit tests: Extended Production Engine (docs/specs/27).
// Split so a single phase can run alone:  ./bps_unit_tests production
#include "TestHarness.hpp"

#include "modules/production/ProductionEngine.hpp"
#include "modules/production/ProductionGraph.hpp"

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
} // namespace

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
