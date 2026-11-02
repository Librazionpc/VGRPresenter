#include "TestHarness.hpp"

#include "core/config/Json.hpp"
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

bool PathEndsWith(const std::string& path, const std::string& tail) {
    std::string folded = path;
    std::replace(folded.begin(), folded.end(), '\\', '/');
    return folded.size() >= tail.size()
           && folded.compare(folded.size() - tail.size(), tail.size(), tail) == 0;
}
} // namespace

void TestMediaLibraryItemEdit() {
    namespace mm = bps::media;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_media_item_edit";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root + "/photos/sub").ok());
    CHECK(fs.CreateDirectories(root + "/clips").ok());
    MakeFile(root + "/photos/img2.png");
    MakeFile(root + "/photos/img10.png");
    MakeFile(root + "/clips/intro.mp4");
    const std::string storage = root + "/state/media-folders.json";

    {
        mm::MediaLibrary lib(storage);
        CHECK(lib.Load().ok());
        CHECK(lib.AddFolder(root + "/photos").ok());
        CHECK(lib.AddFolder(root + "/clips").ok());
        CHECK(lib.Scan(root + "/photos").ok());
        CHECK(lib.Scan(root + "/clips").ok());
        CHECK(lib.Count() == 3);
        // The paths the UI passes are the library's OWN (folderTree/items), never a
        // string the caller made up: that is what the pane hands to rename/move.
        const std::string photosDir = lib.Folders()[0].path;
        const std::string clipsDir = lib.Folders()[1].path;

        // ---- RENAME: a name without an extension KEEPS the file's ----------------
        CHECK(!lib.RenameItem(root + "/photos/img2.png", "").ok());
        CHECK(!lib.RenameItem(root + "/photos/img2.png", "   ").ok());
        CHECK(!lib.RenameItem(root + "/photos/img2.png", "a/b").ok());       // a name, not a path
        CHECK(!lib.RenameItem(root + "/photos/img2.png", "..").ok());
        CHECK(!lib.RenameItem(root + "/photos/nope.png", "x").ok());         // not in the library
        CHECK(lib.RenameItem(root + "/photos/img2.png", "  sunrise  ").ok());   // trimmed
        CHECK(fs.Exists(root + "/photos/sunrise.png"));                      // extension kept
        CHECK(!fs.Exists(root + "/photos/img2.png"));                         // ...and it MOVED
        CHECK(lib.Count() == 3);                                              // nothing gained or lost
        {
            auto items = lib.Items(root + "/photos");
            CHECK(items.size() == 2);
            bool found = false;
            for (const auto& it : items)
                if (it.name == "sunrise") {
                    found = true;
                    // The stored path is the PAL's own spelling (separator style and
                    // all), so this asks the disk rather than comparing strings.
                    CHECK(fs.Exists(it.path));
                    CHECK(PathEndsWith(it.path, "photos/sunrise.png"));
                    CHECK(it.kind == mm::MediaKind::Image);
                    CHECK(it.id == mm::MediaPathHash(it.path));               // the id follows the path
                }
            CHECK(found);
            CHECK(lib.Search("sunrise").size() == 1);                         // findable under the new name
            CHECK(lib.Search("img2").empty());
        }
        // A name already taken in that folder is refused, not overwritten, and the
        // file that was there is untouched.
        CHECK(!lib.RenameItem(root + "/photos/img10.png", "sunrise").ok());
        // A rename that only changes the extension is still a rename.
        CHECK(lib.RenameItem(root + "/photos/img10.png", "portrait.jpg").ok());
        CHECK(fs.Exists(root + "/photos/portrait.jpg"));
        {
            auto items = lib.Items(root + "/photos");
            bool jpgIsImage = false;
            for (const auto& it : items)
                if (it.name == "portrait") jpgIsImage = it.kind == mm::MediaKind::Image;
            CHECK(jpgIsImage);
        }
        CHECK(lib.RenameItem(root + "/photos/sunrise.png", "sunrise").ok());   // same name = no-op
        CHECK(fs.Exists(root + "/photos/sunrise.png"));

        // ---- MOVE: into another library folder, and into a folder inside one ----
        CHECK(!lib.MoveItem(root + "/photos/portrait.jpg", root + "/elsewhere").ok());
        CHECK(!lib.MoveItem(root + "/photos/portrait.jpg", "").ok());
        CHECK(lib.MoveItem(root + "/photos/portrait.jpg", clipsDir).ok());
        CHECK(fs.Exists(root + "/clips/portrait.jpg"));
        CHECK(!fs.Exists(root + "/photos/portrait.jpg"));
        CHECK(lib.Items(photosDir).size() == 1);        // sunrise only
        CHECK(lib.Items(clipsDir).size() == 2);         // intro + portrait
        CHECK(lib.Count() == 3);                        // a move never changes the count
        // Moving onto a name that is already there is refused.
        MakeFile(root + "/clips/sunrise.png");
        CHECK(!lib.MoveItem(root + "/photos/sunrise.png", clipsDir).ok());
        (void)fs.Remove(root + "/clips/sunrise.png");
        const std::string subDir = fs.Join(photosDir, "sub");
        CHECK(lib.Subfolders(photosDir).empty());
        CHECK(lib.MoveItem(root + "/photos/sunrise.png", subDir).ok());
        CHECK(fs.Exists(root + "/photos/sub/sunrise.png"));
        CHECK(lib.Items(photosDir).size() == 1);        // it lists what is beneath, subfolders included
        {
            auto subs = lib.Subfolders(photosDir);
            CHECK(subs.size() == 1 && subs[0].name == "sub" && subs[0].count == 1);
        }

        // ---- DELETE: the file goes, the index follows ---------------------------
        CHECK(!lib.RemoveItem(root + "/clips/nothing.mp4").ok());
        CHECK(lib.RemoveItem(root + "/clips/intro.mp4").ok());
        CHECK(!fs.Exists(root + "/clips/intro.mp4"));
        CHECK(lib.Count() == 2);
        CHECK(lib.Items(clipsDir).size() == 1);
        CHECK(lib.Search("intro").empty());
    }
    // The re-keyed index survives a restart: the renamed file is listed under its
    // new name, the moved one under its new folder, and the deleted one is gone.
    {
        mm::MediaLibrary again(storage);
        CHECK(again.Load().ok());
        CHECK(again.Count() == 2);
        bool sawMoved = false;
        for (const auto& it : again.Items(root + "/clips"))
            sawMoved = sawMoved || it.name == "portrait";
        CHECK(sawMoved);
        bool sawRenamed = false;
        for (const auto& it : again.Items(root + "/photos"))
            sawRenamed = sawRenamed || (it.name == "sunrise" && it.kind == mm::MediaKind::Image);
        CHECK(sawRenamed);   // the rename is in the INDEX, not only on disk
        CHECK(again.Search("intro").empty());
    }
    (void)fs.RemoveAll(root);
}

