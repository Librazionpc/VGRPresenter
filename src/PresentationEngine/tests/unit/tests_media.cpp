// Unit tests: Media Engine (docs/specs/21).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests media
#include "TestHarness.hpp"

void TestMediaEngine() {
    auto& eng = m::MediaEngine::Instance();
    CHECK(eng.Initialize().ok());
    CHECK(eng.Start().ok());
    // Format registry.
    CHECK(eng.IsFormatSupported("png"));
    CHECK(eng.IsFormatSupported("mp4"));
    CHECK(!eng.IsFormatSupported("zzz"));
    auto fmt = eng.ResolveFormat("mp3");
    CHECK(fmt.ok() && fmt.value().type == m::MediaType::Audio);
    // Import (metadata extraction via the default provider).
    auto id = eng.Import("/tmp/photo.png", "Sunrise");
    CHECK(id.ok() && !id.value().empty());
    CHECK(eng.MediaCount() == 1);
    auto asset = eng.Get(id.value());
    CHECK(asset.ok() && asset.value().type == m::MediaType::Image);
    // Unsupported format rejected.
    CHECK(!eng.Import("/tmp/file.zzz", "x").ok());
    // Thumbnails.
    CHECK(!eng.HasThumbnail(id.value()));
    CHECK(eng.GenerateThumbnail(id.value(), 64, 64).ok());
    CHECK(eng.HasThumbnail(id.value()));
    // Playback: play -> pause -> resume -> seek -> rate -> stop.
    CHECK(eng.Play(id.value()).ok());
    CHECK(eng.GetPlaybackState(id.value()) == m::PlaybackState::Playing);
    CHECK(eng.Pause(id.value()).ok());
    CHECK(eng.GetPlaybackState(id.value()) == m::PlaybackState::Paused);
    CHECK(eng.StepFrame(id.value()).ok());
    CHECK(eng.Resume(id.value()).ok());
    CHECK(eng.Seek(id.value(), 5.0).ok());
    CHECK(eng.GetPosition(id.value()) == 5.0);
    CHECK(eng.SetPlaybackRate(id.value(), 2.0).ok());
    CHECK(eng.SetLoop(id.value(), true).ok());
    CHECK(eng.Stop(id.value()).ok());
    CHECK(eng.GetPlaybackState(id.value()) == m::PlaybackState::Stopped);
    // Background processing + cache.
    CHECK(eng.ProcessQueue(4).ok());
    CHECK(eng.CacheEntries() >= 1);
    eng.TrimCache(1);
    CHECK(eng.CacheEntries() <= 1);
    // Remove.
    CHECK(eng.Remove(id.value()).ok());
    CHECK(eng.MediaCount() == 0);
    CHECK(eng.Stop().ok());
    CHECK(eng.Shutdown().ok());
}

// ===========================================================================
// Native .vgr format (docs/specs/22)
// ===========================================================================
