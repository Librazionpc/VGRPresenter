#include "TestHarness.hpp"

#include "platform/PlatformServices.hpp"

#include <QByteArray>
#include <QtGlobal>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// WHAT A PLATFORM BACKEND MAY NOT CHANGE.
//
// Swapping the implementation behind a PAL interface is only safe when the
// answers the rest of the app has ALREADY STORED keep meaning the same thing.
// These tests pin those answers, so a change in spelling or folder layout fails
// here instead of in someone's library:
//
//   * how a listed file's path is spelled  — the media library hashes it into
//     each item's id and matches it against paths saved in shows, video sources
//     and per-file settings;
//   * where the app's data folders are     — a different folder is an empty
//     profile: every show, setting and library would appear to be gone;
//   * that removing what is already gone succeeds — cleanup code relies on it;
//   * that permission bits are the POSIX mode bits the rest of the engine reads.
//
// Qt is the only backend now, so the expectations are stated directly instead
// of being compared against a second implementation.
// ---------------------------------------------------------------------------

namespace {
namespace stdfs = std::filesystem;

std::string Utf8(const stdfs::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

stdfs::path FromUtf8(const std::string& s) {
    return stdfs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

void ExpectFoldersUnder(const bps::platform::IPaths& paths, const std::string& root) {
    struct Row { const char* name; std::string value; } rows[] = {
        {"ConfigDir", paths.ConfigDir()},
        {"UserDataDir", paths.UserDataDir()},
        {"AppDataDir", paths.AppDataDir()},
        {"CacheDir", paths.CacheDir()},
        {"LogDir", paths.LogDir()},
        {"PluginDir", paths.PluginDir()},
        {"AssetDir", paths.AssetDir()},
    };
    for (const Row& r : rows) {
        CHECK(!r.value.empty());
        CHECK(r.value.rfind(root, 0) == 0);   // starts with the profile root
    }
}
} // namespace

void TestPlatformListingSpelling() {
    using namespace bps;
    auto& plat = platform::PlatformAccessor::Get();
    auto& fs = plat.Filesystem();

    // A root spelled the way the OS spells it (the way a folder the user picked arrives).
    const stdfs::path rootPath = FromUtf8(plat.Paths().TempDir()) / "bps_listing_contract";
    const std::string root = Utf8(rootPath);
    (void)fs.RemoveAll(root);
    CHECK(fs.CreateDirectories(Utf8(rootPath / "sub" / "deeper")).ok());
    CHECK(fs.Write(Utf8(rootPath / "a clip.mp4"), "x").ok());
    CHECK(fs.Write(Utf8(rootPath / "sub" / "b.png"), "x").ok());
    CHECK(fs.Write(Utf8(rootPath / "sub" / "deeper" / "c.mp3"), "x").ok());

    // FindFiles: every file, spelled exactly as `root / relative` in the OS's own separators.
    std::vector<std::string> expected = {
        Utf8(rootPath / "a clip.mp4"),
        Utf8(rootPath / "sub" / "b.png"),
        Utf8(rootPath / "sub" / "deeper" / "c.mp3"),
    };
    std::sort(expected.begin(), expected.end());
    auto found = fs.FindFiles(root, "");
    CHECK(found.ok());
    if (found.ok()) {
        std::vector<std::string> got = found.value();
        std::sort(got.begin(), got.end());
        CHECK(got == expected);
        if (got != expected)
            for (const std::string& g : got) std::fprintf(stderr, "  FindFiles gave: %s\n", g.c_str());
    }

    // Enumerate: one level, files AND folders, the same spelling.
    std::vector<std::string> level = {Utf8(rootPath / "a clip.mp4"), Utf8(rootPath / "sub")};
    std::sort(level.begin(), level.end());
    auto listed = fs.Enumerate(root);
    CHECK(listed.ok());
    if (listed.ok()) {
        std::vector<std::string> got = listed.value();
        std::sort(got.begin(), got.end());
        CHECK(got == level);
        if (got != level)
            for (const std::string& g : got) std::fprintf(stderr, "  Enumerate gave: %s\n", g.c_str());
    }

    // A root that already ends in a separator lists the same entries under the
    // same spelling: callers hand over folders straight from a picker, which
    // often ends in one, and the stored paths must not differ because of it.
    const char sep = static_cast<char>(stdfs::path::preferred_separator);
    const std::string rootWithSep = (!root.empty() && root.back() == sep) ? root : root + sep;
    auto trailing = fs.Enumerate(rootWithSep);
    CHECK(trailing.ok());
    if (trailing.ok()) {
        std::vector<std::string> got = trailing.value();
        std::sort(got.begin(), got.end());
        CHECK(got == level);
    }

    // Removing what is not there is success; removing a file really removes it.
    CHECK(fs.Remove(Utf8(rootPath / "never-existed.bin")).ok());
    CHECK(fs.Remove(Utf8(rootPath / "a clip.mp4")).ok());
    CHECK(!fs.Exists(Utf8(rootPath / "a clip.mp4")));

    // Permissions speak POSIX mode bits: a file we just wrote is readable by its owner, and nothing
    // outside rwxrwxrwx is ever set.
    auto meta = fs.Metadata(Utf8(rootPath / "sub" / "b.png"));
    CHECK(meta.ok());
    if (meta.ok()) {
#ifndef Q_OS_WIN
        CHECK((meta.value().permissions & 0400u) != 0);    // owner-readable
#endif
        CHECK((meta.value().permissions & ~0777u) == 0);   // never outside rwxrwxrwx
    }

    (void)fs.RemoveAll(root);
}

void TestPlatformPathsParity() {
    using namespace bps;
    auto& paths = platform::PlatformAccessor::Get().Paths();

    // The data folders exist and are absolute (a relative root here would put a
    // profile inside whatever directory the app was launched from).
    CHECK(!paths.ConfigDir().empty());
    CHECK(!paths.UserDataDir().empty());
    CHECK(stdfs::path(FromUtf8(paths.ConfigDir())).is_absolute());
    CHECK(stdfs::path(FromUtf8(paths.UserDataDir())).is_absolute());

    // The profile override: every folder follows it, under its own sub-tree, so
    // a portable install and the dev profile keep one layout.
    const QByteArray saved = qgetenv("VGR_PROFILE_DIR");
    const bool hadProfile = qEnvironmentVariableIsSet("VGR_PROFILE_DIR");
    // Spelled with this OS's separator, the same way the folders come back.
    const std::string root = Utf8(FromUtf8(paths.TempDir()) / "bps-contract-profile");
    qputenv("VGR_PROFILE_DIR", QByteArray::fromStdString(root));
    ExpectFoldersUnder(paths, root);
    if (hadProfile) qputenv("VGR_PROFILE_DIR", saved);
    else qunsetenv("VGR_PROFILE_DIR");

    // The locale answers something the UI can format with, in the ISO spellings
    // the settings screens store.
    auto& locale = platform::PlatformAccessor::Get().Locale();
    const platform::LocaleInfo info = locale.Current();
    CHECK(!info.language.empty());
    CHECK(info.language.size() >= 2);
    CHECK(!info.dateFormat.empty());
    CHECK(!info.timeFormat.empty());
    CHECK(!info.currency.empty());
}

// ---------------------------------------------------------------------------
// THE CAMERA FEED CONTRACT. The HDMI output draws a taken camera from IVideo::PreviewFrameShared: the
// tap's newest frame without a private copy. A backend that does not override it gets the base wrapper
// around PreviewFramePixels, and that wrapper must hand back exactly the same pixels, size and counter —
// and an empty answer (never a null-buffer "frame") when there is nothing to show.
// ---------------------------------------------------------------------------
namespace {
class FakeVideo final : public bps::platform::IVideo {
public:
    std::vector<bps::platform::VideoDeviceInfo> Enumerate() const override { return {}; }
    std::string Fingerprint() const override { return {}; }
    DecodedFrame PreviewFramePixels(const std::string& deviceId) override {
        ++calls;
        if (deviceId != "cam") return {};
        DecodedFrame f;
        f.width = 4;
        f.height = 2;
        f.seq = 41;
        f.rgba.resize(4u * 2u * 4u);
        for (size_t i = 0; i < f.rgba.size(); ++i) f.rgba[i] = static_cast<uint8_t>(i * 3);
        return f;
    }
    int calls = 0;
};
} // namespace

void TestPreviewFrameSharedContract() {
    FakeVideo video;
    const auto shared = video.PreviewFrameShared("cam");
    CHECK(!shared.empty());
    CHECK(shared.width == 4 && shared.height == 2 && shared.seq == 41);
    CHECK(shared.rgba && shared.rgba->size() == 32u);
    if (shared.rgba && shared.rgba->size() == 32u) {
        bool same = true;
        for (size_t i = 0; i < 32u; ++i) same = same && (*shared.rgba)[i] == static_cast<uint8_t>(i * 3);
        CHECK(same);
    }
    // Two reads are two independent buffers for the base wrapper (it copies); the consumer may keep one.
    const auto again = video.PreviewFrameShared("cam");
    CHECK(again.rgba && again.rgba != shared.rgba);

    // Nothing to show: empty, with no buffer — the feed's "wait and poll again" signal.
    const auto none = video.PreviewFrameShared("unplugged");
    CHECK(none.empty());
    CHECK(!none.rgba);
    // A frame with a size but no pixels is not a frame.
    bps::platform::IVideo::SharedFrame hollow;
    hollow.width = 1920;
    hollow.height = 1080;
    CHECK(hollow.empty());
}

// ---------------------------------------------------------------------------
// The picture decoder behind every thumbnail, video poster and non-PNG still.
// On Linux and macOS that is ONE Qt implementation (platform/qt/QtFrameSource.cpp); until it existed
// those two OSes answered "Unsupported" and showed no picture at all. Pinned here: the output contract
// callers rely on — RGBA8, row 0 on top, never upscaled, an Error (not an empty "success") for a file
// that cannot be read. PNG and BMP only: both are compiled into QtGui, so the test needs no plugin folder.
// ---------------------------------------------------------------------------
#ifndef _WIN32
#include "modules/media/FrameSource.hpp"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

void TestQtFrameSourceStills() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    if (!dir.isValid()) return;

    // 8x4 with a red left half and a blue right half: orientation and channel order are both visible.
    QImage src(8, 4, QImage::Format_RGBA8888);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x)
            src.setPixelColor(x, y, x < 4 ? QColor(255, 0, 0, 255) : QColor(0, 0, 255, 255));
    const QString png = dir.filePath(QStringLiteral("still.png"));
    const QString bmp = dir.filePath(QStringLiteral("still.bmp"));
    CHECK(src.save(png, "PNG"));
    CHECK(src.save(bmp, "BMP"));

