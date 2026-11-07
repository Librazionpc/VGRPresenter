#include "services/CallTrace.h"
#include "services/LiveOutputService.h"

#include "services/FontService.h"
#include "services/ShowConverter.h"
#include "services/SettingsService.h"
#include "services/StyleSpecBuilder.h"
#include "services/DesignLibraryService.h"
#include "services/MediaLibraryService.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/LineWalk.hpp"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/presentation/TemplateTextMetrics.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include "services/EngineBridge.h"
#include "services/EngineWorker.h"
#include "services/EventBus.h"
#include "models/OutputListModel.h"
#include "models/GroupListModel.h"
#include "models/BusListModel.h"
#include "models/VideoSourceListModel.h"

#include "modules/display/DisplayEngine.hpp"
#include "modules/display/Providers.hpp"
#include "modules/audio/AudioEngine.hpp"
#include "platform/PlatformAccessor.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"
#include "modules/production/ProductionEngine.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QThread>
#include <QAudioOutput>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QFont>
#include <QFontMetricsF>
#include <QTextLayout>
#include <QTextOption>
#include <QEasingCurve>
#include <QVariantAnimation>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <utility>

#include "modules/audio/AudioEngine.hpp"
#include "services/RoutingService.h"
#include "services/PressTimeline.h"
#include "services/MediaOnAirState.h"

namespace pl = bps::presentation;
namespace pr = bps::rendering;

using services::diagnostics::g_pressTimeline;
using services::media_on_air::g_mediaFitOnAir;
using services::media_on_air::g_mediaHasAlphaOnAir;

namespace services::media_on_air {
std::string g_mediaFitOnAir;
bool g_mediaHasAlphaOnAir = false;
} // namespace services::media_on_air

bool imageHasTransparentPixel(const QImage &image)
{
    if (image.isNull() || !image.hasAlphaChannel())
        return false;
    const QImage probe = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < probe.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(probe.constScanLine(y));
        for (int x = 0; x < probe.width(); ++x) {
            if (qAlpha(row[x]) < 255)
                return true;
        }
    }
    return false;
}

const QString kMediaMeterLabel = QStringLiteral("Media on air");

