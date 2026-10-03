// Unit tests: Media Engine (docs/specs/21).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests media
#include "TestHarness.hpp"

#include "modules/media/MediaLibrary.hpp"
#include "modules/media/ThumbnailCache.hpp"
#include "modules/rendering/PngCodec.hpp"

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

// ---------------------------------------------------------------------------
// Media library + thumbnail cache (modules/media/MediaLibrary, ThumbnailCache)
// ---------------------------------------------------------------------------

namespace {

// A frame source with no codec: a flat picture, and a log of what was asked of it.
class FakeFrameSource final : public bps::media::IFrameSource {
public:
    std::atomic<int> grabs{0};
    std::atomic<bool> fail{false};
    std::mutex mutex;
    std::vector<double> fractions;
    std::chrono::milliseconds delay{0};

    Result<bps::media::Picture> Grab(const std::string&, bps::media::MediaKind, double fraction,
                                     int targetWidth) override {
        ++grabs;
        {
            std::lock_guard<std::mutex> lock(mutex);
            fractions.push_back(fraction);
        }
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        if (fail) return Error::Make(Err::Unsupported, "Fake", "cannot decode");
        bps::media::Picture p;
        p.width = std::min(targetWidth, 32);
        p.height = 18;
        p.rgba.assign(static_cast<size_t>(p.width) * p.height * 4, 200);
        return Result<bps::media::Picture>{std::move(p)};
    }
};

void MakeFile(const std::string& path, const std::string& content = "x") {
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    CHECK(fs.Write(path, content).ok());
}

} // namespace

void TestPngEncode() {
    namespace r = bps::rendering;
#ifdef BPS_HAVE_ZLIB
    // 3x2; the decoder packs a pixel as 0xAABBGGRR (red = 0xFF0000FF).
    const std::vector<uint8_t> rgba = {
        0, 0, 0, 255,      255, 0, 0, 255,    0, 255, 0, 255,      // black, red, green
        255, 255, 255, 255, 0, 0, 255, 255,   127, 127, 127, 255   // white, blue, grey
    };
    auto png = r::EncodePngRgba8(rgba.data(), 3, 2);
    CHECK(png.ok());
    if (png.ok()) {
        CHECK(r::LooksLikePng(png.value().data(), png.value().size()));
        auto dec = r::DecodePng(png.value().data(), png.value().size());
        CHECK(dec.ok());
        if (dec.ok()) {
            CHECK(dec.value().width == 3 && dec.value().height == 2);
            const std::vector<uint32_t> want = {0xFF000000u, 0xFF0000FFu, 0xFF00FF00u,
                                                0xFFFFFFFFu, 0xFFFF0000u, 0xFF7F7F7Fu};
            CHECK(dec.value().pixels.size() == want.size());
            for (size_t i = 0; i < want.size() && i < dec.value().pixels.size(); ++i)
                CHECK(dec.value().pixels[i] == want[i]);
        }
    }
    CHECK(!r::EncodePngRgba8(nullptr, 3, 2).ok());
    CHECK(!r::EncodePngRgba8(rgba.data(), 0, 2).ok());
#endif
}