    auto source = bps::media::MakePlatformFrameSource();
    CHECK(source != nullptr);
    if (!source) return;

    for (const QString& file : {png, bmp}) {
        const auto r = source->Grab(file.toStdString(), bps::media::MediaKind::Image, 0.0, 1920);
        CHECK(r.ok());
        if (!r.ok()) continue;
        const auto& pic = r.value();
        CHECK(pic.width == 8 && pic.height == 4);          // wide target: never upscaled
        CHECK(pic.rgba.size() == 8u * 4u * 4u);
        if (pic.rgba.size() == 8u * 4u * 4u) {
            CHECK(pic.rgba[0] == 255 && pic.rgba[1] == 0 && pic.rgba[2] == 0 && pic.rgba[3] == 255);   // top-left red
            const size_t right = (7u) * 4u;                                                           // top-right blue
            CHECK(pic.rgba[right] == 0 && pic.rgba[right + 2] == 255);
        }
    }

    // A narrower target scales down, keeping the aspect ratio.
    const auto small = source->Grab(png.toStdString(), bps::media::MediaKind::Image, 0.0, 4);
    CHECK(small.ok());
    if (small.ok()) CHECK(small.value().width == 4 && small.value().height == 2);

    // Not a picture / not a file: an Error, never an empty success.
    const QString junk = dir.filePath(QStringLiteral("junk.png"));
    { QFile f(junk); CHECK(f.open(QIODevice::WriteOnly)); f.write("not an image"); }
    CHECK(!source->Grab(junk.toStdString(), bps::media::MediaKind::Image, 0.0, 480).ok());
    CHECK(!source->Grab(dir.filePath(QStringLiteral("absent.png")).toStdString(),
                        bps::media::MediaKind::Image, 0.0, 480).ok());
}
#endif