void pushMediaFrameToCompositor(const QImage &frame)
{
    const qint64 traceEnterMs = g_pressTimeline.armed() ? QDateTime::currentMSecsSinceEpoch() : 0;
    static bool hadFrame = false;
    if (frame.isNull()) {
        pl::CompositorState::Instance().ClearMedia();
        if (hadFrame) {
            hadFrame = false;
            qInfo("media hand-off: cleared (no frame) — engine media layer will be blank");
        }
        return;
    }
    const QImage rgba = frame.format() == QImage::Format_RGBA8888
        ? frame : frame.convertToFormat(QImage::Format_RGBA8888);
    pl::CompositorState::MediaFrame mf;
    mf.width = static_cast<uint32_t>(rgba.width());
    mf.height = static_cast<uint32_t>(rgba.height());
    mf.rgba.resize(static_cast<size_t>(mf.width) * mf.height * 4);
    for (int y = 0; y < rgba.height(); ++y) {
        std::memcpy(mf.rgba.data() + static_cast<size_t>(y) * mf.width * 4,
                    rgba.constScanLine(y), static_cast<size_t>(mf.width) * 4);
    }
    mf.fit = g_mediaFitOnAir;
    mf.hasAlpha = g_mediaHasAlphaOnAir;
    pl::CompositorState::Instance().SetMediaFrame(std::move(mf));
    if (!hadFrame) {
        hadFrame = true;
        qInfo("media hand-off: %dx%d (%zu bytes RGBA) sent to the engine",
              rgba.width(), rgba.height(), mf.rgba.size());
    }
    if (traceEnterMs != 0 && g_pressTimeline.armed()) {
        const double took = static_cast<double>(QDateTime::currentMSecsSinceEpoch() - traceEnterMs);
        ++g_pressTimeline.frames;
        g_pressTimeline.mediaSumMs += took;
        if (took > g_pressTimeline.mediaWorstMs)
            g_pressTimeline.mediaWorstMs = took;
        if (took >= 8.0)
            qInfo("PRESS TIMELINE: media frame %dx%d convert+copy took %.0f ms ON THE INTERFACE THREAD",
                  rgba.width(), rgba.height(), took);
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
LiveOutputService::MediaPreflight LiveOutputService::preflightMediaTake(const QString &path,
                                                                       const QString &name)
{
    MediaPreflight pre;
    pre.clean = path.startsWith(QStringLiteral("file:///"))
        ? QUrl(path).toLocalFile() : path;
    if (pre.clean.isEmpty() || !QFileInfo::exists(pre.clean))
        return pre;   // found = false: the apply half logs the refusal

    static const char *videoExts[] = { "mp4", "mov", "mkv", "avi", "webm", "m4v", "wmv" };
    static const char *imageExts[] = { "png", "jpg", "jpeg", "jpe", "jfif", "bmp", "dib", "gif",
                                       "webp", "tif", "tiff", "ico", "heic", "heif", "avif",
                                       "svg", "svgz", "jp2", "tga" };
    static const char *audioExts[] = { "mp3", "wav", "wave", "flac", "aac", "m4a", "m4b", "ogg", "oga",
                                       "opus", "wma", "aif", "aiff", "aifc", "mka", "amr", "ac3", "weba" };
    const QString ext = QFileInfo(pre.clean).suffix().toLower();
    for (const char *e : videoExts)
        if (ext == QLatin1String(e)) { pre.isVideo = true; break; }
    for (const char *e : imageExts)
        if (ext == QLatin1String(e)) { pre.isImage = true; break; }
    for (const char *e : audioExts)
        if (ext == QLatin1String(e)) { pre.isAudio = true; break; }
    pre.recognized = pre.isVideo || pre.isImage || pre.isAudio;
    pre.found = true;
    if (!pre.recognized)
        return pre;

    pre.name = name.isEmpty() ? QFileInfo(pre.clean).completeBaseName() : name;
    if (pre.isImage) {
        QImageReader reader(pre.clean);
        reader.setAutoTransform(true);
        const QByteArray format = reader.format();
        if (format == "svg" || format == "svgz") {
            const QSize intrinsic = reader.size();
            if (intrinsic.isValid() && intrinsic.height() > 0 && intrinsic.height() < 1080) {
                const qreal scale = 1080.0 / intrinsic.height();
                reader.setScaledSize(QSize(qRound(intrinsic.width() * scale), 1080));
            }
        }
        pre.image = reader.read();
        pre.hasAlpha = imageHasTransparentPixel(pre.image);
        if (!pre.image.isNull()) {
            const QImage probe = pre.image.convertToFormat(QImage::Format_RGB32);
            const int cx = probe.width() / 2;
            const int cy = probe.height() / 2;
            qInfo("media take image: '%s' %dx%d corners tl=%08x br=%08x center=%08x alpha=%s",
                  qUtf8Printable(pre.clean), probe.width(), probe.height(),
                  probe.pixel(0, 0), probe.pixel(probe.width() - 1, probe.height() - 1),
                  probe.pixel(cx, cy), pre.hasAlpha ? "yes" : "no");
        } else {
            static const QString supported = [] {
                QStringList names;
                for (const QByteArray &f : QImageReader::supportedImageFormats())
                    names << QString::fromLatin1(f).toLower();
                names.removeDuplicates();
                names.sort();
                return names.join(QStringLiteral(", "));
            }();
            qWarning("media take image: '%s' failed to decode (format '%s'; this build decodes: %s)",
                     qUtf8Printable(pre.clean), format.isEmpty() ? "unknown" : format.constData(),
                     qUtf8Printable(supported));
        }
    }
    return pre;
}

void LiveOutputService::takeMediaAtLevel(const QString &path, const QString &name, qreal levelPercent,
                                         bool muted)
{
    pendingMediaLevel_ = qBound<qreal>(0.0, levelPercent / 100.0, 1.0);
    pendingMediaMuted_ = muted;
    pendingFromSource_ = true;
    takeMedia(path, name);
}

void LiveOutputService::applyMediaAudio()
{
    if (!mediaAudio_)
        return;
    mediaAudio_->setVolume(static_cast<float>(mediaLevel_));
    mediaAudio_->setMuted(mediaMuted_ || !speakersAudible_);
    qInfo("media audio: level %.0f%%, %s (transport mute %s, Speakers output %s)", mediaLevel_ * 100.0,
          (mediaMuted_ || !speakersAudible_) ? "SILENT" : "audible", mediaMuted_ ? "on" : "off",
          speakersAudible_ ? "on" : "off");
}

void LiveOutputService::refreshSpeakersAudible()
{
    // The Routing screen's own state once it has synced; before that, what the audio engine itself
    // loaded (the same answer that decides whether the playback device is opened at all).
    RoutingService *routing = RoutingService::instance();
    speakersAudible_ = (routing != nullptr && routing->speakersKnown())
        ? routing->speakersEnabled()
        : bps::audio::AudioEngine::Instance().SpeakersAudible();
}

void LiveOutputService::setMediaLevel(qreal levelPercent, bool muted)
{
    mediaLevel_ = qBound<qreal>(0.0, levelPercent / 100.0, 1.0);
    mediaMuted_ = muted;
    applyMediaAudio();
    emit mediaChanged();
}

void LiveOutputService::takeMedia(const QString &path, const QString &name)
{
    const quint64 token = ++mediaTakeToken_;
    vgr::EngineWorker::instance().runOffThread([this, token, path, name] {
        MediaPreflight pre = preflightMediaTake(path, name);
        if (token != mediaTakeToken_.load())
            return;   // superseded while the disk was being read — drop it here
        vgr::EngineWorker::onInterfaceThread(this, [this, token, pre] {
            if (token != mediaTakeToken_.load())
                return;   // a newer take (or a clear) landed while this was queued
            applyMediaTake(pre);
        });
    });
}

void LiveOutputService::applyMediaTake(const MediaPreflight &pre)
{
    const QString &clean = pre.clean;
    if (!pre.found) {
        qWarning("LiveOutputService: takeMedia: file not found: %s", qUtf8Printable(clean));
        emit mediaChanged();
        return;
    }
    if (!pre.recognized) {
        qWarning("LiveOutputService: takeMedia: '%s' has an unrecognized extension",
                 qUtf8Printable(clean));
        emit mediaChanged();
        return;
    }
    if (!inputLabel_.isEmpty())
        clearInput();

    EnsureMediaPlayer();
    ArmMediaTake(pre);
    StartMediaPlayback(pre);

    EngineBridge::instance().stopStreamMeter(kMediaMeterLabel);
    if (pre.isVideo || pre.isAudio)
        EngineBridge::instance().startStreamMeter(kMediaMeterLabel, QStringLiteral("media"), pre.clean);
    qInfo("LiveOutputService: media on air: '%s' (%s)", qUtf8Printable(mediaName_),
          pre.isVideo ? "video" : (pre.isAudio ? "audio" : "image"));
    emit mediaChanged();
    emit mediaTick();
}

double LiveOutputService::mediaSourceFps() const
{
    if (!mediaPlayer_)
        return 0.0;
    return mediaPlayer_->metaData().value(QMediaMetaData::VideoFrameRate).toDouble();
}

void LiveOutputService::shutdownMedia()
{
    // Every Media Foundation worker thread behind a QMediaPlayer runs Qt code (frame and state
    // callbacks) and so owns Qt thread data. The crash reports for a quit ("Qt6Core.dll,
    // c0000005", always the same offset: Qt's per-thread cleanup reading thread data that was
    // already gone) are those threads ending AFTER the app's teardown had removed what they
    // clean up against. Stopping and deleting the player here, in order, ends them first.
    if (!mediaPlayer_)
        return;
    mediaPlayer_->stop();
    mediaPlayer_->setSource(QUrl());
    mediaPlayer_->setVideoOutput(nullptr);
    mediaPlayer_->setAudioOutput(nullptr);
    delete mediaPlayer_;
    mediaPlayer_ = nullptr;
    delete mediaAudio_;
    mediaAudio_ = nullptr;
    delete mediaSink_;
    mediaSink_ = nullptr;
    mediaOnAir_ = false;
}

void LiveOutputService::EnsureMediaPlayer()
{
    if (!mediaPlayer_) {
        mediaSink_ = new QVideoSink(this);
        mediaAudio_ = new QAudioOutput(this);
        mediaPlayer_ = new QMediaPlayer(this);
        mediaPlayer_->setAudioOutput(mediaAudio_);
        mediaPlayer_->setVideoOutput(mediaSink_);
        // Switching the Speakers output on or off on the Routing screen changes what is heard at once.
        if (RoutingService *routing = RoutingService::instance())
            connect(routing, &RoutingService::routingChanged, this, [this] {
                refreshSpeakersAudible();
                applyMediaAudio();
            });
        connect(mediaPlayer_, &QMediaPlayer::positionChanged, this, [this](qlonglong p) {
            mediaPosition_ = p;
            emit mediaTick();          // transport readout AND the tiles' rev bump
        });
        connect(mediaPlayer_, &QMediaPlayer::durationChanged, this, [this](qlonglong d) {
            mediaDuration_ = d;
            emit mediaChanged();
        });
        connect(mediaPlayer_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState s) {
            mediaState_ = s == QMediaPlayer::PlayingState ? QStringLiteral("playing")
                        : s == QMediaPlayer::PausedState  ? QStringLiteral("paused")
                                                          : QStringLiteral("stopped");
            emit mediaChanged();
        });
        connect(mediaPlayer_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &msg) {
            qWarning("LiveOutputService: media player error: %s", qUtf8Printable(msg));
            clearMedia();
        });
        connect(mediaPlayer_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus st) {
            if (st == QMediaPlayer::EndOfMedia) {
                mediaPlayer_->pause();
                mediaPlayer_->setPosition(0);
                mediaState_ = QStringLiteral("paused");
                emit mediaChanged();
            }
        });
        // DIRECT: this runs on the thread that DECODED the frame, not the interface thread. A frame used
        // to be converted to an image, copied into the compositor (two more 8 MB passes) and announced
        // to every QML listener, all on the interface thread — at 30-60 frames a second, a window that
        // was also drawing the UI. The decoder thread now does the conversion and hands the engine every
        // frame; the interface thread is told once per batch (coalesced), and only gets the newest.
        connect(mediaSink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &f) {
            if (!f.isValid())
                return;
            const QImage image = f.toImage();
            if (image.isNull())
                return;
            mediaFramesDecoded_.fetch_add(1, std::memory_order_relaxed);
            mediaFrameShared_.store(std::make_shared<const QImage>(image));   // what the providers read
            pushMediaFrameToCompositor(image);                                 // the engine: every frame
            if (!mediaGuiUpdatePending_.exchange(true)) {
                QMetaObject::invokeMethod(this, [this] {
                    mediaGuiUpdatePending_.store(false);
                    const std::shared_ptr<const QImage> newest = mediaFrameShared_.load();
                    if (!newest || newest->isNull())
                        return;
                    lastMediaFrame_ = *newest;
                    if (!mediaAlphaDecided_) {
                        mediaAlphaDecided_ = true;
                        mediaHasAlpha_ = imageHasTransparentPixel(lastMediaFrame_);
                        g_mediaHasAlphaOnAir = mediaHasAlpha_;
                        emit mediaChanged();
                    }
                    mediaRev_++;
                    emit mediaTick();
                }, Qt::QueuedConnection);
            }
        }, Qt::DirectConnection);
    }
}