void TestMediaLibraryFolders() {
    namespace mm = bps::media;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_media_library";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root + "/photos/sub").ok());
    CHECK(fs.CreateDirectories(root + "/clips").ok());
    MakeFile(root + "/photos/img2.png");
    MakeFile(root + "/photos/img10.PNG");
    MakeFile(root + "/photos/sub/deep.jpg");
    MakeFile(root + "/photos/notes.txt");            // not media
    MakeFile(root + "/clips/intro.mp4");
    MakeFile(root + "/clips/take.mkv");

    // Kinds by extension.
    CHECK(mm::MediaKindOf("a.PNG") == mm::MediaKind::Image);
    CHECK(mm::MediaKindOf("b.mkv") == mm::MediaKind::Video);
    CHECK(!mm::MediaKindOf("c.txt").has_value() && !mm::MediaKindOf("noext").has_value());
    CHECK(mm::MediaKindOf("x.webp") == mm::MediaKind::Image && mm::MediaKindOf("x.TIFF") == mm::MediaKind::Image
          && mm::MediaKindOf("x.heic") == mm::MediaKind::Image && mm::MediaKindOf("x.svg") == mm::MediaKind::Image);
    CHECK(mm::MediaKindOf("x.MP3") == mm::MediaKind::Audio && mm::MediaKindOf("x.flac") == mm::MediaKind::Audio
          && mm::MediaKindOf("x.wav") == mm::MediaKind::Audio && mm::MediaKindOf("x.m4a") == mm::MediaKind::Audio
          && mm::MediaKindOf("x.opus") == mm::MediaKind::Audio);
    CHECK(mm::MediaKindOf("x.ts") == mm::MediaKind::Video && mm::MediaKindOf("x.flv") == mm::MediaKind::Video
          && mm::MediaKindOf("x.m2ts") == mm::MediaKind::Video && mm::MediaKindOf("x.3gp") == mm::MediaKind::Video);
    // The path hash is stable, separator-insensitive, and file-name safe.
    CHECK(mm::MediaPathHash("C:/a/b.png") == mm::MediaPathHash("C:\\a\\b.png"));
    CHECK(mm::MediaPathHash("C:/a/b.png") != mm::MediaPathHash("C:/a/c.png"));
    {
        const std::string h = mm::MediaPathHash("C:/a/b.png");
        CHECK(!h.empty() && (h[0] == 'a' || h[0] == 'i'));
        CHECK(h.find_first_not_of("ai0123456789") == std::string::npos);
    }

    const std::string storage = root + "/state/media-folders.json";
    {
        mm::MediaLibrary lib(storage);
        CHECK(lib.Load().ok());                       // no file yet = an empty library
        CHECK(lib.Folders().empty());
        CHECK(!lib.AddFolder(root + "/nope").ok());   // not a folder
        CHECK(lib.AddFolder(root + "/photos").ok());
        CHECK(lib.AddFolder(root + "/clips").ok());
        CHECK(!lib.AddFolder(root + "/photos/").ok());   // already there, spelled differently
        CHECK(lib.Folders().size() == 2);
        CHECK(!lib.Scan(root + "/elsewhere").ok());   // not in the library

        auto photos = lib.Scan(root + "/photos");
        CHECK(photos.ok() && photos.value() == 3);    // img2, img10, deep - not notes.txt
        auto clips = lib.Scan(root + "/clips");
        CHECK(clips.ok() && clips.value() == 2);
        CHECK(lib.Count() == 5);

        // Natural name order: img2 before img10; kinds and names are right.
        auto items = lib.Items(root + "/photos");
        CHECK(items.size() == 3);
        if (items.size() == 3) {
            CHECK(items[0].name == "deep");
            CHECK(items[1].name == "img2");
            CHECK(items[2].name == "img10");
            CHECK(items[1].kind == mm::MediaKind::Image);
            CHECK(!items[1].id.empty() && items[1].id == mm::MediaPathHash(items[1].path));
        }
        auto all = lib.Items();
        CHECK(all.size() == 5);
        bool anyVideo = false;
        for (const auto& it : all) anyVideo = anyVideo || it.kind == mm::MediaKind::Video;
        CHECK(anyVideo);

        // Search by NAME: every word must appear (any order, any case), best matches first.
        CHECK(lib.Search("").size() == 5);                                    // empty = everything
        CHECK(lib.Search("IMG").size() == 2);                                 // img2, img10
        CHECK(lib.Search("img 10").size() == 1);                              // ... both words must match
        CHECK(lib.Search("10 img").size() == 1 && lib.Search("zzz").empty());
        CHECK(lib.Search("i", root + "/photos").size() == 2);                 // img2, img10 - not deep
        CHECK(lib.Search("take", root + "/photos").empty());                 // ... but only in that folder
        CHECK(lib.Search("take").size() == 1 && lib.Search("take")[0].kind == mm::MediaKind::Video);
        {
            auto ranked = lib.Search("ep");                                    // "deep" contains it mid-word; nothing starts with it
            CHECK(ranked.size() == 1 && ranked[0].name == "deep");
            auto starts = lib.Search("i");                                      // img2, img10 start with i; intro too - deep does not
            CHECK(starts.size() == 3 && starts.back().name != "deep");
        }

        auto folders = lib.Folders();
        CHECK(folders.size() == 2 && folders[0].name == "photos" && folders[0].count == 3 && folders[0].scanned);
    }
    // The folder LIST and what was found in each folder survive a restart (so the grid is not empty
    // until the disk has been walked again); a scan still refreshes them.
    {
        mm::MediaLibrary again(storage);
        CHECK(again.Load().ok());
        CHECK(again.Folders().size() == 2);
        CHECK(again.Count() == 5 && !again.Folders()[0].scanned);           // remembered, not yet re-scanned
        CHECK(again.Items(root + "/photos").size() == 3);
        CHECK(again.Scan(root + "/photos").ok() && again.Count() == 5 && again.Folders()[0].scanned);

        // A new file shows up on re-scan; removing a folder only forgets it (files stay).
        MakeFile(root + "/photos/img3.gif");
        auto rescanned = again.Scan(root + "/photos");
        CHECK(rescanned.ok() && rescanned.value() == 4);
        CHECK(again.RemoveFolder(root + "/photos").ok());
        CHECK(!again.RemoveFolder(root + "/photos").ok());
        CHECK(again.Folders().size() == 1 && again.Count() == 2);           // clips remain
        CHECK(fs.Exists(root + "/photos/img2.png"));
    }
    {
        mm::MediaLibrary third(storage);
        CHECK(third.Load().ok() && third.Folders().size() == 1);
        CHECK(third.Count() == 2 && third.Items().size() == 2);           // the removed folder's items are gone
        // A damaged index is ignored, not fatal: the folder list still loads, scanning fills it again.
        CHECK(fs.Write(root + "/state/media-index.json", "{ not json").ok());
        mm::MediaLibrary fourth(storage);
        CHECK(fourth.Load().ok() && fourth.Folders().size() == 1 && fourth.Count() == 0);
        CHECK(fourth.Scan(root + "/clips").ok() && fourth.Count() == 2);
    }
}