// ---------------------------------------------------------------------------
// Recording through Qt Multimedia (Linux / macOS): a real take, end to end through the container the
// recording engine uses. It needs Qt's media backend to encode, which needs a Qt application: run with
// BPS_TEST_GUI=1 (and QT_QPA_PLATFORM=offscreen on a machine without a display). Where the backend
// cannot encode (no application, no encoder) the test says so and asserts the matching capability
// answer instead of failing — the contract is "what is advertised is what works".
// ---------------------------------------------------------------------------
#include "modules/recording/QtRecordingContainer.hpp"
#include "platform/qt/QtRecording.hpp"

#include "QtTestSupport.hpp"

#include <QFileInfo>
#include <QTemporaryDir>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

static void QtRecordingContainerBody();
void TestQtRecordingContainer() { bpstest::RunWithEventLoop([] { QtRecordingContainerBody(); }); }
static void QtRecordingContainerBody() {
    namespace rec = bps::recording;
    const auto supported = rec::QtSupportedContainers();
    // The capability the UI reads must agree with what the container list says.
    if (supported.empty()) {
        std::fprintf(stderr, "[recording] Qt media backend cannot encode here (set BPS_TEST_GUI=1): skipped\n");
        CHECK(!bps::platform::QtVideoRecordingSupported());
        return;
    }
    const bool hasMp4 = std::find(supported.begin(), supported.end(), rec::ContainerKind::MP4) != supported.end();
    if (!hasMp4) {
        std::fprintf(stderr, "[recording] no MP4 encoder here: skipped\n");
        return;
    }

    QTemporaryDir dir;
    CHECK(dir.isValid());
    if (!dir.isValid()) return;
    const QString file = dir.filePath(QStringLiteral("take.mp4"));

    rec::RecordingProfile profile;
    profile.id = "qt-test";
    profile.container = rec::ContainerKind::MP4;
    profile.width = 320;
    profile.height = 240;
    profile.fps = 30.0;
    profile.videoBitrateKbps = 1000;
    profile.audioChannels = 2;

    auto container = rec::MakeQtRecordingContainer(rec::ContainerKind::MP4);
    CHECK(container != nullptr);
    if (!container) return;
    auto opened = container->Open(profile, file.toStdString());
    CHECK(opened.ok());
    if (!opened.ok()) {
        std::fprintf(stderr, "[recording] open failed: %s\n", opened.error().message.c_str());
        return;
    }

    // One second: 30 frames (a moving gradient, so the encoder has something to encode) and 48 kHz stereo audio.
    std::vector<uint32_t> pixels(320u * 240u);
    for (int f = 0; f < 30; ++f) {
        for (int y = 0; y < 240; ++y)
            for (int x = 0; x < 320; ++x)
                pixels[static_cast<size_t>(y) * 320u + static_cast<size_t>(x)] =
                    0xFF000000u | (static_cast<uint32_t>((x + f * 4) & 0xFF) << 16) | (static_cast<uint32_t>(y & 0xFF) << 8)
                    | static_cast<uint32_t>((f * 8) & 0xFF);
        rec::VideoFrameIn frame;
        frame.pixels = pixels.data();
        frame.width = 320;
        frame.height = 240;
        frame.timestampMs = f * (1000.0 / 30.0);
        CHECK(container->WriteVideo(frame).ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::vector<float> tone(48'000u * 2u);
    for (size_t i = 0; i < 48'000u; ++i) {
        const float s = 0.25f * std::sin(6.2831853f * 440.0f * static_cast<float>(i) / 48'000.0f);
        tone[i * 2] = s;
        tone[i * 2 + 1] = s;
    }
    rec::AudioBlockIn block;
    block.samples = tone.data();
    block.frames = 48'000u;
    block.channels = 2;
    block.rateHz = 48'000;
    block.timestampMs = 0.0;
    CHECK(container->WriteAudio(block).ok());

    const auto closed = container->Close(true);
    CHECK(closed.ok());
    if (!closed.ok())
        std::fprintf(stderr, "[recording] close failed: %s\n", closed.error().message.c_str());
    const QFileInfo info(file);
    std::fprintf(stderr, "[recording] wrote %s: %lld bytes\n", qPrintable(file), static_cast<long long>(info.size()));
    CHECK(info.exists());
    CHECK(info.size() > 1024);   // a real file with a header and a payload, not an empty container
}
