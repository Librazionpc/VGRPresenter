// QtFrameSource — the Linux/macOS picture decoder behind media::IFrameSource.
//
// WHAT NEEDS IT. Every still the engine paints that its own PNG codec cannot read (JPEG, WebP, GIF,
// BMP, TIFF…), every video poster frame, every Media-pane thumbnail and every audio file's cover art
// comes through IFrameSource::Grab. On Windows that is Media Foundation + WIC (WindowsFrameSource.cpp).
// Until this file existed the other two OSes answered "Unsupported" to all of it — a JPEG background or
// a video's first frame simply did not appear on the output, and the Media pane showed placeholder tiles.
//
// WHY Qt. Decoding a picture is an OS-generic service, so it belongs to Qt, written once:
//   * stills      QImageReader (decodes JPEG/PNG/WebP/GIF/BMP/TIFF/… with the plugins Qt ships, applies the
//                 EXIF rotation, and can downscale AT DECODE TIME — a 24 MP photo thumbnailed to 480 px never
//                 allocates the full-size bitmap for formats that support it)
//   * video       QMediaPlayer + a bare QVideoSink, seeked to `fraction` of the duration (the FFmpeg backend
//                 on Linux and macOS alike; the same decoder the live output uses)
//   * audio       the cover art Qt reads from the file's tags (QMediaMetaData)
//
// The contract is WindowsFrameSource's, so callers cannot tell the backends apart: RGBA8, row 0 on top,
// width = min(source, targetWidth) (never upscaled), an Err::Unsupported Error when nothing can be shown.
//
// THREADS. Grab() is called from worker threads (the thumbnail pool) and from the engine's loaders.
// QImageReader needs no event loop. A video/audio grab needs one for QMediaPlayer, so it runs on its OWN
// short-lived QThread with a local QEventLoop and the caller blocks on join — the same shape as a
// synchronous decode call on Windows. A hard timeout bounds it: a backend that never answers costs the
// caller kBudgetMs and an Error, not a hung worker.

#include "modules/media/FrameSource.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QImageReader>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QMetaObject>
#include <QSize>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <memory>

namespace bps::media {
namespace {

constexpr const char* kModule = "QtFrameSource";
constexpr int kPlayerTimeoutMs = 8000;      // inside the grab thread: give up on the backend
constexpr unsigned long kJoinBudgetMs = 12000;   // the caller's own ceiling (> the timeout above)

Picture ToPicture(QImage img, int targetWidth) {
    Picture out;
    if (img.isNull()) return out;
    const int limit = std::max(1, targetWidth);
    if (img.width() > limit)
        img = img.scaledToWidth(limit, Qt::SmoothTransformation);
    if (img.format() != QImage::Format_RGBA8888)
        img = img.convertToFormat(QImage::Format_RGBA8888);
    out.width = img.width();
    out.height = img.height();
    const size_t rowBytes = static_cast<size_t>(out.width) * 4;
    out.rgba.resize(rowBytes * static_cast<size_t>(out.height));
    for (int y = 0; y < out.height; ++y)   // bytesPerLine can be padded: copy row by row
        std::copy_n(img.constScanLine(y), rowBytes, out.rgba.data() + rowBytes * static_cast<size_t>(y));
    return out;
}

// ---------------------------------------------------------------------------------------------
// Stills
// ---------------------------------------------------------------------------------------------
Result<Picture> GrabImage(const QString& path, int targetWidth) {
    QImageReader reader(path);
    reader.setAutoTransform(true);   // honour the EXIF orientation: a phone photo is upright
    const int limit = std::max(1, targetWidth);
    const QSize native = reader.size();
    if (native.isValid() && native.width() > limit) {
        // Decoder-level downscale (JPEG does this in the IDCT) — far cheaper than decode-then-shrink.
        const int h = std::max(1, static_cast<int>(static_cast<long long>(native.height()) * limit / native.width()));
        reader.setScaledSize(QSize(limit, h));
    }
    QImage img = reader.read();
    if (img.isNull())
        return Error::Make(Err::Unsupported, kModule,
                           "Qt has no decoder for this image (" + reader.errorString().toStdString() + ")");
    Picture pic = ToPicture(std::move(img), targetWidth);
    if (pic.empty())
        return Error::Make(Err::Unsupported, kModule, "the image decoded to nothing");
    return pic;
}

// ---------------------------------------------------------------------------------------------
// Video poster frames and audio cover art — QMediaPlayer on a thread of its own
// ---------------------------------------------------------------------------------------------
struct GrabState {            // shared between the caller and the grab thread (it may outlive a timed-out caller)
    QString path;
    MediaKind kind = MediaKind::Video;
    double fraction = 0.0;
    QImage image;
    QString failure;
};

class GrabThread final : public QThread {
public:
    explicit GrabThread(std::shared_ptr<GrabState> state) : state_(std::move(state)) {}

protected:
    void run() override {
        GrabState& s = *state_;
        QMediaPlayer player;
        QVideoSink sink;
        if (s.kind == MediaKind::Video) player.setVideoSink(&sink);   // no audio output: the grab is silent

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
            s.failure = QStringLiteral("the media backend did not answer in time");
            loop.quit();
        });