void TestThumbnailCache() {
    namespace mm = bps::media;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_media_thumbs";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root + "/src").ok());
    const std::string photo = root + "/src/photo.png";
    const std::string video = root + "/src/clip.mp4";
    MakeFile(photo);
    MakeFile(video);

    auto source = std::make_shared<FakeFrameSource>();
    mm::ThumbnailCache cache(root + "/cache", source, 2);

    // Sizes round up to the cache's fixed sizes.
    CHECK(mm::ThumbnailCache::RoundSize(50) == 100);
    CHECK(mm::ThumbnailCache::RoundSize(250) == 250);
    CHECK(mm::ThumbnailCache::RoundSize(251) == 500);
    CHECK(mm::ThumbnailCache::RoundSize(5000) == 900);

    // First ask decodes and writes "<hash>-<size>.png"; the second is a cache hit.
    auto first = cache.Still(photo, mm::MediaKind::Image, 250);
    CHECK(first.ok());
    if (first.ok()) {
        CHECK(first.value() == cache.CachePath(photo, 250));
        CHECK(first.value().find(mm::MediaPathHash(photo) + "-250.png") != std::string::npos);
        CHECK(fs.Exists(first.value()));
        auto png = fs.ReadBinary(first.value());
        CHECK(png.ok() && bps::rendering::LooksLikePng(png.value().data(), png.value().size()));
    }
    CHECK(source->grabs == 1 && cache.GeneratedCount() == 1);
    CHECK(cache.Still(photo, mm::MediaKind::Image, 250).ok());
    CHECK(source->grabs == 1);                       // served from the cache
    // Another size is another file.
    CHECK(cache.Still(photo, mm::MediaKind::Image, 100).ok());
    CHECK(source->grabs == 2);

    // A video's still is its MIDDLE (FreeShow's seek 0.5); its moving frames step through it.
    CHECK(cache.Still(video, mm::MediaKind::Video, 250).ok());
    CHECK(!source->fractions.empty() && source->fractions.back() == 0.5);
    CHECK(cache.Frame(video, 0, 250).ok());
    CHECK(source->fractions.back() > 0.099 && source->fractions.back() < 0.101);     // 10%
    CHECK(cache.Frame(video, 4, 250).ok());
    CHECK(source->fractions.back() > 0.499 && source->fractions.back() < 0.501);     // 50%
    CHECK(cache.Frame(video, mm::ThumbnailCache::kFrameSteps - 1, 250).ok());
    CHECK(source->fractions.back() < 1.0);                                            // held short of the end
    CHECK(cache.Frame(video, 4, 250).ok());
    const int before = source->grabs;
    CHECK(cache.Frame(video, 4, 250).ok() && source->grabs == before);               // cached
    CHECK(!cache.Frame(video, -1, 250).ok() && !cache.Frame(video, 10, 250).ok());   // no such step

    // Audio: its "picture" (cover art) is looked up like an image's - no position in it.
    const std::string song = root + "/src/song.mp3";
    MakeFile(song);
    CHECK(cache.Still(song, mm::MediaKind::Audio, 250).ok());
    CHECK(source->fractions.back() == 0.0);

    // A missing file is NotFound and loses its cached pictures.
    const std::string gone = root + "/src/gone.png";
    MakeFile(gone);
    CHECK(cache.Still(gone, mm::MediaKind::Image, 250).ok());
    const std::string gonePng = cache.CachePath(gone, 250);
    CHECK(fs.Exists(gonePng));
    CHECK(fs.Remove(gone).ok());
    auto missing = cache.Still(gone, mm::MediaKind::Image, 250);
    CHECK(!missing.ok() && missing.error().code == Err::NotFound);
    CHECK(!fs.Exists(gonePng));

    // Modification times are real Unix-epoch nanoseconds (this was overflowing on Windows), so "newer" means something.
    {
        auto meta = fs.Metadata(photo);
        CHECK(meta.ok() && meta.value().modifiedEpochNs > 1'500'000'000'000'000'000LL);
    }
    // A source newer than its thumbnail regenerates it (FreeShow: source mtime > thumbnail mtime).
    const int beforeEdit = source->grabs;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // above the file system's timestamp granularity
    MakeFile(photo, "edited");
    CHECK(cache.Still(photo, mm::MediaKind::Image, 250).ok());
    CHECK(source->grabs == beforeEdit + 1);

    // Invalidate drops every size and frame of a file.
    cache.Invalidate(video);
    CHECK(!fs.Exists(cache.CachePath(video, 250)) && !fs.Exists(cache.CachePath(video, 250, 4)));

    // A failing decoder: the error comes back, and a repeat straight away is not retried.
    const std::string bad = root + "/src/bad.mp4";
    MakeFile(bad);
    source->fail = true;
    const int beforeFail = source->grabs;
    CHECK(!cache.Still(bad, mm::MediaKind::Video, 250).ok());
    CHECK(source->grabs == beforeFail + 1);
    CHECK(!cache.Still(bad, mm::MediaKind::Video, 250).ok());
    CHECK(source->grabs == beforeFail + 1);          // not asked again this soon
    source->fail = false;
}

