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

#include "services/SlidePreviewMap.h"

namespace pl = bps::presentation;
namespace pr = bps::rendering;

using services::preview::slideToVariantMap;

// One tap, whichever family it comes from; `role` is the consumer the engine keys
// the tap by ("output" / "card" / "bus").
namespace {

bool StartTap(const QString &kind, const QString &label, const QString &mode, const QString &role)
{
    if (kind == QLatin1String("screen"))
        return EngineBridge::instance().startScreenPreview(label, role);
    if (kind == QLatin1String("ndi"))
        return EngineBridge::instance().startNdiPreview(label, role);
    return EngineBridge::instance().startVideoPreview(label, mode, role);
}

// A camera's refusal is reported through its empty frames, not the return value, so
// the taken input and the card previews keep the tap either way.
bool StartTapKeepingCamera(const QString &kind, const QString &label, const QString &mode,
                           const QString &role)
{
    const bool accepted = StartTap(kind, label, mode, role);
    return kind == QLatin1String("camera") ? true : accepted;
}

void StopTap(const QString &kind, const QString &label, const QString &role)
{
    if (kind == QLatin1String("screen"))
        EngineBridge::instance().stopScreenPreview(label, role);
    else if (kind == QLatin1String("ndi"))
        EngineBridge::instance().stopNdiPreview(label, role);
    else
        EngineBridge::instance().stopVideoPreview(label, role);
}

} // namespace

bool LiveOutputService::InputKindHasTap(const QString &kind)
{
    return kind == QLatin1String("camera") || kind == QLatin1String("screen")
           || kind == QLatin1String("ndi");
}

void LiveOutputService::StopPreviousInputTap()
{
    if (inputLabel_.isEmpty())
        return;
    StopTap(inputKind_, inputLabel_, QStringLiteral("output"));
}

bool LiveOutputService::StartInputTap(const QString &kind, const QString &label, const QString &mode)
{
    return StartTapKeepingCamera(kind, label, mode, QStringLiteral("output"));
}

void LiveOutputService::EnsureInputPump()
{
    if (!inputPump_) {
        inputPump_ = new QTimer(this);
        inputPump_->setTimerType(Qt::PreciseTimer);
        inputPump_->setInterval(16);
        connect(inputPump_, &QTimer::timeout, this, &LiveOutputService::pumpTick);
    }
    if (!inputPump_->isActive())
        inputPump_->start();
}

void LiveOutputService::takeInput(const QString &label, const QString &kind, const QString &mode)
{
    if (!InputKindHasTap(kind)) {
        qWarning("LiveOutputService: input kind '%s' has no output-preview tap", kind.toUtf8().constData());
        emit inputChanged();
        return;
    }
    if (label.isEmpty())
        return;

    clearMedia();

    if (inputLabel_ != label || inputKind_ != kind)
        StopPreviousInputTap();

    inputLabel_ = label;
    inputKind_ = kind;
    inputMode_ = mode;
    if (!StartInputTap(kind, label, mode)) {
        inputLabel_.clear();
        inputKind_.clear();
        emit inputChanged();
        return;
    }

    EnsureInputPump();
    const QString resolvedId = EngineBridge::instance().resolvedPreviewDeviceId(label);
    pl::CompositorState::Instance().SetTakenInput(resolvedId.toStdString(), kind.toStdString());
    emit inputChanged();
}

void LiveOutputService::pushNdiInputFrame(const QImage &frame)
{
    if (inputKind_ != QLatin1String("ndi") || inputLabel_.isEmpty())
        return;
    if (frame.isNull() || frame.width() <= 1)
        return;   // no receiver or no frame yet — keep the previous compositor state
    auto &compositor = pl::CompositorState::Instance();
    pl::CompositorState::MediaFrame mf;
    mf.width = uint32_t(frame.width());
    mf.height = uint32_t(frame.height());
    mf.rgba.assign(frame.constBits(), frame.constBits() + size_t(frame.sizeInBytes()));
    compositor.SetMediaFrame(std::move(mf));
    ndiInputLive_ = true;
}

void LiveOutputService::clearNdiInputFrame()
{
    if (!ndiInputLive_)
        return;
    pl::CompositorState::Instance().ClearMedia();
    ndiInputLive_ = false;
}

