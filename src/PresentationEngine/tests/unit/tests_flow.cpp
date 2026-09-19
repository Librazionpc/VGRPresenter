// Unit tests: Service Flow & Automation Engine (docs/specs/26).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests flow
#include "TestHarness.hpp"

namespace {
struct FlowFailAlways final : fa::IAction {
    const char* Type() const noexcept override { return "fail_always"; }
    Result<fa::ActionOutcome> Execute(fa::AutomationContext&) override {
        return fa::ActionOutcome{fa::ActionOutcome::Kind::Failed, {}, "boom"};
    }
};
std::shared_ptr<fa::IAction> MakeFlowFailAlways() { return std::make_shared<FlowFailAlways>(); }
struct FlowBump final : fa::IAction {
    static std::atomic<int> count;
    const char* Type() const noexcept override { return "bump"; }
    Result<fa::ActionOutcome> Execute(fa::AutomationContext&) override {
        count.fetch_add(1);
        return fa::ActionOutcome{fa::ActionOutcome::Kind::Completed, {}, {}};
    }
};
std::atomic<int> FlowBump::count{0};
std::shared_ptr<fa::IAction> MakeFlowBump() { return std::make_shared<FlowBump>(); }
} // namespace
void TestFlowModel() {
    auto& flow = fa::FlowEngine::Instance();
    CHECK(flow.Initialize().ok());
    CHECK(flow.Start().ok());
    CHECK(flow.ActionNames().size() >= 4);
    CHECK(flow.ConditionNames().size() >= 6);
    CHECK(flow.TriggerNames().size() >= 3);

    // A valid Sunday-Service flow as a .vgr document.
    const char* sunday =
        "{\"type\":\"flow\",\"flow\":{"
        "\"id\":\"sunday\",\"name\":\"Sunday Service\","
        "\"variables\":{\"SERVICE_NAME\":\"Morning Service\"},"
        "\"nodes\":["
        "{\"id\":\"countdown\",\"label\":\"Countdown\",\"actionType\":\"log\","
        "\"actionPayload\":\"{SERVICE_NAME} in 10:00\"},"
        "{\"id\":\"welcome\",\"label\":\"Welcome\",\"actionType\":\"noop\"},"
        "{\"id\":\"end\",\"label\":\"End\",\"kind\":\"end\"}"
        "]}}";
    auto id = flow.Load(sunday, "vgr");
    CHECK(id.ok() && id.value() == "sunday");
    CHECK(flow.FlowCount() == 1);
    CHECK(flow.GetFlow("sunday").ok());

    // Save -> reload round trip (json) into a fresh id; vgr carries the marker.
    auto saved = flow.Save(flow.GetFlow("sunday").value(), "json");
    CHECK(saved.ok() && !saved.value().empty());
    auto reloaded = flow.Load(saved.value(), "json", "sunday2");
    CHECK(reloaded.ok() && reloaded.value() == "sunday2");
    auto vgr = flow.Save(flow.GetFlow("sunday").value(), "vgr");
    CHECK(vgr.ok() && vgr.value().find("\"type\":\"flow\"") != std::string::npos);

    // Duplicate / bad formats.
    CHECK(!flow.Load(sunday, "vgr").ok());
    CHECK(flow.Load(sunday, "vgr").error().code == Err::Flow_AlreadyExists);
    CHECK(!flow.Load("{}", "zzz").ok());
    CHECK(flow.Load("{}", "zzz").error().code == Err::Flow_UnsupportedFormat);

    // Validation rejections: duplicate ids, unknown action/condition, bad targets.
    const char* dup = "{\"id\":\"dup\",\"name\":\"D\",\"nodes\":["
                      "{\"id\":\"a\",\"actionType\":\"noop\"},{\"id\":\"a\",\"actionType\":\"noop\"}]}";
    CHECK(!flow.Load(dup, "json").ok());
    const char* badAction = "{\"id\":\"ba\",\"name\":\"BA\",\"nodes\":["
                            "{\"id\":\"a\",\"actionType\":\"does_not_exist\"}]}";
    CHECK(!flow.Load(badAction, "json").ok());
    CHECK(flow.Load(badAction, "json").error().code == Err::Flow_ValidationFailed);
    const char* badCond = "{\"id\":\"bc\",\"name\":\"BC\",\"nodes\":["
                          "{\"id\":\"a\",\"kind\":\"condition\",\"conditionType\":\"nope\"}]}";
    CHECK(!flow.Load(badCond, "json").ok());
    const char* badTarget = "{\"id\":\"bt\",\"name\":\"BT\",\"nodes\":["
                            "{\"id\":\"c\",\"kind\":\"condition\",\"conditionType\":\"true\","
                            "\"trueNodeId\":\"missing\"}]}";
    CHECK(!flow.Load(badTarget, "json").ok());
    const char* noNodes = "{\"id\":\"nn\",\"name\":\"NN\",\"nodes\":[]}";
    CHECK(!flow.Load(noNodes, "json").ok());
    const char* noId = "{\"name\":\"NI\",\"nodes\":[{\"id\":\"a\",\"actionType\":\"noop\"}]}";
    CHECK(!flow.Load(noId, "json").ok());

    // Templates: create, apply, derived flow is independent.
    CHECK(flow.CreateTemplate("Sunday Tpl", flow.GetFlow("sunday").value()).ok());
    CHECK(flow.Templates().size() == 1);
    auto applied = flow.ApplyTemplate("Sunday Tpl", "sunday-copy");
    CHECK(applied.ok() && applied.value().id == "sunday-copy");
    CHECK(applied.ok() && applied.value().templateId == "Sunday Tpl");
    CHECK(applied.ok() && applied.value().nodes.size() == 3);
    CHECK(!flow.ApplyTemplate("nope", "").ok());

    // Macros: reusable action sequences.
    std::vector<std::pair<std::string, std::string>> macro{{"noop", ""}, {"noop", ""}};
    CHECK(flow.RegisterMacro("double_noop", macro).ok());
    CHECK(flow.RunMacro("double_noop").ok());
    CHECK(!flow.RunMacro("nope").ok());
    CHECK(flow.RunMacro("nope").error().code == Err::Flow_MacroNotFound);

    CHECK(flow.RemoveFlow("sunday2").ok());
    CHECK(!flow.RemoveFlow("sunday2").ok());
    CHECK(flow.Stop().ok());
    CHECK(flow.Shutdown().ok());
}
void TestFlowEngine() {
    auto& flow = fa::FlowEngine::Instance();
    CHECK(flow.Initialize().ok());
    CHECK(flow.Start().ok());
    (void)flow.RegisterAction("fail_always", &MakeFlowFailAlways);
    (void)flow.RegisterAction("bump", &MakeFlowBump);

    // Event-driven service flow: Welcome -> WAIT media.state_changed -> Song.
    const char* service =
        "{\"id\":\"svc\",\"name\":\"Service\","
        "\"nodes\":["
        "{\"id\":\"welcome\",\"label\":\"Welcome\",\"actionType\":\"bump\"},"
        "{\"id\":\"video\",\"label\":\"Video\",\"kind\":\"wait\",\"waitFor\":\"media.state_changed\"},"
        "{\"id\":\"song\",\"label\":\"Worship\",\"actionType\":\"bump\"},"
        "{\"id\":\"end\",\"label\":\"End\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(service, "json").ok());

    FlowBump::count.store(0);
    auto exec = flow.Start("svc");
    CHECK(exec.ok());
    std::string e1 = exec.value();
    // Welcome ran; the video wait is pending.
    auto st = flow.ExecutionState(e1);
    CHECK(st.ok() && st.value().state == fa::FlowState::Running);
    CHECK(st.ok() && st.value().waitingFor == "media.state_changed");
    CHECK(st.ok() && st.value().currentNodeId == "video");
    CHECK(FlowBump::count.load() == 1);

    // Pause / resume.
    CHECK(flow.Pause(e1).ok());
    CHECK(flow.ExecutionState(e1).value().state == fa::FlowState::Paused);
    CHECK(!flow.Pause(e1).ok());   // not running -> InvalidState
    CHECK(flow.Resume(e1).ok());

    // The video completes via a real EventBus event -> the song starts.
    CHECK(EventBus::Instance().Publish(events::MediaStateChanged{"v1", "intro", 1, 0.0, 10.0}).ok());
    auto st2 = flow.ExecutionState(e1);
    CHECK(st2.ok() && st2.value().state == fa::FlowState::Completed);
    CHECK(FlowBump::count.load() == 2);

    // Complete execution history: welcome, video, song, end.
    auto hist = flow.History(e1);
    CHECK(hist.ok() && hist.value().size() == 4);
    CHECK(hist.ok() && hist.value()[0].nodeId == "welcome");
    CHECK(hist.ok() && hist.value()[1].nodeId == "video" &&
          hist.value()[1].state == fa::NodeState::Completed);
    CHECK(hist.ok() && hist.value()[2].nodeId == "song");
    CHECK(hist.ok() && hist.value()[3].nodeId == "end");

    // Variables + data-binding event.
    int varEvents = 0;
    Subscription vsub = EventBus::Instance().Subscribe<events::VariableChanged>(
        [&](const events::VariableChanged& e) {
            if (e.name == "SPEAKER_NAME") varEvents++;
        }, 0);
    CHECK(flow.SetVariable(e1, "SPEAKER_NAME", "Brother Branham").ok());
    CHECK(flow.GetVariable(e1, "SPEAKER_NAME").value() == "Brother Branham");
    CHECK(flow.GetVariable(e1, "NOPE").error().code == Err::Flow_VariableNotFound);
    CHECK(varEvents == 1);
    (void)EventBus::Instance().Unsubscribe(vsub);

    // Manual override: skip a wait, jump, stop, restart.
    auto e2 = flow.Start("svc");
    CHECK(e2.ok());
    CHECK(flow.Skip(e2.value()).ok());          // skip the video wait
    CHECK(flow.ExecutionState(e2.value()).value().state == fa::FlowState::Completed);

    auto e3 = flow.Start("svc");
    CHECK(e3.ok());
    CHECK(flow.JumpTo(e3.value(), "end").ok());  // jump straight to the End cue
    CHECK(flow.ExecutionState(e3.value()).value().state == fa::FlowState::Completed);
    CHECK(!flow.JumpTo(e3.value(), "missing").ok());

    auto e4 = flow.Start("svc");
    CHECK(e4.ok());
    CHECK(flow.Stop(e4.value()).ok());
    CHECK(flow.ExecutionState(e4.value()).value().state == fa::FlowState::Stopped);
    CHECK(flow.Stop(e4.value()).error().code == Err::Flow_InvalidState);
    auto e4r = flow.Restart(e4.value());
    CHECK(e4r.ok() && !e4r.value().empty());
    CHECK(flow.Stop(e4r.value()).ok());

    auto e5 = flow.Start("svc");
    CHECK(e5.ok());
    CHECK(flow.EmergencyStop().ok());
    CHECK(flow.ExecutionState(e5.value()).value().state == fa::FlowState::EmergencyStopped);
    CHECK(flow.ActiveExecutions().empty());

    // Standalone action execution + unknown action error.
    CHECK(flow.ExecuteAction("bump", "").ok());
    CHECK(!flow.ExecuteAction("nope", "").ok());
    CHECK(flow.ExecuteAction("nope", "").error().code == Err::Flow_ActionNotFound);

    // Conditional branching: GPU_AVAILABLE picks the high-quality path only.
    const char* branch =
        "{\"id\":\"branch\",\"name\":\"Branch\","
        "\"variables\":{\"GPU_AVAILABLE\":\"1\"},"
        "\"nodes\":["
        "{\"id\":\"check\",\"kind\":\"condition\",\"conditionType\":\"gpu_available\","
        "\"trueNodeId\":\"gpu\",\"falseNodeId\":\"lite\"},"
        "{\"id\":\"lite\",\"label\":\"Light\",\"actionType\":\"bump\",\"nextNodeId\":\"end\"},"
        "{\"id\":\"gpu\",\"label\":\"High\",\"actionType\":\"bump\",\"nextNodeId\":\"end\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(branch, "json").ok());
    FlowBump::count.store(0);
    auto eb = flow.Start("branch");
    CHECK(eb.ok());
    CHECK(flow.ExecutionState(eb.value()).value().state == fa::FlowState::Completed);
    CHECK(FlowBump::count.load() == 1);
    CHECK(flow.History(eb.value()).value()[1].nodeId == "gpu");

    // output_is condition takes the false (stream) path.
    const char* branch2 =
        "{\"id\":\"branch2\",\"name\":\"Branch2\","
        "\"variables\":{\"OUTPUT\":\"stream\"},"
        "\"nodes\":["
        "{\"id\":\"check\",\"kind\":\"condition\",\"conditionType\":\"output_is\","
        "\"conditionPayload\":\"audience\",\"trueNodeId\":\"aud\",\"falseNodeId\":\"stream\"},"
        "{\"id\":\"aud\",\"actionType\":\"bump\",\"nextNodeId\":\"end\"},"
        "{\"id\":\"stream\",\"actionType\":\"bump\",\"nextNodeId\":\"end\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(branch2, "json").ok());
    FlowBump::count.store(0);
    auto eb2 = flow.Start("branch2");
    CHECK(eb2.ok());
    CHECK(flow.ExecutionState(eb2.value()).value().state == fa::FlowState::Completed);
    CHECK(FlowBump::count.load() == 1);
    CHECK(flow.History(eb2.value()).value()[1].nodeId == "stream");

    // Parallel burst: three members run in one step.
    const char* par =
        "{\"id\":\"par\",\"name\":\"Parallel\","
        "\"nodes\":["
        "{\"id\":\"p\",\"kind\":\"parallel\",\"parallelNodeIds\":[\"b1\",\"b2\",\"b3\"]},"
        "{\"id\":\"b1\",\"actionType\":\"bump\"},"
        "{\"id\":\"b2\",\"actionType\":\"bump\"},"
        "{\"id\":\"b3\",\"actionType\":\"bump\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(par, "json").ok());
    FlowBump::count.store(0);
    auto ep = flow.Start("par");
    CHECK(ep.ok());
    CHECK(flow.ExecutionState(ep.value()).value().state == fa::FlowState::Completed);
    CHECK(FlowBump::count.load() == 3);

    // Timeline / delay nodes advance deterministically via Poll.
    const char* delay =
        "{\"id\":\"dl\",\"name\":\"Delay\","
        "\"nodes\":["
        "{\"id\":\"d\",\"label\":\"Countdown\",\"kind\":\"delay\",\"actionPayload\":\"30\"},"
        "{\"id\":\"go\",\"actionType\":\"bump\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(delay, "json").ok());
    FlowBump::count.store(0);
    auto ed = flow.Start("dl");
    CHECK(ed.ok());
    CHECK(flow.ExecutionState(ed.value()).value().waitingFor == "timer:delay");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(flow.Poll(ed.value()).ok());
    CHECK(flow.ExecutionState(ed.value()).value().state == fa::FlowState::Completed);
    CHECK(FlowBump::count.load() == 1);

    // Failure policy: skip.
    const char* failSkip =
        "{\"id\":\"fskip\",\"name\":\"FSkip\","
        "\"nodes\":["
        "{\"id\":\"f\",\"actionType\":\"fail_always\",\"onFailure\":\"skip\"},"
        "{\"id\":\"ok\",\"actionType\":\"bump\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(failSkip, "json").ok());
    FlowBump::count.store(0);
    auto ef = flow.Start("fskip");
    CHECK(ef.ok());
    CHECK(flow.ExecutionState(ef.value()).value().state == fa::FlowState::Completed);
    CHECK(flow.History(ef.value()).value()[0].state == fa::NodeState::Skipped);
    CHECK(FlowBump::count.load() == 1);

    // Failure policy: fallback to another cue.
    const char* failFallback =
        "{\"id\":\"ffb\",\"name\":\"FFB\","
        "\"nodes\":["
        "{\"id\":\"f\",\"actionType\":\"fail_always\",\"onFailure\":\"fallback\","
        "\"fallbackNodeId\":\"ok\"},"
        "{\"id\":\"ok\",\"actionType\":\"bump\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(failFallback, "json").ok());
    FlowBump::count.store(0);
    auto efb = flow.Start("ffb");
    CHECK(efb.ok());
    CHECK(flow.ExecutionState(efb.value()).value().state == fa::FlowState::Completed);
    CHECK(FlowBump::count.load() == 1);
    CHECK(flow.History(efb.value()).value()[1].nodeId == "ok");

    // Failure policy: retry (exhausts -> stops).
    const char* failRetry =
        "{\"id\":\"fretry\",\"name\":\"FRetry\","
        "\"nodes\":["
        "{\"id\":\"f\",\"actionType\":\"fail_always\",\"onFailure\":\"retry\",\"retryLimit\":2},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(failRetry, "json").ok());
    auto er = flow.Start("fretry");
    CHECK(er.ok());
    CHECK(flow.ExecutionState(er.value()).value().state == fa::FlowState::Stopped);

    // Failure policy: pause (operator skips the failing cue and continues).
    const char* failPause =
        "{\"id\":\"fpause\",\"name\":\"FPause\","
        "\"nodes\":["
        "{\"id\":\"f\",\"actionType\":\"fail_always\",\"onFailure\":\"pause\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(failPause, "json").ok());
    auto epz = flow.Start("fpause");
    CHECK(epz.ok());
    CHECK(flow.ExecutionState(epz.value()).value().state == fa::FlowState::Paused);
    CHECK(flow.Skip(epz.value()).ok());   // operator skips the failing cue
    CHECK(flow.ExecutionState(epz.value()).value().state == fa::FlowState::Completed);

    // Failure policy: abort.
    const char* failAbort =
        "{\"id\":\"fabort\",\"name\":\"FAbort\","
        "\"nodes\":["
        "{\"id\":\"f\",\"actionType\":\"fail_always\",\"onFailure\":\"abort\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(failAbort, "json").ok());
    auto ea = flow.Start("fabort");
    CHECK(ea.ok());
    CHECK(flow.ExecutionState(ea.value()).value().state == fa::FlowState::Stopped);

    // Recovery: snapshot a waiting execution, restore it as a fresh execution.
    const char* rec =
        "{\"id\":\"rec\",\"name\":\"Rec\","
        "\"nodes\":["
        "{\"id\":\"a\",\"actionType\":\"bump\"},"
        "{\"id\":\"w\",\"kind\":\"wait\",\"waitFor\":\"presentation.completed\"},"
        "{\"id\":\"b\",\"actionType\":\"bump\"},"
        "{\"id\":\"end\",\"kind\":\"end\"}"
        "]}";
    CHECK(flow.Load(rec, "json").ok());
    FlowBump::count.store(0);
    auto er1 = flow.Start("rec");
    CHECK(er1.ok());
    CHECK(FlowBump::count.load() == 1);
    auto snap = flow.Snapshot(er1.value());
    CHECK(snap.ok() && snap.value().pc == 1);
    CHECK(snap.ok() && snap.value().waitingFor == "presentation.completed");
    // Simulate a crash: retire the original, recover into a fresh execution id.
    (void)flow.Stop(er1.value());
    auto snap2 = snap.value();
    snap2.executionId = "rec-recovered";
    auto er2 = flow.Recover(snap2);
    CHECK(er2.ok() && er2.value() == "rec-recovered");
    auto sRec = flow.ExecutionState(er2.value());
    CHECK(sRec.ok() && sRec.value().waitingFor == "presentation.completed");
    CHECK(sRec.ok() && sRec.value().history.size() == 1);   // "a" rebuilt
    CHECK(EventBus::Instance().Publish(events::PresentationCompleted{"p1"}).ok());
    auto sRec2 = flow.ExecutionState(er2.value());
    CHECK(sRec2.ok() && sRec2.value().state == fa::FlowState::Completed);
    CHECK(FlowBump::count.load() == 2);

    // Invalid recovery target.
    fa::FlowSnapshot badSnap;
    badSnap.flowId = "nope";
    CHECK(!flow.Recover(badSnap).ok());
    CHECK(flow.Recover(badSnap).error().code == Err::Flow_NotFound);

    CHECK(flow.Stop().ok());
    CHECK(flow.Shutdown().ok());
}
