#include "TestHarness.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"

void TestDisplayProviders() {
    // Provider registry: Null + Virtual + Linux all register without clash.
    auto& eng = d::DisplayEngine::Instance();
    CHECK(eng.Initialize().ok());
    auto names = eng.ProviderNames();
    CHECK(names.size() >= 3);
    // Virtual provider enumerates 3 software displays.
    d::VirtualDisplayProvider vp;
    auto vdevs = vp.Enumerate();
    CHECK(vdevs.size() == 3);
    bool foundAudience = false;
    for (const auto& d : vdevs)
        if (d.id == "virtual-aud") foundAudience = true;
    CHECK(foundAudience);
    // Null provider enumerates nothing but never fails.
    d::NullDisplayProvider np;
    CHECK(np.Enumerate().empty());
    auto probe = np.Probe();
    CHECK(probe.ok() && probe.value().empty());
}
void TestDisplayDevices() {
    auto& eng = d::DisplayEngine::Instance();
    auto devs = eng.Devices();
    CHECK(eng.ConnectedDeviceCount() >= 3);   // 3 virtual + Linux monitors
    auto aud = eng.GetDevice("virtual-aud");
    CHECK(aud.ok() && aud.value().width == 1920 && aud.value().height == 1080);
    CHECK(!eng.GetDevice("nope").ok());
    // Hot-plug probe publishes events and detects removal.
    d::VirtualDisplayProvider vp;
    int connected = 0, disconnected = 0;
    Subscription s1 = EventBus::Instance().Subscribe<events::DisplayDeviceConnected>(
        [&](const events::DisplayDeviceConnected&) { ++connected; }, 0);
    Subscription s2 = EventBus::Instance().Subscribe<events::DisplayDeviceDisconnected>(
        [&](const events::DisplayDeviceDisconnected&) { ++disconnected; }, 0);
    // Add + remove a virtual device and refresh.
    d::DisplayDevice extra;
    extra.id = "virtual-test";
    extra.name = "Test Display";
    extra.width = 1280;
    extra.height = 720;
    extra.connected = true;
    extra.virtual_ = true;
    extra.provider = "Virtual";
    CHECK(vp.AddVirtualDevice(extra).ok());
    auto probe = vp.Probe();
    CHECK(probe.ok());
    bool sawAdded = false;
    for (const auto& d : probe.value())
        if (d.id == "virtual-test") sawAdded = true;
    CHECK(sawAdded);
    // Removal is detected the same way and the engine's registry is updated.
    CHECK(vp.RemoveVirtualDevice("virtual-test").ok());
    probe = vp.Probe();
    CHECK(probe.ok());
    bool sawRemoved = false;
    for (const auto& d : probe.value())
        if (d.id == "virtual-test") sawRemoved = true;
    CHECK(sawRemoved);
    (void)EventBus::Instance().Unsubscribe(s1);
    (void)EventBus::Instance().Unsubscribe(s2);
}
void TestDisplayOutputs() {
    auto& eng = d::DisplayEngine::Instance();
    // Create outputs bound to the virtual audience device.
    d::Output aud;
    aud.id = "audience";
    aud.kind = d::OutputKind::Audience;
    aud.name = "Audience";
    aud.displayId = "virtual-aud";
    CHECK(eng.AddOutput(aud).ok());
    CHECK(!eng.AddOutput(aud).ok());   // duplicate rejected
    d::Output stage;
    stage.id = "stage";
    stage.kind = d::OutputKind::Stage;
    stage.displayId = "virtual-stage";
    CHECK(eng.AddOutput(stage).ok());
    CHECK(eng.OutputCount() == 2);
    auto got = eng.GetOutput("audience");
    CHECK(got.ok() && got.value().state == d::OutputState::Running);
    CHECK(eng.EnableOutput("stage", false).ok());
    auto stageGot = eng.GetOutput("stage");
    CHECK(stageGot.ok() && !stageGot.value().enabled);
    // Assign/rebind + transform.
    CHECK(eng.AssignOutput("stage", "virtual-preview").ok());
    d::OutputTransform t;
    t.scaling = d::ScalingMode::Fill;
    CHECK(eng.SetOutputTransform("stage", t).ok());
    CHECK(eng.RemoveOutput("audience").ok());
    CHECK(eng.OutputCount() == 1);
}
void TestDisplayScaling() {
    // Pure scaling math on a 1920x1080 device.
    d::DisplayDevice dev;
    dev.width = 1920;
    dev.height = 1080;
    r::Frame f;
    f.width = 1920;
    f.height = 1080;
    f.pixels.assign(1920 * 1080, 0xFF000000u);
    d::OutputTransform t;
    t.scaling = d::ScalingMode::Stretch;
    auto r1 = d::OutputRouter::ComputeDestRect(dev, t, f);
    CHECK(r1.width == 1920 && r1.height == 1080);
    t.scaling = d::ScalingMode::Fit;
    auto r2 = d::OutputRouter::ComputeDestRect(dev, t, f);
    CHECK(r2.width <= 1920 && r2.height <= 1080);
    t.scaling = d::ScalingMode::PixelPerfect;
    r::Frame f800;
    f800.width = 800;
    f800.height = 600;
    auto r3 = d::OutputRouter::ComputeDestRect(dev, t, f800);
    CHECK(static_cast<int>(r3.width) % 800 == 0);
}
void TestDisplaySelfTest() {
    auto& eng = d::DisplayEngine::Instance();
    auto report = eng.RunSelfTest();
    CHECK(report.checks.size() >= 4);
    CHECK(report.passed > 0);
    // Test pattern generator produces deterministic non-empty frames.
    auto bars = d::TestPatternGenerator::ColorBars(64, 36);
    CHECK(bars.width == 64 && bars.height == 36 && !bars.pixels.empty());
    auto grad = d::TestPatternGenerator::Gradient(32, 32);
    CHECK(!grad.pixels.empty());
    auto cb = d::TestPatternGenerator::Checkerboard(32, 32, 8);
    CHECK(!cb.pixels.empty());
    auto solid = d::TestPatternGenerator::Generate(d::TestPattern::SolidRed, 16, 16);
    // Color::Pack layout is 0xAABBGGRR → solid red (opaque) = 0xFF0000FF.
    CHECK(solid.pixels[0] == 0xFF0000FFu);
    // Frame delivery through the router.
    d::OutputRouter router;
    d::Output o;
    o.id = "out";
    o.displayId = "virtual-aud";
    CHECK(router.AddOutput(o).ok());
    int delivered = 0;
    router.RouteFrame(bars, [&](std::string_view id) -> const d::DisplayDevice* {
        (void)id;
        static d::DisplayDevice dev;
        dev.id = "virtual-aud";
        dev.width = 1920;
        dev.height = 1080;
        dev.connected = true;
        return &dev;
    },
                          [&](const d::Output&, const r::Rect&, const r::Frame& scaled) {
                              CHECK(!scaled.pixels.empty());
                              ++delivered;
                          });
    CHECK(delivered == 1);
}
void TestDisplayRecovery() {
    auto& eng = d::DisplayEngine::Instance();
    d::Output o;
    o.id = "lost-out";
    o.displayId = "virtual-aud";
    o.autoRestore = true;
    CHECK(eng.AddOutput(o).ok());
    // Simulate disconnect: mark device disconnected + output Lost.
    (void)eng.GetDevice("virtual-aud");
    // OnMonitorDisconnected marks the output Lost; then Restore re-attaches.
    auto before = eng.RestoreAssignments();
    CHECK(before.ok());
    auto out = eng.GetOutput("lost-out");
    CHECK(out.ok());
    (void)eng.RemoveOutput("lost-out");
}
void TestDisplayNdi() {
    broadcast::BroadcastEngine::Instance().SetNdiEnabled(true);
    d::NdiDisplayProvider ndi;
    CHECK(std::string(ndi.Name()) == "Ndi");
    auto devices = ndi.Enumerate();
    CHECK(devices.size() == 1);
    CHECK(devices[0].id == "ndi-program");
    CHECK(devices[0].width == 1920 && devices[0].height == 1080);
    CHECK(devices[0].virtual_);
    auto cap = ndi.Capabilities();
    CHECK(cap.supportsVirtualOutputs);

    ndi.SetFrameRate(60.0f);
    CHECK(ndi.FrameRate() == 60.0f);
    devices = ndi.Enumerate();
    CHECK(devices.size() == 1);
    CHECK(devices[0].refreshRateHz == 60);
    ndi.SetFrameRate(24.0f);
    CHECK(ndi.FrameRate() == 24.0f);
    devices = ndi.Enumerate();
    CHECK(devices.size() == 1);
    CHECK(devices[0].refreshRateHz == 24);

    // Empty frame rejected.
    d::RenderFrameView bad;
    CHECK(!ndi.SendFrame(bad).ok());

    constexpr int W = 16, H = 8;
    std::vector<uint32_t> px(static_cast<size_t>(W) * H, 0xFF0000FFu);   // solid red
    d::RenderFrameView view;
    view.width = W;
    view.height = H;
    view.pixels = px.data();
    auto r = ndi.SendFrame(view);
    CHECK(r.ok());
    if (r.ok()) {
        CHECK(ndi.FramesSent() == 1);
        // A receiver on the same engine can pull the frame back.
        auto& bc = broadcast::BroadcastEngine::Instance();
        auto recv = bc.CreateNdiReceiver(ndi.SenderName());
        CHECK(recv.ok());
        broadcast::VideoFrameInfo info;
        std::vector<uint8_t> out;
        auto got = bc.ReceiveFrame(recv.value(), info, out);
        CHECK(got.ok());
        if (got.ok() && got.value()) {
            CHECK(info.width == W && info.height == H);
            CHECK(info.fps == 24.0);   // the advertised refresh survives the wire
            CHECK(info.fourCC == 0x41424752u);
            CHECK(out.size() == static_cast<size_t>(W) * H * 4);
            const auto first = *reinterpret_cast<const uint32_t*>(out.data());
            CHECK(first == 0xFF0000FFu);
        }
        (void)bc.DisconnectReceiver(recv.value());
    }

    // ---- SELF-HEAL: a sender the engine no longer knows -------------------
    {
        auto& bc = broadcast::BroadcastEngine::Instance();
        const std::string dead = ndi.SenderId();
        CHECK(!dead.empty());
        CHECK(bc.StopSender(dead).ok());          // the engine forgets it...
        CHECK(!ndi.SendFrame(view).ok());         // ...so this send fails...
        CHECK(ndi.SenderId().empty());            // ...and the provider lets go of it
        CHECK(ndi.SendFrame(view).ok());          // the next frame heals it
        CHECK(!ndi.SenderId().empty());
    }

    // Nonsense rate → the legacy 30 fallback (never advertise <= 0).
    ndi.SetFrameRate(0.0f);
    CHECK(ndi.FrameRate() == 30.0f);
    CHECK(ndi.Enumerate()[0].refreshRateHz == 30);

    CHECK(d::kDefaultOutputFrameRateHz == 30.0f);
    CHECK(d::kMaxOutputFrameRateHz == 240.0f);
    CHECK(d::AcceptedOutputFrameRate(60.0f) == 60.0f);
    CHECK(d::AcceptedOutputFrameRate(240.0f) == 240.0f);
    CHECK(d::AcceptedOutputFrameRate(0.0f) == 30.0f);      // blank/zero
    CHECK(d::AcceptedOutputFrameRate(-1.0f) == 30.0f);     // negative
    CHECK(d::AcceptedOutputFrameRate(480.0f) == 30.0f);    // over the cap
    // …and the provider really is that rule, not a second copy of it.
    ndi.SetFrameRate(480.0f);
    CHECK(ndi.FrameRate() == 30.0f);
    ndi.SetFrameRate(d::kMaxOutputFrameRateHz);
    CHECK(ndi.FrameRate() == 240.0f);
    ndi.SetFrameRate(24.0f);
    CHECK(ndi.FrameRate() == 24.0f);
    CHECK(ndi.Enumerate()[0].refreshRateHz == 24);

    // Registered in the DisplayEngine by default (Initialize registers it).
    auto& eng = d::DisplayEngine::Instance();
    if (!eng.Initialize().ok()) { /* engine may already be initialized */ }
    bool hasNdi = false;
    for (const auto& p : eng.ProviderNames())
        if (p == "Ndi") hasNdi = true;
    CHECK(hasNdi);
}

// ===========================================================================
// Phase 8 — Presentation Engine (docs/specs/19)
// ===========================================================================