// ---------------------------------------------------------------------------
static pl::CompositorState::MediaFrame rgbaFrameOf(const QImage &frame);

QImage LiveOutputService::SampleTakenInput()
{
    if (inputLabel_.isEmpty())
        return {};
    const QImage frame = inputFrame(inputLabel_, inputKind_);
    if (!inputLive_ && !frame.isNull() && frame.width() > 1) {
        inputLive_ = true;
        qInfo("LiveOutputService: first frame decoded for '%s' (%dx%d)",
              qUtf8Printable(inputLabel_), frame.width(), frame.height());
    }
    return frame;
}

bool LiveOutputService::SampleCardPreviews()
{
    bool cardMoved = false;
    for (auto it = cardPreviews_.cbegin(); it != cardPreviews_.cend(); ++it) {
        const QString &held = it.key();
        const QImage frame = EngineBridge::instance().previewFrameFor(held);
        if (frame.isNull() || frame.width() <= 1)
            continue;
        producing_.insert(held);
        const qint64 key = frame.cacheKey();
        if (cardFrameKeys_.value(held, 0) != key) {
            cardFrameKeys_.insert(held, key);
            cardMoved = true;
        }
    }
    return cardMoved;
}

bool LiveOutputService::PushTakenInputFrame(const QImage &frame)
{
    if (frame.isNull())
        return false;
    const qint64 key = frame.cacheKey();
    if (key == inputFrameKey_)
        return false;
    inputFrameKey_ = key;
    pushNdiInputFrame(frame);
    return true;
}

bool LiveOutputService::PushBusFeedFrames()
{
    bool moved = false;
    for (auto it = busFeeds_.begin(); it != busFeeds_.end(); ++it) {
        const QImage frame = inputFrame(it->label, it->kind);
        if (frame.isNull() || frame.width() <= 1)
            continue;   // warming up or not delivering: keep the last honest picture
        const qint64 key = frame.cacheKey();
        if (key == it->lastFrameKey)
            continue;   // the tap has not produced since the last tick: no copy
        it->lastFrameKey = key;
        pl::CompositorState::Instance().SetBusFrame(it.key(), rgbaFrameOf(frame));
        moved = true;
    }
    return moved;
}

void LiveOutputService::pumpTick()
{
    // A taken CAMERA or SCREEN tap keeps its pixels in the engine, which composes the program from
    // them itself: all this tick needs from it is "did a frame arrive". The tap's counter answers that;
    // the old way copied the whole frame (8 MB at 1080p) into a QImage on the interface thread every
    // time one landed, just to compare its cache key. An NDI input, or a tap with no counter, still
    // takes the copy.
    QImage takenFrame;
    bool movedByCounter = false;
    bool countedKind = false;
    if (!inputLabel_.isEmpty()
        && (inputKind_ == QLatin1String("camera") || inputKind_ == QLatin1String("screen"))) {
        const quint64 seq = EngineBridge::instance().previewFrameSeqFor(inputLabel_);
        if (seq != 0) {
            countedKind = true;
            if (seq != inputFrameSeq_) {
                inputFrameSeq_ = seq;
                movedByCounter = true;
            }
            if (!inputLive_) {
                inputLive_ = true;
                qInfo("LiveOutputService: first frame decoded for '%s'", qUtf8Printable(inputLabel_));
            }
            producing_.insert(inputLabel_);
        }
    }
    if (!countedKind)
        takenFrame = SampleTakenInput();
    const bool cardMoved = SampleCardPreviews();
    if (!inputLabel_.isEmpty() && !producing_.contains(inputLabel_)
        && !takenFrame.isNull() && takenFrame.width() > 1)
        producing_.insert(inputLabel_);
    bool moved = countedKind ? movedByCounter : PushTakenInputFrame(takenFrame);
    if (PushBusFeedFrames())
        moved = true;
    if (cardMoved)
        moved = true;
    if (moved) {
        inputRev_++;
        emit inputChanged();
    }
    if (inputLabel_.isEmpty() && cardPreviews_.isEmpty() && busFeeds_.isEmpty())
        inputPump_->stop();
}

bool LiveOutputService::inputProducing(const QString &label) const
{
    return producing_.contains(label);
}

