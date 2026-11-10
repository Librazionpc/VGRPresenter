#include "TestHarness.hpp"

#include "platform/PlatformServices.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// The platform cage, exercised through the PAL (never through a concrete
// backend): Qt is the only implementation of these services now, so a change
// to QtFilesystem/QtLocale/QtLibrary has to keep every one of them green.
// ---------------------------------------------------------------------------

void TestQtFilesystemContract()
{
    using namespace bps;
    auto &plat = platform::PlatformAccessor::Get();
    auto &fs = plat.Filesystem();
    // A real temp folder on THIS OS ("C:/Users/.../Temp", not "/tmp").
    const std::string root = plat.Paths().TempDir() + "/bps_qt_filesystem";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root).ok());

    // 1. A UTF-8 name survives the round trip — the reason Qt owns the path
    //    conversion (one conversion, not one per OS backend).
    const std::string unicode = root + "/über_документ_文件.txt";
    const std::string text = "héllo ✓";
    CHECK(fs.Write(unicode, text).ok());
    auto read = fs.ReadText(unicode);
    CHECK(read.ok() && read.value() == text);
    CHECK(fs.IsRegularFile(unicode));
    CHECK(fs.FileSize(unicode).ok() && fs.FileSize(unicode).value() == text.size());

    // 2. Append is a real append, and the atomic save replaces a shorter file
    //    without leaving the longer one's tail behind.
    CHECK(fs.Append(root + "/log.txt", "a").ok());
    CHECK(fs.Append(root + "/log.txt", "b").ok());
    CHECK(fs.ReadText(root + "/log.txt").value() == "ab");
    CHECK(fs.WriteAtomic(root + "/state/a.json", "first version").ok());
    CHECK(fs.WriteAtomic(root + "/state/a.json", "v2").ok());
    CHECK(fs.ReadText(root + "/state/a.json").value() == "v2");
    CHECK(!fs.Exists(root + "/state/a.json.tmp"));

    // 3. Metadata: a regular file with a size and a usable mtime; the empty path
    //    is "nothing to describe", not an error.
    auto meta = fs.Metadata(root + "/log.txt");
    CHECK(meta.ok() && meta.value().isRegularFile && !meta.value().isDirectory);
    CHECK(meta.value().sizeBytes == 2);
    CHECK(meta.value().modifiedEpochNs > 0);
    auto none = fs.Metadata("");
    CHECK(none.ok() && !none.value().isRegularFile && !none.value().isDirectory);

    // 4. Absolute() answers a path that is not relative, without requiring the
    //    file to exist (callers use it on names they are about to create).
    auto abs = fs.Absolute("relative/thing.json");
    CHECK(abs.ok() && !abs.value().empty());
    CHECK(!std::string(abs.value()).starts_with("relative"));

    // 5. FindFiles walks recursively and honours the ".ext" spelling; Enumerate
    //    lists one directory only.
    CHECK(fs.CreateDirectories(root + "/find/deep").ok());
    CHECK(fs.Write(root + "/find/one.json", "{}").ok());
    CHECK(fs.Write(root + "/find/deep/two.json", "{}").ok());
    CHECK(fs.Write(root + "/find/deep/skip.txt", "x").ok());
    auto found = fs.FindFiles(root + "/find", ".json");
    CHECK(found.ok() && found.value().size() == 2);
    auto entries = fs.Enumerate(root + "/find");
    CHECK(entries.ok() && entries.value().size() == 2);

    // 6. A temp file is created, empty, and lives in the system temp dir.
    auto temp = fs.CreateTempFile("bps-unit-");
    CHECK(temp.ok());
    if (temp.ok()) {
        CHECK(fs.IsRegularFile(temp.value()));
        CHECK(fs.FileSize(temp.value()).value() == 0);
        (void)fs.Remove(temp.value());
        CHECK(!fs.Exists(temp.value()));
    }

    // 7. A symlink round-trips where the OS allows one (Windows without the
    //    developer-mode privilege refuses: that is a platform limit, not a bug).
    const std::string link = root + "/link.txt";
    if (fs.CreateSymlink(unicode, link).ok()) {
        auto target = fs.ReadSymlink(link);
        CHECK(target.ok() && !target.value().empty());
    }

    // 8. Join/Extension keep the engine's spelling rules.
    CHECK(fs.Join("/tmp/x", "y.json") == "/tmp/x/y.json");
    CHECK(fs.Extension("/tmp/x/y.json") == ".json");
    CHECK(fs.Extension("/tmp/x/.hidden").empty());

    (void)fs.RemoveAll(root);
}

void TestQtFilesystemWatch()
{
    using namespace bps;
    auto &plat = platform::PlatformAccessor::Get();
    auto &fs = plat.Filesystem();
    const std::string root = plat.Paths().TempDir() + "/bps_qt_filesystem_watch";
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(root).ok());
    const std::string file = root + "/watched.txt";
    CHECK(fs.Write(file, "start").ok());

    std::atomic<int> hits{0};
    auto token = fs.Watch(file, [&hits](std::string_view) { hits.fetch_add(1); });
    CHECK(token.ok());
    if (!token.ok()) {
        (void)fs.RemoveAll(root);
        return;
    }
    // The fingerprint is the file's write time: let the clock move past the
    // stamp the watcher started with before touching the file.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(fs.Append(file, " more").ok());
    for (int i = 0; i < 25 && hits.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(hits.load() > 0);
    CHECK(fs.CancelWatch(token.value()).ok());
    // A cancelled watch is not called again.
    const int after = hits.load();
    CHECK(fs.Append(file, " again").ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(hits.load() == after);
    (void)fs.RemoveAll(root);
}

void TestQtServicesLocaleAndLibrary()
{
    using namespace bps;
    auto &locale = platform::PlatformAccessor::Get().Locale();
    const platform::LocaleInfo info = locale.Current();
    CHECK(!info.language.empty());
    CHECK(!info.dateFormat.empty());
    CHECK(!info.timeFormat.empty());
    CHECK(locale.Language() == info.language);

    auto &lib = platform::PlatformAccessor::Get().Library();
    // A name that cannot be a library: the loader must refuse it and say why,
    // instead of handing back a handle that fails later at symbol lookup.
    auto missing = lib.Load("this-library-does-not-exist-7f3a1");
    CHECK(!missing.ok());
    CHECK(!lib.LastError().empty());
    CHECK(!lib.Symbol(nullptr, "anything").ok());
}