void TestThumbnailCacheConcurrency() {
    namespace mm = bps::media;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_media_thumbs_mt";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root + "/src").ok());
    const std::string photo = root + "/src/photo.png";
    MakeFile(photo);

    auto source = std::make_shared<FakeFrameSource>();
    source->delay = std::chrono::milliseconds(80);
    mm::ThumbnailCache cache(root + "/cache", source, 2);

    // Eight threads ask for the same picture at once: it is decoded ONCE, all get the file.
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            if (cache.Still(photo, mm::MediaKind::Image, 250).ok()) ++ok;
        });
    for (auto& t : threads) t.join();
    CHECK(ok == 8);
    CHECK(source->grabs == 1);

    // Many different pictures at once never exceed the concurrency limit.
    struct Probe final : bps::media::IFrameSource {
        std::atomic<int> running{0}, peak{0};
        Result<bps::media::Picture> Grab(const std::string&, bps::media::MediaKind, double, int) override {
            const int now = ++running;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            --running;
            bps::media::Picture p;
            p.width = 8; p.height = 4;
            p.rgba.assign(8 * 4 * 4, 90);
            return Result<bps::media::Picture>{std::move(p)};
        }
    };
    auto probe = std::make_shared<Probe>();
    mm::ThumbnailCache limited(root + "/cache2", probe, 2);
    std::vector<std::thread> many;
    for (int i = 0; i < 10; ++i) {
        const std::string file = root + "/src/f" + std::to_string(i) + ".png";
        MakeFile(file);
        many.emplace_back([&, file] { (void)limited.Still(file, mm::MediaKind::Image, 250); });
    }
    for (auto& t : many) t.join();
    CHECK(probe->peak <= 2);
    CHECK(limited.GeneratedCount() == 10);
}