QImage LiveOutputService::inputFrame(const QString &label, const QString &kind) const
{
    if (kind == QLatin1String("ndi"))
        return EngineBridge::instance().previewNdiFrameFor(label);
    return EngineBridge::instance().previewPixelsFor(label);
}

void LiveOutputService::clearNdiInput()
{
    if (inputKind_ != QLatin1String("ndi") || inputLabel_.isEmpty())
        return;
    clearInput();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void LiveOutputService::previewInput(const QString &label, const QString &kind, const QString &mode)
{
    if (label.isEmpty() || (kind != QLatin1String("camera") && kind != QLatin1String("screen")
                            && kind != QLatin1String("ndi")))
        return;
    if (!EngineBridge::instance().inputSourceReachable(label, kind)) {
        qInfo("LiveOutputService: '%s' is not reachable right now (window closed / device gone)",
              qUtf8Printable(label));
        emit inputChanged();
        return;
    }
    if (!StartTapKeepingCamera(kind, label, mode, QStringLiteral("card"))) {
        emit inputChanged();
        return;
    }
    cardPreviews_[label] = QVariantMap{
        { QStringLiteral("kind"), kind },
        { QStringLiteral("mode"), mode },
    };
    EnsureInputPump();
    emit inputChanged();
}

void LiveOutputService::endPreviewInput(const QString &label)
{
    if (!cardPreviews_.contains(label))
        return;
    const QVariantMap entry = cardPreviews_.take(label).toMap();
    StopTap(entry.value("kind").toString(), label, QStringLiteral("card"));
    producing_.remove(label);
    cardFrameKeys_.remove(label);   // the next hold starts from "no picture yet"
    emit inputChanged();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
static pl::CompositorState::MediaFrame rgbaFrameOf(const QImage &frame)
{
    const QImage rgba = frame.format() == QImage::Format_RGBA8888
        ? frame : frame.convertToFormat(QImage::Format_RGBA8888);
    pl::CompositorState::MediaFrame mf;
    if (rgba.isNull())
        return mf;
    mf.width = static_cast<uint32_t>(rgba.width());
    mf.height = static_cast<uint32_t>(rgba.height());
    mf.rgba.resize(static_cast<size_t>(mf.width) * mf.height * 4);
    for (int y = 0; y < rgba.height(); ++y)
        std::memcpy(mf.rgba.data() + static_cast<size_t>(y) * mf.width * 4,
                    rgba.constScanLine(y), static_cast<size_t>(mf.width) * 4);
    return mf;
}

void LiveOutputService::refreshBusFeeds()
{
    QHash<int, BusFeed> wanted;
    QList<int> namedBuses;
    CollectBusFeedWants(wanted, namedBuses);
    for (int busId : std::as_const(namedBuses))
        AddTemplateWindowsFor(busId, wanted);

    ReleaseStaleBusFeeds(wanted);
    const bool claimed = ClaimWantedBusFeeds(wanted);
    publishBusFeeds();
    EnsureBusFeedPump(claimed);
}

bool LiveOutputService::FeedForSourceRow(int sourceRow, BusFeed &feed) const
{
    if (sourceRow < 0)
        return false;
    const QVariantMap source = VideoSourceListModel::Instance()->getSource(sourceRow);
    const QString label = source.value(QStringLiteral("sublabel")).toString().trimmed();
    if (label.isEmpty())
        return false;
    feed.label = label;
    feed.kind = source.value(QStringLiteral("kind")).toString();
    feed.mode = source.value(QStringLiteral("mode")).toString();
    feed.deviceId = EngineBridge::instance().resolvedPreviewDeviceId(label);
    return true;
}

bool LiveOutputService::FeedForBusRoute(int busId, BusFeed &feed) const
{
    const int row = BusListModel::rowForBusNumber(busId);
    if (row < 0)
        return false;
    const QVariantMap bus = BusListModel::Instance()->getBus(row);
    if (bus.value(QStringLiteral("type")).toString() == QLatin1String("template"))
        return false;
    const QVariantList routes = bus.value(QStringLiteral("routedVideoSources")).toList();
    if (routes.isEmpty())
        return false;
    return FeedForSourceRow(routes.first().toInt(), feed);
}

void LiveOutputService::AddWantedBusFeed(int busId, QList<int> &namedBuses, QHash<int, BusFeed> &wanted)
{
    if (busId > 0 && !namedBuses.contains(busId))
        namedBuses.append(busId);
    if (busId <= 0 || wanted.contains(busId))
        return;
    BusFeed feed;
    if (FeedForBusRoute(busId, feed))
        wanted.insert(busId, feed);
}

void LiveOutputService::CollectBusFeedWants(QHash<int, BusFeed> &wanted, QList<int> &namedBuses)
{
    // Only feeds explicitly used by the on-air slide are part of the program.
    // Output routing must not start a bus feed just because GO LIVE was pressed.
    const QVariantList blocks = onAirSlide_.value(QStringLiteral("blocks")).toList();
    for (const QVariant &v : blocks) {
        const QVariantMap block = v.toMap();
        if (block.value(QStringLiteral("kind")).toString() != QLatin1String("bus"))
            continue;
        AddWantedBusFeed(block.value(QStringLiteral("meta")).toMap()
                             .value(QStringLiteral("busId")).toInt(),
                         namedBuses, wanted);
    }
}

void LiveOutputService::AddTemplateWindowsFor(int busId, QHash<int, BusFeed> &wanted)
{
    if (busId <= 0)
        return;
    const int row = BusListModel::rowForBusNumber(busId);
    if (row < 0)
        return;
    const QVariantMap bus = BusListModel::Instance()->getBus(row);
    if (bus.value(QStringLiteral("type")).toString() != QLatin1String("template"))
        return;
    const QVariantList routes = bus.value(QStringLiteral("routedVideoSources")).toList();
    for (int i = 0; i < routes.size(); ++i) {
        const int planeKey = pl::CompositorState::TemplateWindowPlaneKey(busId, i);
        if (planeKey <= 0 || wanted.contains(planeKey))
            continue;
        BusFeed feed;
        if (FeedForSourceRow(routes.at(i).toInt(), feed))
            wanted.insert(planeKey, feed);
    }
}

void LiveOutputService::ReleaseStaleBusFeeds(const QHash<int, BusFeed> &wanted)
{
    const QList<int> held = busFeeds_.keys();
    for (int busId : held) {
        const BusFeed &current = busFeeds_[busId];
        const auto want = wanted.constFind(busId);
        if (want == wanted.constEnd() || want->label != current.label
            || want->kind != current.kind)
            releaseBusFeed(busId);
    }
}

bool LiveOutputService::ClaimWantedBusFeeds(const QHash<int, BusFeed> &wanted)
{
    constexpr qint64 kBusTapRetryMs = 3000;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool claimed = false;
    for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
        if (busFeeds_.contains(it.key()))
            continue;
        const BusFeed &feed = it.value();
        const QString retryKey = feed.kind + QLatin1Char('|') + feed.label;
        if (busTapRetry_.value(retryKey, 0) > nowMs)
            continue;
        if (!StartTap(feed.kind, feed.label, feed.mode, QStringLiteral("bus"))) {
            busTapRetry_.insert(retryKey, nowMs + kBusTapRetryMs);
            qInfo("LiveOutputService: bus %d feed '%s' could not be taken for the output "
                  "(retrying in %lld s)",
                  it.key(), qUtf8Printable(feed.label), static_cast<long long>(kBusTapRetryMs / 1000));
            continue;
        }
        busTapRetry_.remove(retryKey);
        busFeeds_.insert(it.key(), feed);
        claimed = true;
    }
    return claimed;
}

void LiveOutputService::EnsureBusFeedPump(bool claimed)
{
    if (busFeeds_.isEmpty())
        return;
    EnsureInputPump();
    if (claimed)
        emit inputChanged();
}

void LiveOutputService::releaseBusFeed(int busId)
{
    const auto it = busFeeds_.constFind(busId);
    if (it == busFeeds_.constEnd())
        return;
    const BusFeed feed = it.value();
    busFeeds_.remove(busId);
    StopTap(feed.kind, feed.label, QStringLiteral("bus"));
    pl::CompositorState::Instance().SetBusFrame(busId, pl::CompositorState::MediaFrame{});
    publishBusFeeds();
}

void LiveOutputService::publishBusFeeds()
{
    std::vector<pl::CompositorState::BusFeed> feeds;
    feeds.reserve(static_cast<size_t>(busFeeds_.size()));
    for (auto it = busFeeds_.cbegin(); it != busFeeds_.cend(); ++it)
        feeds.push_back(pl::CompositorState::BusFeed{
            it.key(), it->deviceId.toStdString(), it->kind.toStdString() });
    pl::CompositorState::Instance().SetBusFeeds(std::move(feeds));
}

void LiveOutputService::clearBusFeeds()
{
    const QList<int> held = busFeeds_.keys();
    for (int busId : held)
        releaseBusFeed(busId);   // releases the tap and blanks the plane
    if (inputPump_ && busFeeds_.isEmpty() && cardPreviews_.isEmpty() && inputLabel_.isEmpty())
        inputPump_->stop();
}

QString LiveOutputService::inputFailure(const QString &label, const QString &kind) const
{
    if (label.isEmpty())
        return {};
    if (!cardPreviews_.contains(label) && label != inputLabel_)
        return {};
    if (kind == QLatin1String("ndi"))
        return {};
    return EngineBridge::instance().previewFailure(label);
}

bool LiveOutputService::inputAvailable(const QString &label, const QString &kind)
{
    if (label.isEmpty())
        return false;
    return EngineBridge::instance().inputSourceAlive(label, kind);
}

QString LiveOutputService::inputDelivery(const QString &label, const QString &kind)
{
    if (label.trimmed().isEmpty())
        return {};
    return EngineBridge::instance().inputDelivery(label, kind);
}

bool LiveOutputService::inputHealthy(const QString &label, const QString &kind) const
{
    return EngineBridge::instance().inputSourceReachable(label, kind);
}

void LiveOutputService::clearInput()
{
    if (inputLabel_.isEmpty())
        return;
    StopTap(inputKind_, inputLabel_, QStringLiteral("output"));
    producing_.remove(inputLabel_);
    inputLabel_.clear();
    inputKind_.clear();
    inputMode_.clear();
    inputLive_ = false;
    inputFrameSeq_ = 0;
    clearNdiInputFrame();   // the compositor frame side of an NDI take
    pl::CompositorState::Instance().ClearTakenInput();
    if (inputPump_ && cardPreviews_.isEmpty())
        inputPump_->stop();
    emit inputChanged();
}

// ---------------------------------------------------------------------------
//   1. a real still goes on air, with a NAME and a decoded FRAME (mediaRev
//   2. a path that does not exist is REFUSED and leaves what is on air alone
//   3. take-then-clear: a take still in flight must NOT come back on air
//   4. two takes back to back: the NEWEST wins, whichever decode finishes
static void selftestPass(const char *source, const QString &text)
{
    EngineBridge::write(QStringLiteral("info"), QString::fromLatin1(source),
                        QStringLiteral("PASS: ") + text);
}

static void runMediaTakeSelfTest();
static std::atomic<bool> g_mediaSelftestDone{false};
static void runMediaTakeSelfTestOnce()
{
    if (g_mediaSelftestDone.exchange(true))
        return;
    runMediaTakeSelfTest();
}

static void runMediaTakeSelfTest()
{
    auto *svc = &LiveOutputService::instance();
    const auto verdict = [](const char *phase, bool ok, const QString &detail) {
        const QString line = QStringLiteral("%1 — %2")
                                 .arg(QString::fromLatin1(phase), detail);
        if (ok)
            selftestPass("Media selftest", line);
        else
            qWarning("LiveOutputService[selftest] MEDIA FAIL: %s — %s", phase, qUtf8Printable(detail));
    };
    QStringList stills;
    for (const QString &dir : { QStringLiteral("C:/Windows/Web/Wallpaper/Windows"),
                                QStringLiteral("C:/Windows/Web/Wallpaper/Theme1"),
                                QStringLiteral("C:/Windows/Web/Wallpaper"),
                                QStringLiteral("C:/Windows/Web/Screen") }) {
        const QDir d(dir);
        if (!d.exists())
            continue;
        for (const QString &n : d.entryList({ QStringLiteral("*.jpg"), QStringLiteral("*.png") },
                                            QDir::Files, QDir::Size))
            stills << d.filePath(n);
    }
    stills.removeDuplicates();
    if (stills.isEmpty()) {
        qWarning("LiveOutputService[selftest] MEDIA SKIP: no still on this machine to take");
        return;
    }
    const QString pathA = stills.first();
    const QString pathB = stills.size() > 1 ? stills.at(1) : QString();
    const QString missing = QDir(QDir::tempPath()).filePath(QStringLiteral("vgr-selftest-no-such-file.png"));
    const qulonglong revBefore = svc->mediaRev();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Media selftest"),
                        QStringLiteral("phase: taking '%1'%2").arg(pathA,
                            pathB.isEmpty() ? QStringLiteral(" (no second still for the newest-wins probe)")
                                            : QString()));
    svc->takeMedia(pathA, QString());
    QTimer::singleShot(1500, svc, [verdict, pathA, pathB, revBefore, missing] {
        auto *svc = &LiveOutputService::instance();
        verdict("still on air",
                svc->mediaOnAir() && svc->mediaPath() == pathA && !svc->mediaName().isEmpty()
                    && !svc->mediaIsVideo() && svc->mediaState() == QLatin1String("playing")
                    && svc->mediaRev() > revBefore,
                QStringLiteral("onair=%1 path=%2 name=%3 rev=%4 (was %5)")
                    .arg(svc->mediaOnAir() ? 1 : 0)
                    .arg(svc->mediaPath() == pathA ? QStringLiteral("match") : svc->mediaPath())
                    .arg(svc->mediaName())
                    .arg(svc->mediaRev())
                    .arg(revBefore));
        // 2. A path that is not there: refused, and what is on air stays.
        svc->takeMedia(missing, QString());
        QTimer::singleShot(900, svc, [verdict, pathA, pathB, missing] {
            auto *svc = &LiveOutputService::instance();
            verdict("not-found refused", svc->mediaOnAir() && svc->mediaPath() == pathA,
                    QStringLiteral("onair=%1 path=%2 (asked '%3')")
                        .arg(svc->mediaOnAir() ? 1 : 0)
                        .arg(svc->mediaPath() == pathA ? QStringLiteral("untouched") : svc->mediaPath())
                        .arg(missing));
            // 3. Take, then clear while the decode is still in flight.
            svc->takeMedia(pathA, QString());
            svc->clearMedia();
            QTimer::singleShot(900, svc, [verdict, pathA, pathB] {
                auto *svc = &LiveOutputService::instance();
                verdict("clear cancels an in-flight take",
                        !svc->mediaOnAir() && svc->mediaPath().isEmpty(),
                        QStringLiteral("onair=%1 path='%2' (cleared during the take of '%3')")
                            .arg(svc->mediaOnAir() ? 1 : 0)
                            .arg(svc->mediaPath())
                            .arg(pathA));
                if (pathB.isEmpty())
                    return;
                // 4. Newest wins: the second take owns the layer even though the
                svc->takeMedia(pathA, QString());
                svc->takeMedia(pathB, QString());
                QTimer::singleShot(1800, svc, [verdict, pathB] {
                    auto *svc = &LiveOutputService::instance();
                    verdict("newest take wins", svc->mediaOnAir() && svc->mediaPath() == pathB,
                            QStringLiteral("onair=%1 path=%2 (want '%3')")
                                .arg(svc->mediaOnAir() ? 1 : 0)
                                .arg(svc->mediaPath() == pathB ? QStringLiteral("newest") : svc->mediaPath())
                                .arg(pathB));
                    svc->clearMedia();   // the self-test leaves NO side effects
                });
            });
        });
    });
}void LiveOutputService::runEnvSelfTest()
{
    if (!qEnvironmentVariableIsSet("VGR_OUTPUT_INPUT_TEST"))
        return;
    QTimer::singleShot(25000, &instance(), [] { runMediaTakeSelfTestOnce(); });
    QTimer::singleShot(2500, &instance(), [] {
        auto &svc = instance();
        const QString label = svc.PickSelfTestInputLabel();
        if (label.isEmpty()) {
            qWarning("LiveOutputService[selftest] FAIL: no window or monitor source to take");
            return;
        }
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Input selftest"),
                            QStringLiteral("taking '%1'").arg(label));
        svc.takeInput(label, QStringLiteral("screen"), QString());
        if (svc.inputLabel().isEmpty()) {
            qWarning("LiveOutputService[selftest] FAIL: take was refused");
            return;
        }
        QTimer::singleShot(4000, &svc, [label] { instance().RunInputTakeProbe(label); });
    });
}