void LiveOutputService::ArmMediaTake(const MediaPreflight &pre)
{
    mediaPath_ = pre.clean;
    mediaName_ = pre.name;
    // A file taken as a SOURCE plays at that source's level and mute; any other take plays at full level
    // (and keeps the transport's own mute, as before).
    if (pendingFromSource_) {
        mediaLevel_ = pendingMediaLevel_;
        mediaMuted_ = pendingMediaMuted_;
    } else {
        mediaLevel_ = 1.0;
    }
    pendingFromSource_ = false;
    const QString fileFit = MediaLibraryService::instance().itemFit(pre.clean);
    mediaFitUsesDefault_ = fileFit.isEmpty();
    mediaFit_ = mediaFitUsesDefault_ ? defaultMediaFit_ : fileFit;
    g_mediaFitOnAir = mediaFit_.toStdString();
    mediaIsVideo_ = pre.isVideo;
    mediaIsAudio_ = pre.isAudio;
    mediaHasAlpha_ = !pre.isVideo && !pre.isAudio && pre.hasAlpha;
    g_mediaHasAlphaOnAir = mediaHasAlpha_;
    mediaAlphaDecided_ = !pre.isVideo;
    setLastMediaFrame(QImage());
    pushMediaFrameToCompositor(lastMediaFrame_);
    mediaOnAir_ = true;
}