void TestMediaLibraryNestedFolders() {
    namespace mm = bps::media;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_media_nested";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root + "/events/easter 2027/stage").ok());
    CHECK(fs.CreateDirectories(root + "/events/easter/stage").ok());
    CHECK(fs.CreateDirectories(root + "/events/christmas").ok());
    CHECK(fs.CreateDirectories(root + "/empty/inside").ok());
    MakeFile(root + "/events/top.png");
    MakeFile(root + "/events/easter 2027/a.jpg");
    MakeFile(root + "/events/easter 2027/stage/b.mp4");
    MakeFile(root + "/events/easter 2027/stage/c.mp4");
    MakeFile(root + "/events/easter/d.png");
    MakeFile(root + "/events/christmas/e.gif");
    // (events/easter/stage and empty/inside hold no media, so they must not be listed.)

    const std::string storage = root + "/state/media-folders.json";
    {
        mm::MediaLibrary lib(storage);
        CHECK(lib.Load().ok());
        CHECK(lib.AddFolder(root + "/events").ok());
        CHECK(lib.AddFolder(root + "/empty").ok());
        CHECK(lib.Subfolders(root + "/events").empty());            // nothing until it has been scanned
        auto scanned = lib.Scan(root + "/events");
        CHECK(scanned.ok() && scanned.value() == 6);                // the parent takes in everything beneath it
        CHECK(lib.Scan(root + "/empty").ok() && lib.Subfolders(root + "/empty").empty());
        CHECK(lib.Folders()[0].count == 6);

        // The tree: each folder is followed by what is inside it; counts include everything below.
        auto tree = lib.Subfolders(root + "/events");
        CHECK(tree.size() == 4);
        if (tree.size() == 4) {
            CHECK(tree[0].name == "christmas" && tree[0].depth == 1 && tree[0].count == 1);
            CHECK(tree[1].name == "easter" && tree[1].depth == 1 && tree[1].count == 1);
            CHECK(tree[2].name == "easter 2027" && tree[2].depth == 1 && tree[2].count == 3);
            CHECK(tree[3].name == "stage" && tree[3].depth == 2 && tree[3].count == 2);
            CHECK(tree[3].parent.find("easter 2027") != std::string::npos);
        }

        // A nested folder opens on its own; the parent opens everything.
        CHECK(lib.Items(root + "/events").size() == 6);
        CHECK(lib.Items(root + "/events/easter 2027").size() == 3);
        CHECK(lib.Items(root + "/events/easter 2027/stage").size() == 2);
        CHECK(lib.Items(root + "/events/easter").size() == 1);     // not "easter 2027": a folder name is not a prefix match
        CHECK(lib.Items(root + "/events/nowhere").empty());
        CHECK(lib.Search("b", root + "/events/easter 2027/stage").size() == 1);
        CHECK(lib.Search("a", root + "/events/easter 2027").size() == 1);
        CHECK(lib.Search("e").size() == 1);                        // across everything (only "e" matches e.gif)
    }
    // The tree is restored with the remembered index, before any scan.
    {
        mm::MediaLibrary again(storage);
        CHECK(again.Load().ok());
        CHECK(again.Subfolders(root + "/events").size() == 4);
        CHECK(again.Items(root + "/events/easter 2027/stage").size() == 2);
    }
}