void TestMediaLibraryUndoDelete() {
    namespace mm = bps::media;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_media_undo_delete";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root + "/photos").ok());
    CHECK(fs.CreateDirectories(root + "/extra").ok());
    MakeFile(root + "/photos/img2.png");
    MakeFile(root + "/photos/img10.png");
    MakeFile(root + "/photos/img3.png");
    MakeFile(root + "/extra/note.mp3");
    const std::string storage = root + "/state/media-folders.json";

    {
        mm::MediaLibrary lib(storage);
        CHECK(lib.Load().ok());
        CHECK(lib.AddFolder(root + "/photos").ok());
        CHECK(lib.AddFolder(root + "/extra").ok());
        CHECK(lib.Scan(root + "/photos").ok());
        CHECK(lib.Scan(root + "/extra").ok());
        CHECK(lib.Count() == 4);

        // ---- a delete that can be taken back ------------------------------------
        CHECK(!lib.TrashItem(root + "/photos/nope.png").ok());          // not in the library
        auto trashed = lib.TrashItem(root + "/photos/img10.png");
        CHECK(trashed.ok());
        const std::string token = trashed.ok() ? trashed.value() : std::string();
        CHECK(!fs.Exists(root + "/photos/img10.png"));                  // out of the folder...
        CHECK(fs.Exists(token));                                          // ...but not destroyed
        CHECK(lib.Count() == 3);
        // Gone from the grid (the search itself is fuzzy — a near name may still come
        // back as a correction — so this asks about the file, not the hit count).
        for (const auto& it : lib.Items(root + "/photos"))
            CHECK(it.name != "img10");

        // A second delete has its OWN token: two notices on screen must never undo
        // each other's file.
        auto trashed2 = lib.TrashItem(root + "/photos/img3.png");
        CHECK(trashed2.ok() && trashed2.value() != token);
        CHECK(lib.Count() == 2);

        // ---- undo -----------------------------------------------------------------
        auto back = lib.RestoreTrashedItem(token);
        CHECK(back.ok());
        CHECK(fs.Exists(root + "/photos/img10.png"));                    // the same path it had
        CHECK(!fs.Exists(token));                                         // the trash copy is gone
        CHECK(lib.Count() == 3);
        {
            // Findable again, and FIRST: the literal name outranks a fuzzy neighbour.
            const auto hits = lib.Search("img10");
            CHECK(!hits.empty() && hits.front().name == "img10");
        }
        // ...and it came back IN ITS PLACE, not at the end of the grid.
        {
            auto items = lib.Items(root + "/photos");
            CHECK(items.size() == 2);                                     // img10 + img2 (img3 is still out)
            if (items.size() == 2) {
                CHECK(items[0].name == "img2");
                CHECK(items[1].name == "img10");
                CHECK(items[1].id == mm::MediaPathHash(items[1].path));    // same identity as before
            }
        }
        // A token is good ONCE.
        auto again = lib.RestoreTrashedItem(token);
        CHECK(!again.ok() && again.error().code == Err::NotFound);

        // ---- something has taken the name: the file stays safe in the trash -----
        auto third = lib.TrashItem(root + "/photos/img2.png");
        CHECK(third.ok());
        MakeFile(root + "/photos/img2.png", "a new file under the old name");
        auto blocked = lib.RestoreTrashedItem(third.value());
        CHECK(!blocked.ok() && blocked.error().code == Err::AlreadyExists);
        CHECK(fs.Exists(third.value()));                                  // the copy is still there
        CHECK(lib.Count() == 2);                                          // the library lists neither copy yet
        // The way clears, and the same token works: nothing was lost by the refusal.
        CHECK(fs.Remove(root + "/photos/img2.png").ok());
        CHECK(lib.RestoreTrashedItem(third.value()).ok());
        CHECK(fs.Exists(root + "/photos/img2.png"));

        // ---- a folder that has left the library: the file still comes back -------
        auto orphan = lib.TrashItem(root + "/extra/note.mp3");
        CHECK(orphan.ok());
        CHECK(lib.RemoveFolder(root + "/extra").ok());
        CHECK(lib.RestoreTrashedItem(orphan.value()).ok());
        CHECK(fs.Exists(root + "/extra/note.mp3"));                      // on the disk again...
        CHECK(lib.Count() == 2);                                          // ...but not re-indexed

        // The permanent delete is still permanent, and is not what the operator's
        // Delete calls (see removeItem in the service).
        CHECK(lib.RemoveItem(root + "/photos/img2.png").ok());
        CHECK(!fs.Exists(root + "/photos/img2.png"));
    }
    // A restart cannot undo: the tokens lived in the run that made them, and the
    // index remembers only what is in the library.
    {
        mm::MediaLibrary restarted(storage);
        CHECK(restarted.Load().ok());
        CHECK(restarted.Count() == 1);                                     // img10 (img2 was deleted for good, img3 is in the trash)
        CHECK(!restarted.RestoreTrashedItem("whatever").ok());
        bool saw = false;
        for (const auto& it : restarted.Items(root + "/photos"))
            saw = saw || it.name == "img10";
        CHECK(saw);
    }
    (void)fs.RemoveAll(root);
}

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
        const std::string index = root + "/state/media-index.json";
        CHECK(fs.Write(index, "{ not json").ok());
        mm::MediaLibrary fourth(storage);
        CHECK(fourth.Load().ok() && fourth.Folders().size() == 1 && fourth.Count() == 0);
        std::vector<std::string> quarantined;
        auto entries = fs.Enumerate(root + "/state");
        CHECK(entries.ok());
        if (entries.ok())
            for (const auto& e : entries.value())
                if (e.find("media-index.json.corrupt-") != std::string::npos) quarantined.push_back(e);
        CHECK(quarantined.size() == 1);
        if (quarantined.size() == 1) {
            auto kept = fs.ReadText(quarantined.front());
            CHECK(kept.ok() && kept.value() == "{ not json");
        }
        // …and the next scan rebuilds a healthy index at the same path.
        CHECK(fourth.Scan(root + "/clips").ok() && fourth.Count() == 2);
        auto rebuilt = fs.ReadText(index);
        CHECK(rebuilt.ok() && bps::json::Parse(rebuilt.value()).ok());
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

        CHECK(lib.Items("").size() == 1);          // empty = "everything"
        CHECK(!lib.Scan("").ok());                 // empty is not a library folder
        CHECK(!lib.RemoveFolder("").ok());
        CHECK(lib.Subfolders("").empty());
        (void)fs.RemoveAll(root);
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void TestMediaLibraryItemFit() {
    namespace mm = bps::media;
    auto& fs = bps::platform::PlatformAccessor::Get().Filesystem();
    const std::string root = "/tmp/bps_media_item_fit";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root + "/photos").ok());
    MakeFile(root + "/photos/logo.png");
    MakeFile(root + "/photos/portrait.png");
    const std::string storage = root + "/state/media-folders.json";

    auto fitOf = [](const std::vector<mm::LibraryItem>& items, const std::string& name) {
        for (const auto& it : items)
            if (it.name == name) return it.fit;
        return std::string("<missing>");
    };

    {
        mm::MediaLibrary lib(storage);
        CHECK(lib.Load().ok());
        CHECK(lib.AddFolder(root + "/photos").ok());
        CHECK(lib.Scan(root + "/photos").ok());

        // Nothing chosen yet: no file names a fill, so every one of them follows
        // its output's own default.
        CHECK(fitOf(lib.Items(root + "/photos"), "portrait") == "");
        CHECK(lib.ItemFit(root + "/photos/portrait.png") == "");

        CHECK(lib.SetItemFit(root + "/photos/portrait.png", "cover").ok());
        CHECK(lib.ItemFit(root + "/photos/portrait.png") == "cover");
        CHECK(fitOf(lib.Items(root + "/photos"), "portrait") == "cover");
        CHECK(fitOf(lib.Items(root + "/photos"), "logo") == "");   // only that file

        // The three the renderer knows are the three it takes; anything else is
        // refused rather than silently becoming the default.
        CHECK(!lib.SetItemFit(root + "/photos/portrait.png", "blur").ok());
        CHECK(!lib.SetItemFit(root + "/photos/portrait.png", "Contain").ok());   // case matters
        CHECK(lib.ItemFit(root + "/photos/portrait.png") == "cover");
        // A file the library does not list has no fill to set.
        CHECK(!lib.SetItemFit(root + "/photos/nope.png", "cover").ok());
        // Setting the same value again is a no-op that still answers ok().
        CHECK(lib.SetItemFit(root + "/photos/portrait.png", "cover").ok());

        // A RESCAN REBUILDS THE ITEMS from the disk — the fill must not go with
        // them (it is not a fact about the file on disk, it is a choice).
        CHECK(lib.Scan(root + "/photos").ok());
        CHECK(fitOf(lib.Items(root + "/photos"), "portrait") == "cover");

        // A RENAME carries it to the new path (the fill is about the picture, not
        // about the path it happened to sit at).
        CHECK(lib.RenameItem(root + "/photos/portrait.png", "portrait 2").ok());
        CHECK(lib.ItemFit(root + "/photos/portrait 2.png") == "cover");
        CHECK(lib.ItemFit(root + "/photos/portrait.png") == "");
        CHECK(fitOf(lib.Items(root + "/photos"), "portrait 2") == "cover");

        CHECK(lib.SetItemFit(root + "/photos/logo.png", "fill").ok());
        CHECK(lib.ItemFit(root + "/photos/logo.png") == "fill");
    }

    // A RESTART: the fills are their own small document beside the index, so they
    // are back with the folder list and the remembered items.
    {
        mm::MediaLibrary lib(storage);
        CHECK(lib.Load().ok());
        CHECK(lib.ItemFit(root + "/photos/portrait 2.png") == "cover");
        CHECK(lib.ItemFit(root + "/photos/logo.png") == "fill");
        CHECK(fitOf(lib.Items(root + "/photos"), "logo") == "fill");

        // "": back to whatever the output says — a real answer, not a miss.
        CHECK(lib.SetItemFit(root + "/photos/logo.png", "").ok());
        CHECK(lib.ItemFit(root + "/photos/logo.png") == "");
        CHECK(fitOf(lib.Items(root + "/photos"), "logo") == "");
    }
    {
        // ...and the clear is what a third run reads too (the empty map is
        // written, not just kept in memory).
        mm::MediaLibrary lib(storage);
        CHECK(lib.Load().ok());
        CHECK(lib.ItemFit(root + "/photos/logo.png") == "");
        CHECK(lib.ItemFit(root + "/photos/portrait 2.png") == "cover");
    }
    (void)fs.RemoveAll(root);
}