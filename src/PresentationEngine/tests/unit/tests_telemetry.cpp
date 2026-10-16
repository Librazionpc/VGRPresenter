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

#include <chrono>
#include <thread>

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

    // --- The LIVE LOOP: rate, cost and the curve -----------------------------
    // The three meters above are percentages of the 60Hz target, which is the
    // wrong shape for a live show: a loop composing one frame a second reads as
    // ~2% next to a preview repainting at 60Hz, so "it gets slower as the
    // service goes on" had no number and had to be mined out of the log. The
    // loop reports each iteration as COMPOSED (a raster) or REPRESENTED (the
    // last composed frame copied to the outputs because nothing changed), and
    // this section pins both, the per-second sample they close out, and the
    // stop that clears the session.
    {
        tel.OnLiveStopped();                      // start from a known session
        tel.OnLiveFrame(120.0, true);             // a composed frame, and what it cost
        st::LiveStats s = tel.LiveSnapshot();
        CHECK(s.composed == 1);
        CHECK(s.represented == 0);
        CHECK(s.frameMs == 120.0);
        CHECK(s.sharedCopies == 0);

        // ...and the copy that replaced the old per-second re-raster is cheap.
        tel.OnLiveFrame(8.0, false);
        s = tel.LiveSnapshot();
        CHECK(s.represented == 1);
        CHECK(s.representMs == 8.0);
        // Per-output rasters replaced by that copy, reported as a RUNNING TOTAL
        // (the loop publishes it outside its own lock).
        tel.OnLiveSharedCopies(3);
        CHECK(tel.LiveSnapshot().sharedCopies == 3);

        // WHICH ELEMENT MADE THE FRAME EXPENSIVE: the composed frame's own cost
        // profile (RenderEngine's — the pixel work each layer/element asked for)
        // is published beside the iteration, published on the snapshot for the
        // caption, and CARRIED into each whole-second sample rather than counted
        // — a hotspot is a property of the scene, so a second with no raster of
        // its own (a copy) still names what the frame it copied was made of.
        tel.OnLiveCost("Images · Announcement video", "Images", "Announcement video", 71.0, 85.2,
                       41.0);
        s = tel.LiveSnapshot();
        CHECK(s.costElement == "Images · Announcement video");
        CHECK(s.costLayer == "Images");
        CHECK(s.costObject == "Announcement video");
        CHECK(s.costSharePct == 71.0);
        CHECK(s.costMs == 85.2);
        // ...and the part of the same frame the scene's own background took, so
        // a content share is not read as the whole picture.
        CHECK(s.costBackgroundPct == 41.0);

        // The CURVE: one sample per whole second of loop, closed out by the
        // first iteration after that second ends. The clock is the loop's own
        // (see OnLiveFrame) — a loop held off while the machine was busy must
        // not read as a slow loop — so the test waits real time for it.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        tel.OnLiveFrame(9.0, false);              // closes the second that just ended
        auto history = tel.LiveHistory();
        CHECK(history.size() == 1);
        if (!history.empty()) {
            CHECK(history[0].composedFps > 0.0);                    // 1 raster in the second
            CHECK(history[0].presentFps > history[0].composedFps);  // plus the copies
            CHECK(history[0].frameMs == 120.0);
            CHECK(history[0].representMs == 9.0);
            CHECK(history[0].sharedCopies == 3);
            CHECK(history[0].costElement == "Images · Announcement video");
            CHECK(history[0].costLayer == "Images");
            CHECK(history[0].costSharePct == 71.0);
            CHECK(history[0].costMs == 85.2);
            CHECK(history[0].costBackgroundPct == 41.0);
        }

        // STOP clears the session: the next go-live must not open on the last
        // service's curve (a load that is no longer there reads as a fault).
        tel.OnLiveStopped();
        CHECK(tel.LiveSnapshot().composed == 0);
        CHECK(tel.LiveSnapshot().represented == 0);
        CHECK(tel.LiveHistory().empty());
        // ...and with it the hotspot: naming a scene element after the session
        // that was showing it has ended would blame a show that is off air.
        CHECK(tel.LiveSnapshot().costElement.empty());
        CHECK(tel.LiveSnapshot().costMs == 0.0);
        CHECK(tel.LiveSnapshot().costBackgroundPct == 0.0);
    }

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