QString LiveOutputService::PickSelfTestInputLabel() const
{
    const QVariantList screen = EngineBridge::instance().screenDevices();
    QString label;
    for (const QVariant &v : screen) {
        const QVariantMap d = v.toMap();
        if (d.value("id").toString().startsWith(QStringLiteral("win:"))) {
            label = d.value("label").toString();
            if (label != QCoreApplication::applicationName())
                return label;
        }
    }
    label.clear();
    for (const QVariant &v : screen) {
        const QVariantMap d = v.toMap();
        if (d.value("id").toString().startsWith(QStringLiteral("\\\\.\\DISPLAY"))) {
            label = d.value("label").toString();
            break;
        }
    }
    if (!label.isEmpty()) {
        qInfo("LiveOutputService[selftest] no window source — taking monitor '%s' instead",
              qUtf8Printable(label));
    }
    return label;
}

void LiveOutputService::RunInputTakeProbe(const QString &label)
{
    auto &svc = instance();
    if (svc.inputLive())
        selftestPass("Input selftest", QStringLiteral("frames decoded for '%1'").arg(label));
    else
        qWarning("LiveOutputService[selftest] FAIL: no frames decoded for '%s'",
                 qUtf8Printable(label));
    svc.clearInput();
    QTimer::singleShot(400, &svc, [label] {
        auto &s = instance();
        s.takeInput(label, QStringLiteral("screen"), QString());
        s.takeInput(label, QStringLiteral("screen"), QString());
        const bool held = s.inputLabel() == label;
        QTimer::singleShot(2500, &s, [label, held] { instance().RunInputReactivateProbe(label, held); });
    });
}