// ---------------------------------------------------------------------------
// Empty-path safety (regression, 2026-10-01).
//
// WHY THIS TEST EXISTS: the app was crashing on GO LIVE with an NDI output
// enabled, with five byte-identical crash records reading
//
//   signal=0 (filesystem error: cannot make absolute path: Invalid argument [])
//
// On Windows, std::filesystem::absolute() calls GetFullPathNameW, which
// fails with ERROR_INVALID_PARAMETER (22) for an EMPTY path. The error_code
// overload returns that as an error code (harmless); the THROWING overload
// raises std::filesystem::filesystem_error, which escaped into
// std::terminate and killed the process.
//
// The PAL's own Absolute() always used the error_code overload and so never
// threw — but it turned an empty query into an Err::IoError, which callers
// treat as fatal, and it made the "empty path" case look like a disk
// failure in the log. Both are now handled explicitly: an empty path is a
// question with no answer, answered with an empty string.
//
// The media library is the reachable path: MediaLibrary::Canonical("") and
// Items("") both route an empty folder name into Absolute().
// ---------------------------------------------------------------------------
void TestEmptyPathSafety() {
    namespace mm = bps::media;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();

    // Absolute("") is an empty string, not an error and never a throw.
    {
        auto abs = fs.Absolute("");
        CHECK(abs.ok());
        CHECK(abs.value().empty());
    }

    // Metadata("") reports "not a file, not a directory" — not an IoError.
    {
        auto meta = fs.Metadata("");
        CHECK(meta.ok());
        CHECK(!meta.value().isDirectory);
        CHECK(!meta.value().isRegularFile);
    }

    // A normal path still resolves (the guard must not swallow real work).
    {
        const std::string root = "/tmp/bps_empty_path";
        (void)fs.RemoveAll(root);
        CHECK(fs.CreateDirectories(root).ok());
        auto abs = fs.Absolute(root);
        CHECK(abs.ok());
        CHECK(!abs.value().empty());
        (void)fs.RemoveAll(root);
    }

    // The media library: an empty folder name finds nothing and scans
    // nothing, and must not take the process down on the way.
    {
        const std::string root = "/tmp/bps_empty_path_lib";
        (void)fs.RemoveAll(root);
        CHECK(fs.CreateDirectories(root).ok());
        MakeFile(root + "/pic.png");

        mm::MediaLibrary lib(root + "/state/media-folders.json");
        CHECK(lib.Load().ok());
        CHECK(lib.AddFolder(root).ok());
        CHECK(lib.Scan(root).ok());                // items_ is filled by Scan, not AddFolder

        // Canonical() is private; its empty-path guard is exercised through
        // the public callers below (Scan/RemoveFolder/Subfolders all route
        // their argument through it).
        CHECK(lib.Items("").size() == 1);          // empty = "everything"
        CHECK(!lib.Scan("").ok());                 // empty is not a library folder
        CHECK(!lib.RemoveFolder("").ok());
        CHECK(lib.Subfolders("").empty());
        (void)fs.RemoveAll(root);
    }
}