        qint64 wantUs = -1;     // the position the frame must come from (-1 = the first frame is fine)
        bool started = false;

        QObject::connect(&player, &QMediaPlayer::errorOccurred, &loop,
                         [&](QMediaPlayer::Error, const QString& text) {
                             s.failure = text.isEmpty() ? QStringLiteral("the media backend cannot open this file") : text;
                             loop.quit();
                         });

        if (s.kind == MediaKind::Audio) {
            // Cover art arrives with the tags: once the file is loaded the metadata is complete.
            QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop, [&](QMediaPlayer::MediaStatus st) {
                if (st == QMediaPlayer::InvalidMedia) { loop.quit(); return; }
                if (st != QMediaPlayer::LoadedMedia && st != QMediaPlayer::BufferedMedia) return;
                const QMediaMetaData md = player.metaData();
                QImage art = md.value(QMediaMetaData::ThumbnailImage).value<QImage>();
                if (art.isNull()) art = md.value(QMediaMetaData::CoverArtImage).value<QImage>();
                s.image = art;
                if (art.isNull()) s.failure = QStringLiteral("this audio file has no cover art");
                loop.quit();
            });
        } else {
            QObject::connect(&sink, &QVideoSink::videoFrameChanged, &loop, [&](const QVideoFrame& f) {
                if (!f.isValid()) return;
                // After a seek the backend may still deliver one frame from the old position: skip
                // frames earlier than the wanted instant (an unstamped frame is accepted).
                if (wantUs > 0 && f.startTime() >= 0 && f.startTime() + 750000 < wantUs) return;
                const QImage img = f.toImage();
                if (img.isNull()) return;
                s.image = img;
                loop.quit();
            });
            QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &loop, [&](QMediaPlayer::MediaStatus st) {
                if (st == QMediaPlayer::InvalidMedia) {
                    s.failure = QStringLiteral("the media backend cannot decode this video");
                    loop.quit();
                    return;
                }
                if (started || (st != QMediaPlayer::LoadedMedia && st != QMediaPlayer::BufferedMedia)) return;
                started = true;
                const qint64 dur = player.duration();   // ms
                if (s.fraction > 0.0 && dur > 0) {
                    const qint64 ms = static_cast<qint64>(static_cast<double>(dur) * std::min(1.0, s.fraction));
                    wantUs = ms * 1000;
                    player.setPosition(ms);
                }
                player.play();   // a paused/stopped player may not deliver a frame; the first one ends the grab
            });
        }

        player.setSource(QUrl::fromLocalFile(s.path));
        timeout.start(kPlayerTimeoutMs);
        loop.exec();
        player.stop();
        player.setVideoSink(nullptr);
    }

private:
    std::shared_ptr<GrabState> state_;
};

Result<Picture> GrabViaPlayer(const QString& path, MediaKind kind, double fraction, int targetWidth) {
    if (!QCoreApplication::instance())   // QMediaPlayer needs an application object (the CLI/tools have none)
        return Error::Make(Err::Unsupported, kModule, "no Qt application is running to host the media decoder");
    auto state = std::make_shared<GrabState>();
    state->path = path;
    state->kind = kind;
    state->fraction = fraction;

    auto* worker = new GrabThread(state);
    worker->start();
    if (!worker->wait(kJoinBudgetMs)) {
        // Cannot kill a thread inside a backend; let it end by its own timeout and clean itself up.
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        return Error::Make(Err::Unsupported, kModule, "the media decoder did not finish in time");
    }
    delete worker;

    Picture pic = ToPicture(state->image, targetWidth);
    if (pic.empty())
        return Error::Make(Err::Unsupported, kModule,
                           state->failure.isEmpty() ? "no picture could be read from this file"
                                                    : state->failure.toStdString());
    return pic;
}

class QtFrameSource final : public IFrameSource {
public:
    Result<Picture> Grab(const std::string& path, MediaKind kind, double fraction, int targetWidth) override {
        const QString p = QString::fromStdString(path);
        switch (kind) {
        case MediaKind::Image: return GrabImage(p, targetWidth);
        case MediaKind::Video: return GrabViaPlayer(p, kind, fraction, targetWidth);
        case MediaKind::Audio: return GrabViaPlayer(p, kind, fraction, targetWidth);
        }
        return Error::Make(Err::Unsupported, kModule, "unknown media kind");
    }
};

} // namespace

std::shared_ptr<IFrameSource> MakeQtFrameSource() {
    return std::make_shared<QtFrameSource>();
}

} // namespace bps::media