void LiveOutputService::RunInputReactivateProbe(const QString &label, bool held)
{
    auto &svc = instance();
    if (held && svc.inputLive()) {
        selftestPass("Input selftest",
                     QStringLiteral("re-activation kept the tap, frames flowing ('%1')").arg(label));
    } else {
        qWarning("LiveOutputService[selftest] REACTIVATE FAIL: held=%d live=%d",
                 held ? 1 : 0, svc.inputLive() ? 1 : 0);
    }
    svc.clearInput();
    svc.RunMeterProbe();
}

void LiveOutputService::RunMeterProbe()
{
    const QVariantList audio = EngineBridge::instance().audioDevices();
    QString audioLabel;
    for (const QVariant &v : audio) {
        const QVariantMap d = v.toMap();
        if (d.value("isInput").toBool()) {
            audioLabel = d.value("label").toString();
            break;
        }
    }
    if (audioLabel.isEmpty()) {
        qWarning("LiveOutputService[selftest] METER SKIP: no audio input device");
        return;
    }
    qInfo("LiveOutputService[selftest] metering input '%s'", qUtf8Printable(audioLabel));
    EngineBridge::instance().startInputMeter(audioLabel);
    QTimer::singleShot(1200, &instance(), [audioLabel] { instance().CheckMeterProbe(audioLabel); });
}

void LiveOutputService::CheckMeterProbe(const QString &audioLabel)
{
    QVariantMap snap;
    const QVariantList list = EngineBridge::instance().inputMeterList();
    for (const QVariant &v : list) {
        const QVariantMap s = v.toMap();
        if (s.value("label").toString().compare(audioLabel, Qt::CaseInsensitive) == 0) {
            snap = s;
            break;
        }
    }
    const int cc = snap.value("channelCount").toInt();
    if (cc > 0) {
        const QVariantList peaks = snap.value("peaks").toList();
        selftestPass("Meter selftest",
                     QStringLiteral("'%1' tap live, %2 channels, peak=%3")
                         .arg(audioLabel)
                         .arg(cc)
                         .arg(peaks.isEmpty() ? 0.0 : peaks.first().toDouble()));
    } else {
        qWarning("LiveOutputService[selftest] METER FAIL: '%s' produced no snapshot (no signal)",
                 qUtf8Printable(audioLabel));
    }
    EngineBridge::instance().stopInputMeter(audioLabel);
    runMediaTakeSelfTestOnce();
}