void LiveOutputService::StartMediaPlayback(const MediaPreflight &pre)
{
    if (pre.isVideo || pre.isAudio) {
        mediaPlayer_->setSource(QUrl::fromLocalFile(pre.clean));
        refreshSpeakersAudible();
        applyMediaAudio();
        mediaState_ = QStringLiteral("playing");
        mediaPlayer_->play();
        return;
    }
    setLastMediaFrame(pre.image);
    if (!lastMediaFrame_.isNull())
        mediaRev_++;
    pushMediaFrameToCompositor(lastMediaFrame_);
    mediaState_ = QStringLiteral("playing");
    mediaPosition_ = 0;
    mediaDuration_ = 0;
}

void LiveOutputService::setMediaFit(const QString &fit)
{
    const QString wanted = (fit == QLatin1String("contain") || fit == QLatin1String("cover")
                            || fit == QLatin1String("fill"))
        ? fit : QString();
    if (mediaPath_.isEmpty())
        return;   // nothing on air to set it on (the card picker writes the file itself)
    MediaLibraryService::instance().setItemFit(mediaPath_, wanted);
    mediaFitUsesDefault_ = wanted.isEmpty();
    const QString effective = mediaFitUsesDefault_ ? defaultMediaFit_ : wanted;
    if (mediaFit_ == effective)
        return;
    mediaFit_ = effective;
    g_mediaFitOnAir = effective.toStdString();
    pl::CompositorState::Instance().SetMediaFit(effective.toStdString());
    emit mediaChanged();
}

