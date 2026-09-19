// Unit tests: CAMS / content / asset management (docs/specs/13).
// Split from tests/unit/main.cpp so a single phase can run alone:
//   ./bps_unit_tests cams
#include "TestHarness.hpp"

void TestCamsUuid() {
    auto a = content::Uuid::Generate();
    auto b = content::Uuid::Generate();
    CHECK(a != b);
    CHECK(a.ToString().size() == 36);
    CHECK(b.ToString().size() == 36);
    // v4: version nibble in byte 6, variant 10 in byte 8.
    CHECK(a.ToString()[14] == '4');
    char v = a.ToString()[19];
    CHECK(v == '8' || v == '9' || v == 'a' || v == 'b');
    auto parsed = content::Uuid::FromString(a.ToString());
    CHECK(parsed == a);
    CHECK(content::Uuid::FromString("garbage") == content::Uuid{});
}
void TestCamsVfs() {
    namespace fs = std::filesystem;
    const std::string dir = "/tmp/bps_cams_vfs";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // MemoryVfs
    auto mem = std::make_shared<content::MemoryVfs>("mem");
    CHECK(mem->WriteText("a.txt", "hello memory").ok());
    CHECK(mem->Write("sub/b.bin", std::vector<uint8_t>{1, 2, 3}).ok());
    auto t = mem->ReadText("a.txt");
    CHECK(t.ok() && t.value() == "hello memory");
    auto e = mem->Exists("a.txt");
    CHECK(e.ok() && e.value());
    auto list = mem->List("");
    CHECK(list.ok());
    CHECK(list.value().size() == 2);
    CHECK(mem->Move("a.txt", "renamed.txt").ok());
    CHECK(!mem->Exists("a.txt").value());
    CHECK(mem->Exists("renamed.txt").value());
    CHECK(mem->Remove("renamed.txt").ok());
    CHECK(!mem->Exists("renamed.txt").value());

    // DiskVfs through the PAL
    auto disk = std::make_shared<content::DiskVfs>("disk", dir);
    CHECK(disk->WriteText("x/y.txt", "disk content").ok());
    auto dt = disk->ReadText("x/y.txt");
    CHECK(dt.ok() && dt.value() == "disk content");
    CHECK(disk->Exists("x/y.txt").ok() && disk->Exists("x/y.txt").value());
    auto dlist = disk->List("");
    CHECK(dlist.ok());
    bool foundDir = false;
    for (const auto& p : dlist.value()) if (p == "x") foundDir = true;   // non-recursive
    CHECK(foundDir);
    auto sub = disk->List("x");
    CHECK(sub.ok());
    bool found = false;
    for (const auto& p : sub.value()) if (p == "x/y.txt") found = true;
    CHECK(found);
    CHECK(disk->Move("x/y.txt", "z.txt").ok());
    CHECK(disk->Exists("x/y.txt").ok() && !disk->Exists("x/y.txt").value());
    CHECK(disk->Exists("z.txt").value());
    CHECK(disk->Remove("z.txt").ok());

    fs::remove_all(dir, ec);
}
void TestCamsZip() {
    namespace fs = std::filesystem;
    const std::string dir = "/tmp/bps_cams_zip";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    content::ZipWriter writer;
    std::vector<uint8_t> one{'h', 'i', ' ', 't', 'h', 'e', 'r', 'e'};
    std::vector<uint8_t> two{'1', '2', '3'};
    CHECK(writer.AddEntry("hello.txt", one).ok());
    CHECK(writer.AddEntry("nested/data.bin", two).ok());
    auto zip = writer.Finalize();
    CHECK(zip.ok());
    CHECK(zip.value().size() > 100);
    const std::string zipPath = dir + "/pkg.zip";
    CHECK(writer.WriteTo(zipPath).ok());
    CHECK(fs::exists(zipPath, ec));

    // Read back through ZipVfs
    auto zv = std::make_shared<content::ZipVfs>("pkg", zipPath);
    CHECK(zv->Open().ok());
    CHECK(zv->EntryCount() == 2);
    auto t = zv->ReadText("hello.txt");
    CHECK(t.ok() && t.value() == "hi there");
    auto b = zv->Read("nested/data.bin");
    CHECK(b.ok() && b.value() == two);
    auto sz = zv->Size("hello.txt");
    CHECK(sz.ok() && sz.value() == one.size());
    auto exists = zv->Exists("hello.txt");
    CHECK(exists.ok() && exists.value());
    CHECK(zv->Read("missing").error().code == Err::NotFound);
    CHECK(zv->Write("x", one).error().code == Err::Unsupported);   // read-only

    // CRC32 sanity (known vector)
    const uint8_t vec[] = "123456789";
    CHECK(content::Crc32(vec, 9) == 0xCBF43926u);

    fs::remove_all(dir, ec);
}
void TestCamsDatabase() {
    content::AssetDatabase db;
    content::AssetMetadata a;
    a.uuid = content::Uuid::Generate();
    a.name = "sermon-notes";
    a.type = content::AssetType::Text;
    a.path = "notes/sermon-notes.txt";
    a.tags = {"notes", "sermon"};
    a.category = "teaching";
    a.favorite = true;
    CHECK(db.Upsert(a).ok());
    content::AssetMetadata b = a;
    b.uuid = content::Uuid::Generate();
    b.name = "background";
    b.type = content::AssetType::Image;
    b.path = "media/background.png";
    b.favorite = false;
    b.tags.clear();          // b is a copy of a; give it distinct metadata
    b.category.clear();
    CHECK(db.Upsert(b).ok());

    CHECK(db.Count() == 2);
    auto got = db.Get(a.uuid);
    CHECK(got.ok() && got.value().name == "sermon-notes");
    CHECK(db.ByType(content::AssetType::Image).size() == 1);
    CHECK(db.ByTag("sermon").size() == 1);
    CHECK(db.ByCategory("teaching").size() == 1);
    CHECK(db.ByExtension("PNG").size() == 1);
    CHECK(db.Favorites().size() == 1);
    auto byPath = db.FindByPath("notes/sermon-notes.txt");
    CHECK(byPath.has_value() && byPath->uuid == a.uuid);
    CHECK(db.Remove(a.uuid).ok());
    CHECK(db.Count() == 1);
    CHECK(db.Remove(content::Uuid::Generate()).error().code == Err::NotFound);
}
void TestCamsRegistryCache() {
    content::AssetRegistry reg;
    content::RuntimeAsset ra;
    ra.meta.uuid = content::Uuid::Generate();
    ra.meta.name = "loaded";
    ra.bytes = std::vector<uint8_t>(1024, 7);
    ra.meta.hash = "deadbeefdeadbeef";
    CHECK(reg.Register(ra).ok());
    CHECK(reg.Count() == 1);
    auto acq = reg.Acquire(ra.meta.uuid);
    CHECK(acq.ok());
    CHECK(acq.value()->refCount == 1);
    reg.Release(ra.meta.uuid);
    CHECK(acq.value()->refCount == 0);
    CHECK(reg.SetPinned(ra.meta.uuid, true).ok());
    CHECK(reg.IsPinned(ra.meta.uuid));
    CHECK(reg.AddAlias("primary", ra.meta.uuid).ok());
    auto alias = reg.ResolveAlias("primary");
    CHECK(alias.has_value() && alias.value() == ra.meta.uuid);
    auto dup = reg.FindByHash("deadbeefdeadbeef");
    CHECK(dup.has_value() && dup.value() == ra.meta.uuid);
    CHECK(reg.Unregister(ra.meta.uuid).error().code == Err::InvalidState);   // pinned
    CHECK(reg.SetPinned(ra.meta.uuid, false).ok());
    CHECK(reg.Unregister(ra.meta.uuid).ok());
    CHECK(reg.Count() == 0);

    // Cache: LRU + pin + shrink
    content::AssetCache cache(64);   // tiny capacity forces eviction
    auto mk = [](const std::string& name, size_t size) {
        auto a = std::make_shared<content::RuntimeAsset>();
        a->meta.uuid = content::Uuid::Generate();
        a->meta.name = name;
        a->bytes.assign(size, 'x');
        return a;
    };
    auto c1 = mk("c1", 40), c2 = mk("c2", 40);
    CHECK(cache.Put(c1->meta.uuid, c1).ok());
    CHECK(cache.Get(c1->meta.uuid) != nullptr);   // hit -> LRU front
    CHECK(cache.Get(c1->meta.uuid) != nullptr);   // second hit
    CHECK(cache.Put(c2->meta.uuid, c2).ok());     // exceeds 64 -> evicts c1
    CHECK(cache.Contains(c1->meta.uuid) == false);
    CHECK(cache.Contains(c2->meta.uuid));
    CHECK(cache.Get(c1->meta.uuid) == nullptr);   // miss
    auto stats = cache.Stats();
    CHECK(stats.evictions >= 1);
    CHECK(stats.hits >= 2 && stats.misses >= 1);

    content::AssetCache pinCache(100);
    auto p1 = mk("p1", 60);
    CHECK(pinCache.Put(p1->meta.uuid, p1).ok());
    CHECK(pinCache.EvictAllUnpinned() == 60);
    CHECK(!pinCache.Contains(p1->meta.uuid));
}
void TestCamsLoader() {
    auto loader = content::AssetLoader([](const content::AssetMetadata& m) {
        return Result<std::vector<uint8_t>>{std::vector<uint8_t>{m.name.begin(), m.name.end()}};
    });
    content::AssetMetadata m;
    m.uuid = content::Uuid::Generate();
    m.name = "payload";
    bool progressed = false;
    auto r = loader.Load(m, [&](const content::LoadProgress& p) {
        progressed = p.fraction == 1.0;
    });
    CHECK(r.ok());
    CHECK(std::string(r.value().begin(), r.value().end()) == "payload");
    CHECK(progressed);
    // Hash is stable + distinct for different data.
    CHECK(content::AssetLoader::Hash((const uint8_t*)"abc", 3) ==
          content::AssetLoader::Hash((const uint8_t*)"abc", 3));
    CHECK(content::AssetLoader::Hash((const uint8_t*)"abc", 3) !=
          content::AssetLoader::Hash((const uint8_t*)"abd", 3));

    // Async path (ThreadPool is live in main()).
    std::atomic<bool> done{false};
    content::AssetMetadata m2;
    m2.uuid = content::Uuid::Generate();
    m2.name = "async";
    auto cancel = content::MakeCancelToken();
    loader.LoadAsync(m2, cancel,
                     [&](Result<std::vector<uint8_t>> rr, const content::LoadProgress&) {
                         CHECK(rr.ok());
                         done.store(true);
                     });
    for (int i = 0; i < 200 && !done.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(done.load());
}
void TestCamsImportExport() {
    content::ImportManager im;
    CHECK(im.Register(std::make_shared<content::TextImporter>()).ok());
    CHECK(im.Register(std::make_shared<content::JsonImporter>()).ok());
    CHECK(im.Register(std::make_shared<content::ImageImporter>()).ok());
    CHECK(im.Register(std::make_shared<content::TextImporter>()).error().code == Err::AlreadyExists);
    CHECK(im.ImporterCount() == 3);
    auto exts = im.SupportedExtensions();
    CHECK(std::find(exts.begin(), exts.end(), "txt") != exts.end());
    CHECK(im.FindForExtension(".TXT") != nullptr);
    CHECK(im.FindForExtension("mp4") == nullptr);   // no video importer yet

    content::ImportContext ctx;
    ctx.fileName = "notes.txt";
    ctx.destinationDir = "docs";
    ctx.tags = {"a", "b"};
    std::vector<uint8_t> bytes{'h', 'i'};
    auto res = im.Import("notes.txt", bytes, ctx);
    CHECK(res.ok());
    CHECK(res.value().assets.size() == 1);
    CHECK(res.value().assets[0].meta.type == content::AssetType::Text);
    CHECK(res.value().assets[0].meta.path == "docs/notes.txt");
    CHECK(res.value().assets[0].meta.uuid != content::Uuid{});

    // Json importer validates content.
    std::vector<uint8_t> bad{'n', 'o', 't', ' ', 'j', 's', 'o', 'n'};
    auto badRes = im.Import("x.json", bad, ctx);
    CHECK(!badRes.ok());
    const std::string goodJson = "{\"k\":1}";
    std::vector<uint8_t> good(goodJson.begin(), goodJson.end());
    auto goodRes = im.Import("x.json", good, ctx);
    CHECK(goodRes.ok());

    // No importer -> UnsupportedFormat
    CHECK(im.Import("x.mp4", bytes, ctx).error().code == Err::Content_UnsupportedFormat);

    // Exporters
    content::ExportManager em;
    CHECK(em.Register(std::make_shared<content::TextExporter>()).ok());
    CHECK(em.Register(std::make_shared<content::JsonExporter>()).ok());
    CHECK(em.Register(std::make_shared<content::PackageExporter>()).ok());
    CHECK(em.ExporterCount() == 3);
    auto fmts = em.SupportedFormats();
    CHECK(std::find(fmts.begin(), fmts.end(), "json") != fmts.end());

    const std::string outDir = "/tmp/bps_cams_export";
    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    std::filesystem::create_directories(outDir, ec);

    content::AssetMetadata meta;
    meta.uuid = content::Uuid::Generate();
    meta.name = "song.txt";
    meta.type = content::AssetType::Text;
    meta.path = "songs/song.txt";
    content::ExportContext ectx;
    ectx.meta = meta;
    ectx.bytes = std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'};
    ectx.destinationPath = outDir + "/out.txt";
    CHECK(em.Export("text", ectx).ok());
    CHECK(std::filesystem::exists(ectx.destinationPath, ec));
    ectx.destinationPath = outDir + "/out.json";
    CHECK(em.Export("json", ectx).ok());
    CHECK(std::filesystem::exists(ectx.destinationPath, ec));
    ectx.meta.type = content::AssetType::Package;
    ectx.destinationPath = outDir + "/out.zip";
    CHECK(em.Export("package", ectx).ok());
    CHECK(std::filesystem::exists(ectx.destinationPath, ec));
    CHECK(em.Export("nope", ectx).error().code == Err::Content_UnsupportedFormat);

    std::filesystem::remove_all(outDir, ec);
}
static content::SearchQuery MakeQuery(std::string text = {}, content::AssetType type =
                                      content::AssetType::Unknown,
                                      std::string tag = {}, std::string category = {},
                                      std::string extension = {}, std::string author = {},
                                      bool favsOnly = false) {
    content::SearchQuery q;
    q.text = std::move(text);
    q.type = type;
    q.tag = std::move(tag);
    q.category = std::move(category);
    q.extension = std::move(extension);
    q.author = std::move(author);
    q.favoritesOnly = favsOnly;
    return q;
}
void TestCamsIndexerSearch() {
    content::AssetIndexer ix;
    content::AssetMetadata a;
    a.uuid = content::Uuid::Generate();
    a.name = "Amazing Grace";
    a.type = content::AssetType::Song;
    a.tags = {"hymn"};
    a.category = "worship";
    a.description = "classic hymn";
    a.favorite = true;
    a.path = "songs/amazing-grace.song";
    ix.Index(a);
    content::AssetMetadata b;
    b.uuid = content::Uuid::Generate();
    b.name = "Grace Church Logo";
    b.type = content::AssetType::Image;
    b.tags = {"logo"};
    b.path = "brand/logo.png";
    b.favorite = false;
    ix.Index(b);
    CHECK(ix.IndexedCount() == 2);
    CHECK(ix.TokenCount() > 0);

    auto exact = ix.Search(MakeQuery("grace"));
    CHECK(exact.size() == 2);   // both match "grace"
    auto partial = ix.Search(MakeQuery("amaz"));
    CHECK(partial.size() == 1 && partial[0].meta.uuid == a.uuid);
    auto tagFilter = ix.Search(MakeQuery("grace", content::AssetType::Unknown, "hymn"));
    CHECK(tagFilter.size() == 1);
    auto typeFilter = ix.Search(MakeQuery("grace", content::AssetType::Image));
    CHECK(typeFilter.size() == 1 && typeFilter[0].meta.uuid == b.uuid);
    auto favs = ix.Search(MakeQuery("grace", content::AssetType::Unknown, "", "", "", "", true));
    CHECK(favs.size() == 1 && favs[0].meta.uuid == a.uuid);
    auto ext = ix.Search(MakeQuery("grace", content::AssetType::Unknown, "", "", "png"));
    CHECK(ext.size() == 1 && ext[0].meta.uuid == b.uuid);
    auto none = ix.Search(MakeQuery("zzz"));
    CHECK(none.empty());
    auto empty = ix.Search(content::SearchQuery{});
    CHECK(empty.size() == 2);   // no term -> all docs
    ix.Remove(a.uuid);
    CHECK(ix.IndexedCount() == 1);
}
void TestCamsWatcherValidator() {
    // Watcher over a memory VFS
    auto mem = std::make_shared<content::MemoryVfs>("w");
    CHECK(mem->WriteText("one.txt", "1").ok());
    content::AssetWatcher w;
    std::vector<content::WatchChange> seen;
    w.SetCallback([&](const content::WatchChange& c) { seen.push_back(c); });
    CHECK(w.AddRoot("w:/", [&](std::string_view) -> std::optional<std::vector<std::string>> {
        auto l = mem->List("");
        if (!l.ok()) return std::nullopt;
        return l.value();
    }).ok());
    CHECK(w.RootCount() == 1);
    CHECK(w.Poll().empty());   // baseline: no changes
    CHECK(mem->WriteText("two.txt", "2").ok());
    auto changes = w.Poll();
    CHECK(changes.size() == 1);
    CHECK(changes[0].kind == content::WatchChange::Kind::Added);
    CHECK(changes[0].path == "two.txt");
    CHECK(mem->Remove("one.txt").ok());
    auto removed = w.Poll();
    bool sawRemoved = false;
    for (const auto& c : removed) if (c.kind == content::WatchChange::Kind::Removed) sawRemoved = true;
    CHECK(sawRemoved);
    CHECK(seen.size() >= 1);   // callback fired

    // Validator
    content::AssetValidator v;
    content::AssetMetadata good;
    good.uuid = content::Uuid::Generate();
    good.name = "ok";
    good.path = "x.txt";
    auto goodRep = v.Validate(good, [](const std::string&) { return std::optional<bool>{true}; },
                              [](const std::string&) { return std::optional<uint64_t>{10}; });
    CHECK(goodRep.Valid());
    auto badRep = v.Validate(good, [](const std::string&) { return std::optional<bool>{false}; },
                             nullptr);
    CHECK(!badRep.Valid());
    CHECK(badRep.issues[0].code == "missing_file");

    content::AssetMetadata dup1, dup2;
    dup1.uuid = dup2.uuid = content::Uuid::Generate();   // same uuid twice
    dup1.name = dup2.name = "dup";
    auto libRep = v.ValidateLibrary({dup1, dup2});
    bool sawDup = false;
    for (const auto& i : libRep.issues) if (i.code == "duplicate_uuid") sawDup = true;
    CHECK(sawDup);
}
void TestCamsSerializerCompressor() {
    content::AssetMetadata m;
    m.uuid = content::Uuid::Generate();
    m.name = "test asset";
    m.type = content::AssetType::Song;
    m.path = "songs/test.song";
    m.sizeBytes = 42;
    m.tags = {"a", "b"};
    m.category = "cat";
    m.author = "author";
    m.description = "desc";
    m.favorite = true;
    auto doc = content::AssetSerializer::ToJson(m);
    auto back = content::AssetSerializer::FromJson(doc);
    CHECK(back.ok());
    CHECK(back.value().uuid == m.uuid);
    CHECK(back.value().name == m.name);
    CHECK(back.value().type == m.type);
    CHECK(back.value().tags == m.tags);
    CHECK(back.value().favorite);

    // Library round-trip
    content::AssetMetadata n = m;
    n.uuid = content::Uuid::Generate();
    auto lib = content::AssetSerializer::LibraryToJson({m, n});
    auto libBack = content::AssetSerializer::LibraryFromJson(lib);
    CHECK(libBack.ok() && libBack.value().size() == 2);

    // Migration: future version rejected
    json::Value::Object o;
    o["schemaVersion"] = json::Value::Number(99);
    o["assets"] = json::Value(json::Value::Array{});
    CHECK(!content::AssetSerializer::LibraryFromJson(json::Value(std::move(o))).ok());

    // Compressors
    content::StoredCompressor stored;
    std::vector<uint8_t> data(1000, 'a');
    auto c1 = stored.Compress(data.data(), data.size());
    CHECK(c1 == data);
    auto d1 = stored.Decompress(c1.data(), c1.size());
    CHECK(d1.ok() && d1.value() == data);

    content::RleCompressor rle;
    auto c2 = rle.Compress(data.data(), data.size());
    CHECK(c2.size() < data.size());   // runs compress well
    auto d2 = rle.Decompress(c2.data(), c2.size());
    CHECK(d2.ok() && d2.value() == data);
    // A trailing byte that can't form a run header is dropped (lenient); a
    // run header with no byte after it is a hard parse error.
    CHECK(rle.Decompress((const uint8_t*)"\xff", 1).ok());
    const uint8_t danglingRun[1] = {0x02};   // run=2, but no byte follows
    CHECK(rle.Decompress(danglingRun, 1).error().code == Err::ParseError);

    // DeflateCompressor (zlib when available; stored fallback otherwise).
    content::DeflateCompressor deflate;
    // Highly repetitive data must round-trip byte-exactly and compress well.
    std::vector<uint8_t> rep;
    for (int i = 0; i < 5000; ++i) rep.push_back(static_cast<uint8_t>(i % 7));
    auto cd = deflate.Compress(rep.data(), rep.size());
    auto dd = deflate.Decompress(cd.data(), cd.size());
    CHECK(dd.ok());
    CHECK(dd.value() == rep);
#ifdef BPS_HAVE_ZLIB
    CHECK(cd.size() < rep.size() / 2);   // runs deflate extremely well
#endif
    // Mixed / incompressible data still round-trips.
    std::vector<uint8_t> rand;
    uint32_t x = 12345;
    for (int i = 0; i < 4096; ++i) {
        x = x * 1664525u + 1013904223u;
        rand.push_back(static_cast<uint8_t>(x >> 24));
    }
    auto cr = deflate.Compress(rand.data(), rand.size());
    auto dr = deflate.Decompress(cr.data(), cr.size());
    CHECK(dr.ok() && dr.value() == rand);
    // Empty input round-trips cleanly.
    CHECK(deflate.Decompress(nullptr, 0).ok());
    CHECK(deflate.Decompress(nullptr, 0).value().empty());
#ifdef BPS_HAVE_ZLIB
    // Corrupt stream fails cleanly, not with a crash.
    const uint8_t garbage[8] = {0x78, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!deflate.Decompress(garbage, sizeof garbage).ok());
#endif
}
void TestCamsThumbnail() {
    content::ThumbnailManager tm;
    content::AssetMetadata m;
    m.uuid = content::Uuid::Generate();
    m.name = "Sunset";
    m.type = content::AssetType::Image;
    auto svg = tm.GeneratePlaceholder(m);
    CHECK(svg.find("<svg") != std::string::npos);
    CHECK(tm.Cached(m.uuid, "small") == svg);
    auto svg2 = tm.GeneratePlaceholder(m, {48, 48, "small"});
    CHECK(svg2.find("width='48'") != std::string::npos);
    CHECK(tm.CacheCount() >= 1);

    // PNG + JPEG dimension extraction
    const uint8_t png[29] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                             0, 0, 0, 0x0D, 'I', 'H', 'D', 'R',
                             0, 0, 0, 100, 0, 0, 0, 50, 8, 0, 0, 0, 0};
    auto dims = content::ThumbnailManager::ImageDimensions(png, sizeof png);
    CHECK(dims.first == 100 && dims.second == 50);
    // JPEG: SOI + SOF0 (height 0x30=48, width 0x40=64)
    const uint8_t jpg[26] = {0xFF, 0xD8,
                             0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x30, 0x00, 0x40, 1,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    auto jdims = content::ThumbnailManager::ImageDimensions(jpg, sizeof jpg);
    CHECK(jdims.first == 64 && jdims.second == 48);
    CHECK(content::ThumbnailManager::ImageDimensions((const uint8_t*)"junk", 4) ==
          std::make_pair(0, 0));
}
void TestCamsContentManager() {
    namespace fs = std::filesystem;
    const std::string dir = "/tmp/bps_cams_lib";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto& cm = content::ContentManager::Instance();
    CHECK(cm.Initialize().ok());
    CHECK(cm.MountDisk("lib", dir).ok());
    CHECK(cm.Start().ok());

    // Create + metadata
    content::ImportOptions opts;
    opts.tags = {"worship"};
    opts.category = "songs";
    auto u1 = cm.CreateText("hymn.txt", content::AssetType::Text, "Amazing Grace", opts);
    CHECK(u1.ok());
    auto meta = cm.GetMetadata(u1.value());
    CHECK(meta.ok() && meta.value().name == "hymn.txt");
    CHECK(meta.value().type == content::AssetType::Text);
    CHECK(meta.value().tags == std::vector<std::string>{"worship"});
    CHECK(cm.AssetCount() == 1);
    CHECK(cm.FindByPath("hymn.txt").has_value());

    // Load + read
    auto loaded = cm.Load(u1.value());
    CHECK(loaded.ok());
    CHECK(loaded.value()->text == "Amazing Grace");
    CHECK(cm.LoadedCount() >= 1);
    auto txt = cm.ReadText(u1.value());
    CHECK(txt.ok() && txt.value() == "Amazing Grace");
    CHECK(cm.GetCacheStats().hits >= 1);

    // Save (modify)
    CHECK(cm.SaveText(u1.value(), "Amazing Grace (updated)").ok());
    CHECK(cm.ReadText(u1.value()).value() == "Amazing Grace (updated)");

    // Rename + Move
    CHECK(cm.Rename(u1.value(), "grace.txt").ok());
    CHECK(cm.GetMetadata(u1.value()).value().path == "grace.txt");
    CHECK(cm.Move(u1.value(), "songs/grace.txt").ok());
    CHECK(cm.GetMetadata(u1.value()).value().path == "songs/grace.txt");

    // Duplicate
    auto dup = cm.Duplicate(u1.value());
    CHECK(dup.ok() && dup.value() != u1.value());
    CHECK(cm.AssetCount() == 2);
    CHECK(cm.ReadText(dup.value()).ok());

    // Import a host file through the importer pipeline
    const std::string host = dir + "/hostfile.json";
    {
        std::ofstream f(host);
        f << "{\"title\":\"Imported\"}";
    }
    auto ui = cm.ImportFile(host, {});
    CHECK(ui.ok());
    CHECK(cm.GetMetadata(ui.value()).value().type == content::AssetType::Json);
    CHECK(cm.AssetCount() == 3);
    CHECK(cm.ReadText(ui.value()).value().find("title") != std::string::npos);

    // Search
    auto results = cm.Search(MakeQuery("grace"));
    CHECK(results.size() == 2);   // original + duplicate
    auto byCat = cm.Search(MakeQuery("grace", content::AssetType::Unknown, "", "songs"));
    CHECK(byCat.size() == 2);

    // Pin + prefetch + cache control
    CHECK(cm.Pin(u1.value(), true).ok());
    CHECK(cm.Prefetch(ui.value()).ok());
    (void)cm.Evict();
    (void)cm.ShrinkCache(0.5);

    // Export single + package
    const std::string out = dir + "/out.txt";
    CHECK(cm.Export(u1.value(), "text", out).ok());
    CHECK(fs::exists(out, ec));
    const std::string pkg = dir + "/pkg.zip";
    CHECK(cm.ExportPackage({u1.value(), dup.value()}, pkg).ok());
    CHECK(fs::exists(pkg, ec));
    // Mount the exported package and read it back
    auto zm = cm.MountZip("pkg", pkg);
    if (zm.ok()) {
        CHECK(zm.value()->ReadText("songs/grace.txt").ok());
        CHECK(cm.Unmount("pkg").ok());
    }
    CHECK(zm.ok());   // package must have been produced

    // Watcher: register a change
    CHECK(cm.WatchRoot("lib", "").ok());
    {
        std::ofstream f(dir + "/watched.txt");
        f << "new file";
    }
    cm.PollWatch();
    bool sawWatched = false;
    for (const auto& m : cm.List())
        if (m.name == "watched.txt") sawWatched = true;
    CHECK(sawWatched);

    // Validation: delete a file out from under the db to force a missing_file
    // (the asset was moved to songs/grace.txt earlier)
    fs::remove(dir + "/songs/grace.txt", ec);
    auto report = cm.ValidateAll();
    bool sawMissing = false;
    for (const auto& i : report.issues) if (i.code == "missing_file") sawMissing = true;
    CHECK(sawMissing);

    // Reload + Reset
    CHECK(cm.Reload().ok());
    cm.ReindexAll();
    CHECK(cm.GetHealth().state == HealthState::Healthy ||
          cm.GetHealth().state == HealthState::Degraded);

    // Delete
    CHECK(cm.Delete(dup.value()).ok());
    CHECK(cm.AssetCount() == 3);   // grace.txt, watched.txt, hostfile.json
    CHECK(cm.Delete(ui.value()).ok());

    // Persistence: Shutdown flushes to the DatabaseManager; re-init restores.
    CHECK(cm.Shutdown().ok());
    CHECK(cm.Initialize().ok());
    bool restored = false;
    for (const auto& m : cm.List())
        if (m.name == "grace.txt" || m.name == "watched.txt") restored = true;
    CHECK(restored);

    (void)cm.Stop();
    (void)cm.Reset();
    CHECK(cm.AssetCount() == 0);

    fs::remove_all(dir, ec);
}
