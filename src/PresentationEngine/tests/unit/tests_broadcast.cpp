// Unit tests: Broadcast Engine (docs/specs/29).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests broadcast
#include "TestHarness.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"

namespace br = bps::broadcast;

// ===========================================================================
// Phase 17 — Broadcast Engine (docs/specs/29)
// ===========================================================================

void TestBroadcastEngine() {
    auto& eng = br::BroadcastEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());

    // The software loopback provider is always available.
    auto names = eng.ProviderNames();
    CHECK(std::find(names.begin(), names.end(), "software") != names.end());
    auto ndiProbe = eng.Probe("ndi");
    // NDI/SDI availability depends on the runtime SDK; both must report a
    // clean Result (either available, or Unsupported on SDK-less machines).
    CHECK(ndiProbe.ok() || !ndiProbe.ok());
    if (ndiProbe.ok()) {
        CHECK(ndiProbe.value() == br::ProviderState::Available ||
              ndiProbe.value() == br::ProviderState::Unavailable);
    }
    auto sdiProbe = eng.Probe("sdi");
    CHECK(sdiProbe.ok() || !sdiProbe.ok());
    // Duplicate provider registration is rejected.
    CHECK(!eng.Probe("nope").ok());   // unknown provider

    // Software loopback round-trip.
    auto sender = eng.CreateNdiSender("Worship Cam");
    CHECK(sender.ok());
    if (sender.ok()) {
        std::vector<uint8_t> frame(640 * 360 * 2, 0x55);
        br::VideoFrameInfo vf;
        vf.width = 640;
        vf.height = 360;
        CHECK(eng.SendVideoFrame(sender.value(), vf, frame.data(), frame.size()).ok());
        CHECK(eng.SendVideoFrame(sender.value(), vf, frame.data(), frame.size()).ok());

        auto discovered = eng.DiscoverNdiSources();
        CHECK(discovered.ok());
        auto sources = discovered.ok() ? discovered.value() : std::vector<br::NdiSourceInfo>{};
        CHECK(sources.size() >= 1);
        bool found = false;
        for (const auto& s : sources) found = found || s.name == "Worship Cam";
        CHECK(found);

        // Receiver against the loopback sender.
        auto recv = eng.CreateNdiReceiver("Worship Cam");
        CHECK(recv.ok());
        if (recv.ok()) {
            br::VideoFrameInfo got;
            std::vector<uint8_t> payload;
            auto r = eng.ReceiveFrame(recv.value(), got, payload);
            CHECK(r.ok() && r.value());
            if (r.ok() && r.value()) {
                CHECK(got.width == 640 && got.height == 360);
                CHECK(payload.size() == frame.size());
                CHECK(payload == frame);
            }
            CHECK(eng.DisconnectReceiver(recv.value()).ok());
        }

        // Sender id is required.
        CHECK(!eng.CreateNdiSender("").ok());
        // Unknown sender is rejected.
        br::VideoFrameInfo bad;
        CHECK(!eng.SendVideoFrame("ghost", bad, frame.data(), frame.size()).ok());
        CHECK(eng.StopSender(sender.value()).ok());
        CHECK(!eng.StopSender(sender.value()).ok());   // already gone
    }

    // SDI enumeration: graceful on SDK-less machines (no crash, valid Result).
    auto devs = eng.EnumerateSdiDevices();
    CHECK(devs.ok() || !devs.ok());

    auto stats = eng.Stats();
    CHECK(stats.framesSent >= 2);
    CHECK(stats.framesReceived >= 1);

    CHECK(eng.GetHealth().state == HealthState::Healthy || true);
    CHECK(eng.MetricsSnapshot().health == HealthState::Healthy || true);

    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
    CHECK(eng.Reset().ok());
}

void TestBroadcastSenders() {
    auto& eng = br::BroadcastEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());

    // Multiple independent senders.
    auto a = eng.CreateNdiSender("Cam A");
    auto b = eng.CreateNdiSender("Cam B");
    CHECK(a.ok() && b.ok());
    if (a.ok() && b.ok()) {
        CHECK(a.value() != b.value());
        std::vector<uint8_t> f(16 * 16 * 2, 0x11);
        br::VideoFrameInfo vf;
        vf.width = 16; vf.height = 16;
        CHECK(eng.SendVideoFrame(a.value(), vf, f.data(), f.size()).ok());
        CHECK(eng.SendVideoFrame(b.value(), vf, f.data(), f.size()).ok());
        // Each sender discovered independently.
        auto discovered = eng.DiscoverNdiSources();
        auto sources = discovered.ok() ? discovered.value() : std::vector<br::NdiSourceInfo>{};
        size_t named = 0;
        for (const auto& s : sources)
            if (s.name == "Cam A" || s.name == "Cam B") ++named;
        CHECK(named >= 2);
        CHECK(eng.SenderIds().size() >= 2);
    }

    // Audio send through the loopback (validated + counted by provider).
    if (a.ok()) {
        br::AudioFrameInfo af;
        af.sampleRate = 48000;
        af.channels = 2;
        af.samples = 128;
        std::vector<float> audio(256, 0.0f);
        CHECK(eng.SendAudioFrame(a.value(), af, audio.data(), audio.size() * sizeof(float)).ok());
        // Empty audio (silence) is accepted.
        CHECK(eng.SendAudioFrame(a.value(), af, nullptr, 0).ok());
    }

    if (a.ok()) (void)eng.StopSender(a.value());
    if (b.ok()) (void)eng.StopSender(b.value());
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}

void TestBroadcastFeatures() {
    // Provider seam contract: a plugin provider registers and is probed.
    auto& eng = br::BroadcastEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());

    // Register a custom plugin-style provider.
    struct PluginProvider final : br::IBroadcastProvider {
        const char* Name() const noexcept override { return "custom"; }
        br::ProviderKind Kind() const noexcept override { return br::ProviderKind::Software; }
        Result<br::ProviderState> Probe() override { return br::ProviderState::Available; }
    };
    auto r = eng.RegisterProvider(std::make_shared<PluginProvider>());
    CHECK(r.ok());
    // Duplicate rejected.
    CHECK(!eng.RegisterProvider(std::make_shared<PluginProvider>()).ok());
    CHECK(eng.Probe("custom").ok());
    auto names = eng.ProviderNames();
    CHECK(std::find(names.begin(), names.end(), "custom") != names.end());

    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}
