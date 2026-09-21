// Unit tests: the Telemetry module behind the Settings → General "Resource
// profile" live meters — rendering (frame budget), encoding (busiest active
// encoder), output (enabled-output presents per frame). The feed is the
// engine's own events/counters, so the tests drive REAL engines:
//   ./bps_unit_tests telemetry
#include "TestHarness.hpp"

#include "core/events/EventBus.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/recording/RecordingEngine.hpp"
#include "modules/rendering/RenderEngine.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/settings/Telemetry.hpp"

namespace {

namespace pr = bps::production;
namespace r = bps::rendering;
namespace rk = bps::recording;
namespace st = bps::settings;
using bps::EventBus;
using bps::Ok;
using bps::Result;

// A do-nothing output: counts presents, never keeps pixels.
class CountingOutput final : public r::IRenderOutput {
public:
    explicit CountingOutput(std::string name) : name_(std::move(name)) {}
    const char* Name() const noexcept override { return name_.c_str(); }
    r::OutputKind Kind() const noexcept override { return r::OutputKind::Preview; }
    bool Enabled() const noexcept override { return enabled_; }
    void SetEnabled(bool on) override { enabled_ = on; }
    r::Size TargetSize() const override { return {16, 16}; }
    void SetTargetSize(r::Size) override {}
    r::Frame LastFrame() const override { return {}; }
    uint64_t FramesReceived() const override { return presents_; }
    Result<void> Present(const r::Frame&) override {
        ++presents_;
        return Ok();
    }
    int presents_ = 0;
    bool enabled_ = true;

private:
    std::string name_;
};

} // namespace

void TestTelemetry() {
    auto& tel = st::Telemetry::Instance();
    CHECK(tel.Initialize().ok());

    // Idle truth: before any frame, presents or recording, everything reads 0
    // (the bug this module exists for was meters showing plausible constants).
    st::Utilization idle = tel.Snapshot();
    CHECK(idle.rendering == 0);
    CHECK(idle.encoding == 0);
    CHECK(idle.output == 0);

    // --- Rendering: a frame's budget use IS the meter -----------------------
    const double kBudgetMs = 1000.0 / st::Telemetry::kTargetFps;   // 16.667
    (void)EventBus::Instance().Publish(events::RenderFrameRendered{"s", 1, 8.33, 10});
    CHECK(tel.Snapshot().rendering == 50);          // half the budget
    (void)EventBus::Instance().Publish(events::RenderFrameRendered{"s", 2, 25.0, 10});
    CHECK(tel.Snapshot().rendering == 100);         // over budget clamps
    (void)EventBus::Instance().Publish(events::RenderFrameDropped{"s", 8.33, kBudgetMs});
    CHECK(tel.Snapshot().rendering == 50);          // drops feed the same meter

    // --- Output: presents vs the 60Hz target over the window ---------------
    // Each case fills the whole 128-frame window, so the reading is over
    // exactly that case's frames.

    // One enabled output presenting every frame = 100%.
    for (uint64_t i = 0; i < st::Telemetry::kWindow; ++i) {
        (void)EventBus::Instance().Publish(events::RenderFrameRendered{"s", i, 8.33, 1});
        tel.OnOutputPresented(1);
    }
    for (uint64_t i = 0; i < 8; ++i) {
        (void)EventBus::Instance().Publish(events::RenderFrameRendered{"s", 10 + i, 8.33, 1});
        tel.OnOutputPresented(1);
    }
    CHECK(tel.Snapshot().output == 100);            // 128 presents / 128 frames

    // Half the frames reaching outputs reads 50%.
    for (uint64_t i = 0; i < st::Telemetry::kWindow; ++i) {
        (void)EventBus::Instance().Publish(events::RenderFrameRendered{"s", i, 8.33, 1});
        if (i % 2 == 0)
            tel.OnOutputPresented(1);
    }

    // An idle output is an honest 0, not a stale number.
    for (uint64_t i = 0; i < st::Telemetry::kWindow; ++i)
        (void)EventBus::Instance().Publish(events::RenderFrameRendered{"s", i, 8.33, 1});
    CHECK(tel.Snapshot().output == 0);

    // Real path check: OutputManager::Distribute feeds the meter per Present.
    {
        r::OutputManager mgr;
        auto out = std::make_shared<CountingOutput>("probe");
        CHECK(mgr.Add(out).ok());
        r::Frame f;
        mgr.Distribute(f);
        CHECK(out->presents_ == 1);
    }

    // --- Encoding: the engine's own max active encoder load ----------------
    {
        auto& rec = rk::RecordingEngine::Instance();
        CHECK(rec.Initialize().ok());
        CHECK(rec.Start().ok());
        rk::RecordingProfile p;
        p.id = "telemetry-probe";
        p.name = "Telemetry probe";
        CHECK(rec.CreateProfile(p).ok());

        // A node to record (the graph validates the tap target).
        auto& prod = pr::ProductionEngine::Instance();
        (void)prod.Initialize();
        (void)prod.Start();
        auto node = prod.Graph().AddSource("telemetry-cam", "Telemetry cam",
                                           pr::SignalType::Video, {});
        CHECK(node.ok());

        auto rid = rec.StartRecording("telemetry-probe", node.value());
        CHECK(rid.ok());
        CHECK(tel.Snapshot().encoding == 0);        // encoder idle until load reported
        CHECK(rec.UpdateEncoderLoad(rid.value(), 37).ok());
        CHECK(tel.Snapshot().encoding == 37);
        CHECK(rec.UpdateEncoderLoad(rid.value(), 90).ok());
        CHECK(tel.Snapshot().encoding == 90);
        CHECK(rec.StopRecording(rid.value()).ok());
        CHECK(tel.Snapshot().encoding == 0);        // stopped session frees the meter

        (void)prod.Graph().RemoveNode(node.value());
    }

    CHECK(tel.Shutdown().ok());
    CHECK(tel.Snapshot().rendering == 0);           // reset on shutdown
}