void LiveOutputService::setDefaultMediaFit(const QString &fit)
{
    const QString wanted = (fit == QLatin1String("contain") || fit == QLatin1String("cover")
                            || fit == QLatin1String("fill"))
        ? fit : QStringLiteral("contain");
    if (defaultMediaFit_ == wanted)
        return;
    defaultMediaFit_ = wanted;
    if (mediaOnAir_ && mediaFitUsesDefault_) {
        mediaFit_ = wanted;
        g_mediaFitOnAir = wanted.toStdString();
        pl::CompositorState::Instance().SetMediaFit(wanted.toStdString());
    }
    emit mediaChanged();
}

QString LiveOutputService::mediaMeterLabel() const
{
    return kMediaMeterLabel;
}

void LiveOutputService::clearMedia()
{
    ++mediaTakeToken_;
    if (!mediaOnAir_)
        return;
    if (mediaPlayer_)
        mediaPlayer_->stop();
    mediaOnAir_ = false;
    mediaPath_.clear();
    mediaName_.clear();
    mediaFit_.clear();
    mediaFitUsesDefault_ = true;
    g_mediaFitOnAir.clear();
    mediaHasAlpha_ = false;
    mediaAlphaDecided_ = false;
    g_mediaHasAlphaOnAir = false;
    mediaState_ = QStringLiteral("stopped");
    mediaPosition_ = 0;
    mediaDuration_ = 0;
    setLastMediaFrame(QImage());
    pl::CompositorState::Instance().ClearMedia();
    EngineBridge::instance().stopStreamMeter(kMediaMeterLabel);
    qInfo("LiveOutputService: media taken off air");
    emit mediaChanged();
    emit mediaTick();
}

void LiveOutputService::mediaTogglePlay()
{
    if (!mediaOnAir_ || (!mediaIsVideo_ && !mediaIsAudio_) || !mediaPlayer_)
        return;
    if (mediaPlayer_->playbackState() == QMediaPlayer::PlayingState)
        mediaPlayer_->pause();
    else
        mediaPlayer_->play();
}

void LiveOutputService::mediaSeek(qreal fraction)
{
    if (!mediaOnAir_ || (!mediaIsVideo_ && !mediaIsAudio_) || !mediaPlayer_ || mediaDuration_ <= 0)
        return;
    mediaPlayer_->setPosition(qlonglong(qBound(0.0, fraction, 1.0) * mediaDuration_));
}

void LiveOutputService::mediaToggleMuted()
{
    mediaMuted_ = !mediaMuted_;
    applyMediaAudio();
    emit mediaChanged();
}

QString LiveOutputService::formatMediaTime(qlonglong ms) const
{
    if (ms < 0)
        ms = 0;
    const int totalSec = int(ms / 1000);
    return QStringLiteral("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QLatin1Char('0'));
}

QImage LiveMediaFrameProvider::requestImage(const QString &id, QSize *size,
                                            const QSize &requestedSize)
{
    Q_UNUSED(id)
    Q_UNUSED(requestedSize)
    // Read the PUBLISHED frame (this may run on the image loader thread): the interface thread's own
    // member is replaced under us.
    const std::shared_ptr<const QImage> published = LiveOutputService::instance().mediaFrameShared();
    static const QImage none;
    const QImage &frame = published ? *published : none;
    if (frame.isNull()) {
        static const QImage empty = [] {
            QImage e(1, 1, QImage::Format_ARGB32);
            e.fill(Qt::transparent);
            return e;
        }();
        if (size)
            *size = QSize(1, 1);
        return empty;
    }
    // A consumer that asks for a size (a small monitor tile) gets the picture shrunk to COVER that size,
    // keeping the file's own aspect: the Image's fill mode then fits, crops or stretches it as it should.
    // Never enlarged — a full 1080p upload per tile per frame was what made the monitor stutter.
    if (requestedSize.isValid() && !requestedSize.isEmpty()) {
        const QSize covering = frame.size().scaled(requestedSize, Qt::KeepAspectRatioByExpanding);
        if (covering.width() < frame.width() && covering.height() < frame.height()) {
            const QImage small = shrinkForPreview(frame, requestedSize, Qt::KeepAspectRatioByExpanding);
            {   // TEMP-DIAG
                static int n = 0;
                if (n++ % 90 == 0)
                    qInfo("mediaplay provider: frame %dx%d fmt=%d req=%dx%d -> %dx%d fmt=%d px=%08x", frame.width(),
                          frame.height(), int(frame.format()), requestedSize.width(), requestedSize.height(),
                          small.width(), small.height(), int(small.format()),
                          small.isNull() ? 0u : small.pixel(small.width() / 2, small.height() / 2));
            }
            if (size)
                *size = small.size();
            return small;
        }
    }
    if (size)
        *size = frame.size();
    return frame;
}