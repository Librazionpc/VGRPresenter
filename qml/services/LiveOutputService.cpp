#include "core/common/StageTiming.hpp"
#include "services/CallTrace.h"
#include "services/LiveOutputService.h"
#include "services/StepCost.hpp"

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
#include "core/logging/ChangeGate.hpp"
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
#include <memory>
#include <mutex>
#include <unordered_map>
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

#include <QLoggingCategory>

#include "services/NdiLog.hpp"

Q_LOGGING_CATEGORY(lcNdiProgress, "vgr.ndi.progress");

#include <utility>

namespace pl = bps::presentation;
namespace pr = bps::rendering;

#include "services/PressTimeline.h"
#include "services/SlidePreviewMap.h"

services::diagnostics::PressTimeline services::diagnostics::g_pressTimeline;

using services::diagnostics::g_pressTimeline;
using services::diagnostics::pressTimelineStep;
using services::preview::outputStyleForPreview;
using services::preview::setSlideReadingFlow;
using services::preview::slideToVariantMap;

namespace {
void pushActiveOverlaysToCompositor(const QVariantList &activeOverlays)
{
    std::vector<pl::CompositorState::ActiveOverlay> out;
    out.reserve(static_cast<size_t>(activeOverlays.size()));
    for (const QVariant &v : activeOverlays) {
        const QString id = v.toMap().value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        const QVariantMap design = OverlayLibraryService::instance().design(id);
        if (design.isEmpty())
            continue;   // a deleted/missing overlay — drop it silently, same
        pl::CompositorState::ActiveOverlay ao;
        ao.id = id.toStdString();
        ao.placeUnderSlide = design.value(QStringLiteral("placeUnderSlide")).toBool();
        ao.background = design.value(QStringLiteral("background")).toString().toStdString();
        for (const QVariant &b : design.value(QStringLiteral("blocks")).toList())
            ao.blocks.push_back(ShowConverter::blockFromVariant(b.toMap()));
        out.push_back(std::move(ao));
    }
    pl::CompositorState::Instance().SetActiveOverlays(std::move(out));
}
} // namespace

LiveOutputService *LiveOutputService::s_instance = nullptr;

LiveOutputService::LiveOutputService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;

    previewTimer_ = new QTimer(this);
    previewTimer_->setInterval(1000);
    connect(previewTimer_, &QTimer::timeout, this, []() {
        if (EngineBridge::instance().booted())
            (void)bps::live::LiveOutputController::Instance().StartIdlePreview();
    });
    previewTimer_->start();

    ensurePollTimer();

    connect(&EngineBridge::instance(), &EngineBridge::previewRefused,
            this, &LiveOutputService::onPreviewRefused);
}

void LiveOutputService::onPreviewRefused(const QString &label, const QString &kind)
{
    const QString why = EngineBridge::instance().previewFailure(label);
    if (cardPreviews_.contains(label) && cardPreviews_.value(label).toMap()
                                            .value(QStringLiteral("kind")).toString() == kind) {
        endPreviewInput(label);   // same unwind a released card preview takes
        qWarning("LiveOutputService: card preview of '%s' refused: %s",
                 qUtf8Printable(label), qUtf8Printable(why));
        return;
    }
    if (inputLabel_ != label || inputKind_ != kind)
        return;   // not ours: another holder of this label took the refusal
    qWarning("LiveOutputService: take of '%s' refused: %s — unwinding",
             qUtf8Printable(label), qUtf8Printable(why));
    clearInput();   // the SAME unwind the operator's own clear runs
}

LiveOutputService &LiveOutputService::instance()
{
    static LiveOutputService inst;
    return inst;
}

LiveOutputService *LiveOutputService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void LiveOutputService::goLive()
{
    VGR_CALL_TRACE("LiveOutputService.goLive");
    g_pressTimeline.arm();
    qInfo("PRESS TIMELINE: press | live=%d stagedSlides=%d mediaOnAir=%d mediaIsVideo=%d "
          "mediaState='%s' mediaRev=%d input='%s' overlays=%d ndiSending=%d '%s'",
          live_ ? 1 : 0, static_cast<int>(stagedSlidesRaw_.size()),
          mediaOnAir_ ? 1 : 0, mediaIsVideo_ ? 1 : 0,
          qUtf8Printable(mediaState_), mediaRev_, qUtf8Printable(inputLabel_),
          static_cast<int>(activeOverlays_.size()), ndiSending_ ? 1 : 0,
          qUtf8Printable(ndiSendingOutput_));
    if (!stagedSlidesRaw_.isEmpty()) {
        pressTimelineStep("branch: staged pick");
        goLiveWithSlides(stagedName_, stagedSlidesRaw_);
        return;                     // the last step inside closes the trace
    }
    pressTimelineStep("branch: blank carrier (nothing staged)");

    EventBus::instance().notify(QStringLiteral("Going live…"),
                                QStringLiteral("info"), QStringLiteral("Go Live"));
    goLiveBlank();
}

void LiveOutputService::goLiveWithSlides(const QString &name, const QVariantList &slides)
{
    VGR_CALL_TRACE("LiveOutputService.goLiveWithSlides");
    readingEngineFlowText_.clear();   // a new commit: the engine's passage is whatever these slides carry
    lastRepublishSig_.clear();
    bps::presentation::Presentation content;
    content.name = name.toStdString();
    for (const QVariant &v : slides) {
        QVariantMap raw = v.toMap();
        setSlideReadingFlow(raw, scrollMode_,
                            raw.value(QStringLiteral("flowText")).toString(),
                            raw.value(QStringLiteral("flowChar"), 0.0).toDouble());
        // The staged slide carries the line breaks cut when it was staged; the walk may have moved since, and
        // every output's table is anchored at the word it has reached — what goes out is the CURRENT table.
        if (scrollMode_ && lineSourceTextOnly_ && readingBudgetFitActive_ && !readingFlowText_.isEmpty()
                && raw.value(QStringLiteral("flowText")).toString() == readingFlowText_)
            raw.insert(QStringLiteral("flowBreaks"), readingBreaksFor(readingFlowText_));
        auto slide = ShowConverter::slideFromVariant(raw);
        qInfo("live slide: title='%s' rawHasBody=%d bodyChars=%d textChars=%d blocks=%d fill=%s "
              "flowChars=%d flowCharAt=%.2f",
              slide.title.c_str(),
              raw.contains(QStringLiteral("body")) ? 1 : 0,
              static_cast<int>(raw.value(QStringLiteral("body")).toString().size()),
              static_cast<int>(slide.text.size()),
              static_cast<int>(slide.blocks.size()),
              raw.value(QStringLiteral("fill")).toMap().isEmpty() ? "no" : "yes",
              static_cast<int>(slide.flowText.size()),
              static_cast<double>(slide.flowChar));
        content.slides.push_back(std::move(slide));
    }
    if (content.slides.empty()) {
        qWarning("LiveOutputService: goLiveWithSlides: no slides in '%s'", name.toUtf8().constData());
        EventBus::instance().notify(QStringLiteral("Nothing selected to put on air"),
                                    QStringLiteral("warning"), QStringLiteral("Go Live"));
        emit onAirChanged();
        if (g_pressTimeline.armed()) {
            g_pressTimeline.report("press ended: nothing to send");
            g_pressTimeline.close();
        }
        return;
    }
    pressTimelineStep("slides converted to engine shape");
    auto &controller = bps::live::LiveOutputController::Instance();
    const bool replacingLiveContent = controller.IsLive();
    const qint64 startEnterMs = QDateTime::currentMSecsSinceEpoch();
    auto r = controller.StartFromSlides(name.toStdString(), content.slides);
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch() - startEnterMs;
    if (g_pressTimeline.armed())
        qInfo("PRESS TIMELINE: step 'StartFromSlides(%s)' took %lld ms -> %s",
              replacingLiveContent ? "replace" : "fresh start",
              static_cast<long long>(startMs), r.ok() ? "ok" : "FAILED");
    if (!r.ok()) {
        const QString reason = QString::fromStdString(r.error().message);
        qWarning("LiveOutputService: goLiveWithSlides failed: %s", r.error().message.c_str());
        EventBus::instance().notify(reason, QStringLiteral("warning"), QStringLiteral("Go Live"));
        emit onAirChanged();
        if (g_pressTimeline.armed()) {
            g_pressTimeline.report("press ended: engine refused");
            g_pressTimeline.close();
        }
        return;
    }
    armNdiSenderForSession();
    pressTimelineStep("armNdiSenderForSession");
    // (GO LIVE puts out what the Monitor Preview shows and plays — nothing else. It used to start
    // the program's audio playout here, which opened the speakers and the microphone with
    // nothing playing; the audio engine now follows the Routing screen alone.)
    if (!content.slides.front().clearStage) {
        blackoutActive_ = false;
        blackoutRestoreTitle_.clear();
        blackoutRestoreSlides_.clear();
    }

    if (!live_) {
        live_ = true;
        emit liveChanged();
    }
    if (!stagedSlidesRaw_.isEmpty() || !stagedSlide_.isEmpty()) {
        stagedName_.clear();
        stagedSlidesRaw_.clear();
        stagedSlide_ = QVariantMap{};
        BumpStaged();
    }
    if (!replacingLiveContent) {
        refreshOnAirSlide();
        emit onAirChanged();
    }
    pressTimelineStep(replacingLiveContent ? "refreshOnAirSlide deferred to pollTick"
                                       : "refreshOnAirSlide + onAirChanged");
    ensurePollTimer();          // already armed since boot — a no-op here
    startNdiSendClock();        // the NDI feed's own clock, paced by the output's refresh rate
    pollTick();
    pressTimelineStep("startNdiSendClock + pollTick");
    if (g_pressTimeline.armed()) {
        qInfo("PRESS TIMELINE: done | live=%d onAir='%s' ndiSending=%d '%s' mediaOnAir=%d",
              live_ ? 1 : 0, qUtf8Printable(onAirTitle_), ndiSending_ ? 1 : 0,
              qUtf8Printable(ndiSendingOutput_), mediaOnAir_ ? 1 : 0);
        g_pressTimeline.report("press done");
        g_pressTimeline.close();
    }
}

// The one way a staged change is announced. The revision is what a surface memoises on:
// "is this the pick I already composed?" must not cost a read of the slide itself.
void LiveOutputService::BumpStaged()
{
    ++stagedRev_;
    emit stagedChanged();
}

void LiveOutputService::stageSlides(const QString &name, const QVariantList &slides)
{
    stageSlidesWithPreview(name, slides, {});
}

// ---- CLEAR ALL / SHOW ALL ------------------------------------------------
void LiveOutputService::clearAll()
{
    VGR_CALL_TRACE("LiveOutputService.clearAll");
    ClearedSnapshot snap;
    if (live_) {
        snap.contentSlidesRaw = onAirSlidesRaw();
        if (!snap.contentSlidesRaw.isEmpty()) {
            snap.contentWasOnAir = true;
            snap.contentTitle = onAirTitle_;
            snap.contentStyled = onAirSlide_;
        }
    } else {
        snap.contentSlidesRaw = stagedSlidesRaw_;
        snap.contentWasOnAir = false;
        snap.contentTitle = stagedName_;
        snap.contentStyled = stagedSlide_;
    }
    snap.inputLabel = inputLabel_;
    snap.inputKind = inputKind_;
    snap.inputMode = inputMode_;
    if (mediaOnAir_) {
        snap.mediaPath = mediaPath_;
        snap.mediaName = mediaName_;
    }
    snap.overlays = activeOverlays_;
    snap.valid = !snap.contentSlidesRaw.isEmpty() || !snap.inputLabel.isEmpty()
                 || !snap.mediaPath.isEmpty() || !snap.overlays.isEmpty();
    cleared_ = snap;
    emit clearedChanged();

    if (!inputLabel_.isEmpty())
        clearInput();
    if (mediaOnAir_)
        clearMedia();
    if (!activeOverlays_.isEmpty())
        clearAllOverlays();
    if (live_ && !onAirSlide_.value(QStringLiteral("blocks")).toList().isEmpty())
        clearOnAirSlide();
    if (!stagedSlidesRaw_.isEmpty())
        clearStaged();
}

void LiveOutputService::restoreCleared()
{
    VGR_CALL_TRACE("LiveOutputService.restoreCleared");
    if (!cleared_.valid)
        return;
    const ClearedSnapshot snap = cleared_;
    cleared_ = ClearedSnapshot{};   // one-shot: consumed the moment it is used
    emit clearedChanged();

    if (!snap.inputLabel.isEmpty())
        takeInput(snap.inputLabel, snap.inputKind, snap.inputMode);
    else if (!snap.mediaPath.isEmpty())
        takeMedia(snap.mediaPath, snap.mediaName);
    for (const QVariant &v : snap.overlays) {
        const QVariantMap o = v.toMap();
        takeOverlay(o.value(QStringLiteral("id")).toString(),
                    o.value(QStringLiteral("name")).toString());
    }
    if (snap.contentSlidesRaw.isEmpty())
        return;
    if (snap.contentWasOnAir && live_)
        goLiveWithSlides(snap.contentTitle, snap.contentSlidesRaw);
    else {
        stagedName_ = snap.contentTitle;
        stagedSlidesRaw_ = snap.contentSlidesRaw;
        stagedSlide_ = snap.contentStyled;
        BumpStaged();
    }
}

void LiveOutputService::clearStaged()
{
    if (stagedSlidesRaw_.isEmpty() && stagedSlide_.isEmpty())
        return;
    stagedName_.clear();
    stagedSlidesRaw_.clear();
    stagedSlide_ = QVariantMap{};
    BumpStaged();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
void LiveOutputService::takeOverlay(const QString &id, const QString &name)
{
    if (id.isEmpty())
        return;
    for (const QVariant &v : std::as_const(activeOverlays_))
        if (v.toMap().value(QStringLiteral("id")).toString() == id)
            return;   // already on — callers toggle via overlayIsOnAir
    activeOverlays_.append(QVariantMap{ { QStringLiteral("id"), id },
                                        { QStringLiteral("name"), name } });
    pushActiveOverlaysToCompositor(activeOverlays_);
    emit overlaysChanged();
}

void LiveOutputService::clearOverlay(const QString &id)
{
    for (int i = 0; i < activeOverlays_.size(); ++i) {
        if (activeOverlays_.at(i).toMap().value(QStringLiteral("id")).toString() == id) {
            activeOverlays_.removeAt(i);
            pushActiveOverlaysToCompositor(activeOverlays_);
            emit overlaysChanged();
            return;
        }
    }
}

void LiveOutputService::clearAllOverlays()
{
    if (activeOverlays_.isEmpty())
        return;
    activeOverlays_.clear();
    pushActiveOverlaysToCompositor(activeOverlays_);
    emit overlaysChanged();
}

bool LiveOutputService::overlayIsOnAir(const QString &id) const
{
    for (const QVariant &v : std::as_const(activeOverlays_))
        if (v.toMap().value(QStringLiteral("id")).toString() == id)
            return true;
    return false;
}

void LiveOutputService::stop()
{
    g_pressTimeline.arm();
    qInfo("PRESS TIMELINE: press (STOP) | live=%d stagedSlides=%d mediaOnAir=%d "
          "mediaIsVideo=%d input='%s' overlays=%d ndiSending=%d '%s'",
          live_ ? 1 : 0, static_cast<int>(stagedSlidesRaw_.size()),
          mediaOnAir_ ? 1 : 0, mediaIsVideo_ ? 1 : 0, qUtf8Printable(inputLabel_),
          static_cast<int>(activeOverlays_.size()), ndiSending_ ? 1 : 0,
          qUtf8Printable(ndiSendingOutput_));
    const bool wasLive = live_;
    bps::live::LiveOutputController::Instance().StopLiveAsync();
    pressTimelineStep("StopLiveAsync (the join is on a reaper thread)");
    if (live_) {
        live_ = false;
        emit liveChanged();
    }
    clearBusFeeds();
    if (!blackoutRestoreSlides_.isEmpty()) {
        stagedName_ = blackoutRestoreTitle_;
        stagedSlidesRaw_ = blackoutRestoreSlides_;
        stagedSlide_ = previewWithActiveStyle(blackoutRestoreSlides_.first().toMap());
        BumpStaged();
        blackoutActive_ = false;
        blackoutRestoreTitle_.clear();
        blackoutRestoreSlides_.clear();
    } else if (!onAirSlideRaw_.isEmpty()) {
        stagedName_ = onAirTitle_;
        stagedSlidesRaw_ = QVariantList{ onAirSlideRaw_ };
        stagedSlide_ = onAirSlide_;
        BumpStaged();
    }
    onAirSlide_ = QVariantMap{};
    onAirSlideRaw_ = QVariantMap{};
    onAirTitle_.clear();
    onAirIndex_ = -1;
    onAirTotal_ = 0;
    emit onAirChanged();
    stopNdiSendClock();
    stopNdiFeed();
    if (!outputRuntime_.isEmpty()) {
        outputRuntime_.clear();
        emit outputRuntimeChanged();
    }
    clearNdiInputFrame();
    if (wasLive)
        EventBus::instance().notify(QStringLiteral("Going off air…"),
                                    QStringLiteral("info"), QStringLiteral("Go Live"));
    pressTimelineStep("stop teardown done");
    g_pressTimeline.report("press (STOP) done");
    g_pressTimeline.close();
}

void LiveOutputService::goLiveBlank()
{
    VGR_CALL_TRACE("LiveOutputService.goLiveBlank");
    QVariantMap blank;
    blank.insert(QStringLiteral("blocks"), QVariantList{});
    blank.insert(QStringLiteral("background"), QStringLiteral("transparent"));
    blank.insert(QStringLiteral("title"), QString());
    goLiveWithSlides(QString(), { blank });
}

void LiveOutputService::clearOnAirSlide()
{
    if (!live_)
        return;
    goLiveBlank();
}

void LiveOutputService::blackout()
{
    if (!live_ || blackedOut())
        return;
    blackoutActive_ = true;
    blackoutRestoreTitle_ = onAirTitle_;
    blackoutRestoreSlides_ = onAirSlidesRaw();

    QVariantMap clear;
    clear.insert(QStringLiteral("blocks"), QVariantList{});
    clear.insert(QStringLiteral("background"), QStringLiteral("transparent"));
    clear.insert(QStringLiteral("title"), QString());
    clear.insert(QStringLiteral("clearStage"), true);
    goLiveWithSlides(QString(), { clear });
}

void LiveOutputService::endBlackout()
{
    if (!blackedOut())
        return;
    blackoutActive_ = false;
    const QString title = blackoutRestoreTitle_;
    const QVariantList slides = blackoutRestoreSlides_;
    blackoutRestoreTitle_.clear();
    blackoutRestoreSlides_.clear();
    if (slides.isEmpty()) {
        goLiveBlank();
        return;
    }
    goLiveWithSlides(title, slides);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
bool LiveOutputService::next()
{
    if (!live_)
        return false;
    auto r = pl::PresentationEngine::Instance().Next();
    if (r.ok()) {
        refreshOnAirSlide();
        emit onAirChanged();
        return true;
    }
    return false;
}

bool LiveOutputService::previous()
{
    if (!live_)
        return false;
    auto r = pl::PresentationEngine::Instance().Previous();
    if (r.ok()) {
        refreshOnAirSlide();
        emit onAirChanged();
        return true;
    }
    return false;
}

void LiveOutputService::stepPassage(int direction)
{
    // The slide path's half of the press measurement (see StepCost.hpp): lineStep carries
    // the scroll half, and the two lines are what the two paths cost side by side.
    services::diagnostics::StepCost stepCost("slide", direction);
    if (!live_) {
        if (!stagedSlidesRaw_.isEmpty())
            emit passageStepRequested(direction);
        return;
    }
    const bool moved = direction < 0 ? previous() : next();
    if (!moved)
        emit passageStepRequested(direction);
}

bool LiveOutputService::stagedIsOurs(const QString &title) const
{
    const bool result = !live_ && !stagedSlidesRaw_.isEmpty()
                       && !title.isEmpty() && stagedName_ == title;
    const QString line = QStringLiteral("title='%1' stagedName_='%2' live_=%3 stagedEmpty=%4 result=%5")
                             .arg(title)
                             .arg(stagedName_)
                             .arg(live_)
                             .arg(stagedSlidesRaw_.isEmpty())
                             .arg(result);
    if (line != lastStagedIsOursLine_) {
        lastStagedIsOursLine_ = line;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StagedIsOurs"), line);
    }
    return result;
}

void LiveOutputService::setWallStepper(const QString &id, bool active)
{
    if (id.isEmpty())
        return;
    const bool before = wallSteppable();
    const QString ownerBefore = wallStepperId_;
    if (active) {
        wallSteppers_.insert(id, true);
    } else {
        wallSteppers_.remove(id);
    }
    if (active) {
        wallStepperId_ = id;
    } else if (wallStepperId_ == id) {
        wallStepperId_.clear();
        for (auto it = wallSteppers_.cbegin(); it != wallSteppers_.cend(); ++it) {
            if (it.value()) {
                wallStepperId_ = it.key();
                break;
            }
        }
    }
    if (wallSteppable() != before || wallStepperId_ != ownerBefore)
        emit wallStepperChanged();
}

bool LiveOutputService::wallSteppable() const
{
    for (auto it = wallSteppers_.cbegin(); it != wallSteppers_.cend(); ++it)
        if (it.value())
            return true;
    return false;
}

bool LiveOutputService::jumpTo(int index)
{
    if (!live_ || index < 0)
        return false;
    auto r = pl::PresentationEngine::Instance().JumpTo(size_t(index));
    if (r.ok()) {
        refreshOnAirSlide();
        emit onAirChanged();
        return true;
    }
    return false;
}

bool LiveOutputService::jumpToGroup(const QString &tag)
{
    if (!live_ || tag.isEmpty())
        return false;
    auto r = pl::PresentationEngine::Instance().JumpByTag(tag.toStdString());
    if (r.ok()) {
        refreshOnAirSlide();
        emit onAirChanged();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
bool LiveOutputService::blackedOut() const
{
    return live_ && (blackoutActive_
                     || onAirSlide_.value(QStringLiteral("clearStage")).toBool());
}

QVariantList LiveOutputService::onAirSlidesRaw() const
{
    QVariantList out;
    if (!live_)
        return out;
    const std::vector<pl::Slide> slides =
        bps::live::LiveOutputController::Instance().OnAirSlidesSnapshot();
    for (const pl::Slide &s : slides) {
        if (s.hidden)
            continue;
        out.append(slideToVariantMap(&s, pl::OutputStyleSpec{}));
    }
    return out;
}

void LiveOutputService::refreshOnAirSlide()
{
    pl::Slide current;
    const bool haveSlide = live_
        && bps::live::LiveOutputController::Instance().OnAirSlideSnapshot(current);
    const pl::Slide *slide = haveSlide ? &current : nullptr;
    onAirSlide_ = slide ? slideToVariantMap(slide, pl::PresentationEngine::Instance().ActiveOutputStyle())
                        : QVariantMap{};
    onAirSlideRaw_ = slide ? slideToVariantMap(slide, pl::OutputStyleSpec{}) : QVariantMap{};
    if (slide && !slide->flowText.empty()) {
        onAirSlideRaw_.insert(QStringLiteral("flowText"), QString::fromStdString(slide->flowText));
        onAirSlideRaw_.insert(QStringLiteral("flowChar"), slide->flowChar);
    }
    const QString tag = (slide && !slide->tags.empty())
                            ? QString::fromStdString(slide->tags.front()) : QString();
    if (tag != onAirGroupTag_) {
        onAirGroupTag_ = tag;
        if (OutputListModel *outputs = OutputListModel::instance())
            outputs->repushActiveStyleForTag(tag);
    }
}

bool LiveOutputService::stagedHasFlow() const
{
    const QVariantMap slide = stagedSlidesRaw_.isEmpty() ? stagedSlide_
                                                        : stagedSlidesRaw_.first().toMap();
    return !slide.value(QStringLiteral("flowText")).toString().isEmpty();
}

bool LiveOutputService::onAirHasFlow() const
{
    return !onAirSlideRaw_.value(QStringLiteral("flowText")).toString().isEmpty();
}

QVariantMap LiveOutputService::previewWithActiveStyle(const QVariantMap &slide) const
{
    const pl::Slide engineSlide = ShowConverter::slideFromVariant(slide);
    const QString tag = slide.value(QStringLiteral("tag")).toString();
    if (!tag.isEmpty() && !GroupListModel::instance()->templateForTag(tag).isEmpty()) {
        const QString styleId = OutputListModel::instance()
                                    ? OutputListModel::instance()->activeStyleId() : QString();
        return slideToVariantMap(&engineSlide, outputStyleForPreview(styleId, tag));
    }
    return slideToVariantMap(&engineSlide);
}

static bool samePictureApartFromReading(const QVariantMap &a, const QVariantMap &b)
{
    const auto isReadingField = [](const QString &key) {
        return key == QLatin1String("flowText") || key == QLatin1String("flowChar");
    };
    int countA = 0;
    for (auto it = a.cbegin(); it != a.cend(); ++it) {
        if (isReadingField(it.key()))
            continue;
        ++countA;
        const auto other = b.constFind(it.key());
        if (other == b.cend() || other.value() != it.value())
            return false;
    }
    int countB = 0;
    for (auto it = b.cbegin(); it != b.cend(); ++it)
        if (!isReadingField(it.key()))
            ++countB;
    return countA == countB;
}

QVariantMap LiveOutputService::readingStagedPreview(const QVariantMap &slide)
{
    const auto *model = OutputListModel::instance();
    const QString key = QStringLiteral("style=%1|live=%2|take=%3|rev=%4|input=%5")
                            .arg(model ? model->activeStyleId() : QString())
                            .arg(live_ ? 1 : 0)
                            .arg(mediaOnAir() ? 1 : 0)
                            .arg(mediaRev())
                            .arg(inputLabel());
    if (!readingPreviewOut_.isEmpty() && key == readingPreviewKey_
        && samePictureApartFromReading(readingPreviewRaw_, slide))
        return readingPreviewOut_;
    readingPreviewKey_ = key;
    readingPreviewRaw_ = slide;
    readingPreviewOut_ = previewWithActiveStyle(slide);
    return readingPreviewOut_;
}

QVariantMap LiveOutputService::previewWithStyle(const QVariantMap &slide,
                                                 const QString &styleId) const
{
    // Interface-thread cost, counted: this is a whole slide converted and re-styled, and QML asks for it
    // from many handlers. "previewWithStyle: N in 5 s" says how often; the figure beside it, how long.
    static bps::timing::StageTiming timing("previewWithStyle", 5.0);
    const auto started = std::chrono::steady_clock::now();
    const pl::Slide engineSlide = ShowConverter::slideFromVariant(slide);
    QVariantMap result = slideToVariantMap(&engineSlide,
                                           outputStyleForPreview(styleId,
                                                                 slide.value(QStringLiteral("tag")).toString()));
    timing.Add("compose", bps::timing::MsSince(started));
    timing.EndUnit();
    return result;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
qulonglong LiveOutputService::previewFramesNow() const
{
    return bps::live::LiveOutputController::Instance().PreviewFrames();
}

qulonglong LiveOutputService::framesSentNow() const
{
    return bps::live::LiveOutputController::Instance().FramesSent();
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
namespace {
constexpr qint64 kFeedProbeMs = 1000;      // how often the stamps are read
constexpr qint64 kFeedGraceMs = 4000;      // GO LIVE warm-up: buffers start empty
constexpr qint64 kFeedStallMs = 3500;      // three missed heartbeats is a stall
constexpr qint64 kFeedRestoredMs = 6000;   // how long "Feed restored" stays up

qint64 SteadyMsNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

void LiveOutputService::setFeedNotice(const QString &text, const QString &kind)
{
    if (feedNoticeText_ == text && feedNoticeKind_ == kind)
        return;
    feedNoticeText_ = text;
    feedNoticeKind_ = kind;
    emit feedHealthChanged();
}

bool LiveOutputService::outputFeedStalled(const QString &outputName) const
{
    return stalledOutputs_.contains(outputName);
}

void LiveOutputService::sampleFeedHealth()
{
    if (!live_) {
        ClearFeedHealthWhenIdle();
        return;
    }

    const qint64 now = SteadyMsNow();
    if (liveSinceMs_ == 0)
        liveSinceMs_ = now;
    if (now - lastFeedProbeMs_ < kFeedProbeMs)
        return;
    lastFeedProbeMs_ = now;
    const auto *model = OutputListModel::instance();
    if (!model)
        return;

    const auto ages = bps::rendering::RenderEngine::Instance().OutputFeedAges();
    QHash<QString, FeedSample> sampleByBuffer;
    sampleByBuffer.reserve(static_cast<int>(ages.size()));
    for (const auto &row : ages) {
        sampleByBuffer.insert(QString::fromStdString(row.name),
                              FeedSample{static_cast<qint64>(row.ageMs),
                                         static_cast<qulonglong>(row.frames)});
    }

    PublishOutputRuntime(sampleByBuffer);

    if (now - liveSinceMs_ < kFeedGraceMs)
        return;

    QStringList labels;
    std::vector<std::string> toBlack;
    const QSet<QString> stalled = StalledOutputsFrom(sampleByBuffer, labels, toBlack);

    const QString labelLine = labels.join(QStringLiteral(", "));
    const bool wasStalled = !stalledOutputs_.isEmpty();
    const QString wasLabelLine = stalledLabels_;
    const bool changed = stalled != stalledOutputs_ || labelLine != stalledLabels_;
    stalledOutputs_ = stalled;
    stalledLabels_ = labelLine;
    if (changed) {
        ++feedHealthRev_;
        emit feedHealthChanged();
    }

    if (!stalled.isEmpty()) {
        AnnounceFeedStalled(labelLine, labels, toBlack);
        return;
    }
    if (wasStalled)
        AnnounceFeedRestored(wasLabelLine);
}

void LiveOutputService::ClearFeedHealthWhenIdle()
{
    liveSinceMs_ = 0;
    lastFeedProbeMs_ = 0;
    const bool hadRuntime = !outputRuntime_.isEmpty();
    if (stalledOutputs_.isEmpty() && feedNoticeText_.isEmpty()
        && stalledLabels_.isEmpty() && !hadRuntime)
        return;
    stalledOutputs_.clear();
    stalledLabels_.clear();
    feedNoticeText_.clear();
    feedNoticeKind_.clear();
    outputRuntime_.clear();
    ++feedHealthRev_;
    emit feedHealthChanged();
    if (hadRuntime)
        emit outputRuntimeChanged();
}

QString LiveOutputService::FeedBufferForRow(int row) const
{
    const auto *model = OutputListModel::instance();
    if (!model || !model->hasOwnPass(row))
        return QString::fromLatin1(bps::live::LiveOutputController::kPreviewName);
    return QStringLiteral("__out_%1__")
        .arg(qHash(model->getOutput(row).value(QStringLiteral("name")).toString()));
}

void LiveOutputService::PublishOutputRuntime(const QHash<QString, FeedSample> &samples)
{
    const auto *model = OutputListModel::instance();
    if (!model)
        return;
    QVariantMap runtime;
    for (int i = 0; i < model->rowCount(); ++i) {
        const QVariantMap row = model->getOutput(i);
        const FeedSample sample = samples.value(FeedBufferForRow(i));
        QVariantMap state;
        state.insert(QStringLiteral("presented"), sample.ageMs >= 0);
        state.insert(QStringLiteral("ageMs"), QVariant::fromValue<qint64>(sample.ageMs));
        state.insert(QStringLiteral("frames"), QVariant::fromValue<qulonglong>(sample.frames));
        state.insert(QStringLiteral("enabled"), row.value(QStringLiteral("isEnabled")).toBool());
        runtime.insert(row.value(QStringLiteral("name")).toString(), state);
    }
    if (runtime == outputRuntime_)
        return;
    outputRuntime_ = runtime;
    emit outputRuntimeChanged();
}

QSet<QString> LiveOutputService::StalledOutputsFrom(const QHash<QString, FeedSample> &samples,
                                                    QStringList &labels,
                                                    std::vector<std::string> &toBlack)
{
    QSet<QString> stalled;
    const auto *model = OutputListModel::instance();
    if (!model)
        return stalled;
    for (int i = 0; i < model->rowCount(); ++i) {
        const QVariantMap row = model->getOutput(i);
        if (!row.value(QStringLiteral("isEnabled")).toBool())
            continue;
        const QString name = row.value(QStringLiteral("name")).toString();
        const QString buffer = FeedBufferForRow(i);
        const auto age = samples.constFind(buffer);
        if (age == samples.constEnd() || age.value().ageMs < 0)
            continue;   // never presented to yet: warming, not stalled
        if (age.value().ageMs <= kFeedStallMs)
            continue;
        stalled.insert(name);
        labels << QStringLiteral("M%1").arg(i + 1);
        if (!stalledOutputs_.contains(name))
            toBlack.push_back(buffer.toStdString());
    }
    return stalled;
}

void LiveOutputService::AnnounceFeedStalled(const QString &labelLine, const QStringList &labels,
                                            const std::vector<std::string> &toBlack)
{
    if (!toBlack.empty()) {
        qInfo("LiveOutputService: feed stalled — %s: no frames for over %lld ms, blacked out",
              qUtf8Printable(labelLine), static_cast<long long>(kFeedStallMs));
        vgr::EngineWorker::instance().runOffThread([buffers = toBlack]() {
            (void)bps::rendering::RenderEngine::Instance().BlackoutOutputs(buffers);
        });
    }
    EventBus::instance().notify(
        QStringLiteral("%1 · No live feed — blacked out until frames return").arg(labelLine),
        QStringLiteral("error"), QStringLiteral("Live output"),
        QStringLiteral("live.feed.stalled"), labels);
    if (feedNoticeTimer_)
        feedNoticeTimer_->stop();
    setFeedNotice(QStringLiteral("%1 · No live feed").arg(labelLine),
                  QStringLiteral("stalled"));
}

void LiveOutputService::AnnounceFeedRestored(const QString &labelLine)
{
    qInfo("LiveOutputService: feed restored — %s: frames flowing again",
          qUtf8Printable(labelLine));
    setFeedNotice(QStringLiteral("%1 · Feed restored").arg(labelLine),
                  QStringLiteral("restored"));
    EventBus::instance().notify(
        QStringLiteral("%1 · Feed restored").arg(labelLine),
        QStringLiteral("success"), QStringLiteral("Live output"),
        QStringLiteral("live.feed.restored"));
    if (!feedNoticeTimer_) {
        feedNoticeTimer_ = new QTimer(this);
        feedNoticeTimer_->setSingleShot(true);
        connect(feedNoticeTimer_, &QTimer::timeout, this, [this]() {
            if (!feedStalled())
                setFeedNotice(QString(), QString());
        });
    }
    feedNoticeTimer_->start(static_cast<int>(kFeedRestoredMs));
}

void LiveOutputService::ensurePollTimer()
{
    if (poll_) {
        if (!poll_->isActive())
            poll_->start();
        return;
    }
    poll_ = new QTimer(this);
    poll_->setInterval(100);
    connect(poll_, &QTimer::timeout, this, &LiveOutputService::pollTick);
    poll_->start();

    // THE MONITOR TILES' CLOCK. Every tile repaints when frameRev moves, and pollTick (100 ms, with all
    // the slower bookkeeping) was the only thing moving it: a 30 fps video showed in the monitor at 10
    // fps — it looked laggy whatever the engine did. This timer only compares two counters and bumps the
    // revision, ~30 times a second, so the monitor follows the picture the engine actually composed.
    auto *previewClock = new QTimer(this);
    previewClock->setInterval(33);
    previewClock->setTimerType(Qt::PreciseTimer);
    connect(previewClock, &QTimer::timeout, this, [this]() {
        auto &ctrl = bps::live::LiveOutputController::Instance();
        bool moved = false;
        if (ctrl.PreviewFrames() != previewFrames_) {
            previewFrames_ = ctrl.PreviewFrames();
            moved = true;
        }
        if (live_) {
            const qulonglong sent = ctrl.FramesSent();
            if (sent != framesSent_) {
                framesSent_ = sent;
                moved = true;
            }
        }
        if (moved) {
            ++frameRev_;
            emit frameRevChanged();
        }
    });
    previewClock->start();
}

void LiveOutputService::pollTick()
{
    connectToNdiOutputRoster();
    connectReadingBasis();   // once, on the first tick that finds the model
    auto &ctrl = bps::live::LiveOutputController::Instance();

    // ONE frame revision per tick, however many counters moved: every tile's image URL embeds it, so
    // each bump is a new image request per tile.
    bool frameMoved = false;
    if (ctrl.PreviewFrames() != previewFrames_) {
        previewFrames_ = ctrl.PreviewFrames();
        frameMoved = true;
    }
    const auto bumpFrameRev = [this, &frameMoved]() {
        if (!frameMoved)
            return;
        frameMoved = false;
        ++frameRev_;
        emit frameRevChanged();
    };

    if (!live_) {
        bumpFrameRev();
        return;
    }

    pollTickRecheckNdi();

    if (ctrl.IsLiveContentReplacementPending()) {
        bumpFrameRev();
        return;
    }

    sampleFeedHealth();

    EngineBridge::instance().refreshNdiConnections();

    const qulonglong sent = ctrl.FramesSent();
    if (sent != framesSent_) {
        framesSent_ = sent;
        frameMoved = true;
    }
    bumpFrameRev();

    // ---- NDI program sender ------------------------------------------------
    checkNdiFeedRate();

    auto &controller = bps::live::LiveOutputController::Instance();
    pl::Slide onAirNow;
    const bool haveOnAir = controller.OnAirSlideSnapshot(onAirNow);
    const pl::Slide *slide = haveOnAir ? &onAirNow : nullptr;
    size_t snapshotIndex = 0;
    size_t snapshotTotal = 0;
    (void)controller.OnAirPositionSnapshot(snapshotIndex, snapshotTotal);
    const QString title = slide ? QString::fromStdString(slide->title) : QString();
    const int idx = static_cast<int>(snapshotIndex);
    const int total = static_cast<int>(snapshotTotal);
    if (title != onAirTitle_ || idx != onAirIndex_ || total != onAirTotal_) {
        onAirTitle_ = title;
        onAirIndex_ = idx;
        onAirTotal_ = total;
        refreshOnAirSlide();
        emit onAirChanged();
    } else {
        const QVariantMap next = slide
            ? slideToVariantMap(slide, pl::PresentationEngine::Instance().ActiveOutputStyle())
            : QVariantMap{};
        const QVariantMap nextRaw = slide ? slideToVariantMap(slide, pl::OutputStyleSpec{})
                                          : QVariantMap{};
        if (next != onAirSlide_ || nextRaw != onAirSlideRaw_) {
            onAirSlide_ = next;
            onAirSlideRaw_ = nextRaw;
            emit onAirChanged();
        }
    }

    refreshBusFeeds();
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
QImage shrinkForPreview(const QImage &src, const QSize &target, Qt::AspectRatioMode mode)
{
    if (src.isNull() || target.width() < 1 || target.height() < 1)
        return src;
    const QSize dst = mode == Qt::IgnoreAspectRatio ? target : src.size().scaled(target, mode);
    if (dst.width() < 1 || dst.height() < 1)
        return src;
    const int stepX = src.width() / dst.width();
    const int stepY = src.height() / dst.height();
    if ((stepX < 2 && stepY < 2) || (src.format() != QImage::Format_RGBA8888
                                      && src.format() != QImage::Format_RGBA8888_Premultiplied
                                      && src.format() != QImage::Format_ARGB32
                                      && src.format() != QImage::Format_ARGB32_Premultiplied
                                      && src.format() != QImage::Format_RGB32))
        return src.scaled(target, mode, Qt::SmoothTransformation);
    const bool rgbaOrder = src.format() == QImage::Format_RGBA8888
                           || src.format() == QImage::Format_RGBA8888_Premultiplied;
    const bool premult = src.format() == QImage::Format_RGBA8888_Premultiplied
                         || src.format() == QImage::Format_ARGB32_Premultiplied;
    constexpr int kGrid = 4;   // samples per axis inside each destination pixel's footprint
    QImage out(dst, QImage::Format_RGBA8888);
    const double fx = static_cast<double>(src.width()) / dst.width();
    const double fy = static_cast<double>(src.height()) / dst.height();
    // Source columns for one destination column, computed once for the whole image.
    std::vector<int> cols(static_cast<size_t>(dst.width()) * kGrid);
    for (int x = 0; x < dst.width(); ++x)
        for (int k = 0; k < kGrid; ++k)
            cols[static_cast<size_t>(x) * kGrid + k] =
                std::min(src.width() - 1, static_cast<int>((x + (k + 0.5) / kGrid) * fx));
    for (int y = 0; y < dst.height(); ++y) {
        uchar *o = out.scanLine(y);
        int rows[kGrid];
        for (int k = 0; k < kGrid; ++k)
            rows[k] = std::min(src.height() - 1, static_cast<int>((y + (k + 0.5) / kGrid) * fy));
        for (int x = 0; x < dst.width(); ++x) {
            unsigned sr = 0, sg = 0, sb = 0, sa = 0;
            for (int ky = 0; ky < kGrid; ++ky) {
                const uchar *row = src.constScanLine(rows[ky]);
                for (int kx = 0; kx < kGrid; ++kx) {
                    const uchar *p = row + cols[static_cast<size_t>(x) * kGrid + kx] * 4;
                    // Bytes in memory: RGBA8888 -> R G B A; ARGB32/RGB32 (little endian) -> B G R A.
                    const unsigned r = rgbaOrder ? p[0] : p[2];
                    const unsigned g = p[1];
                    const unsigned b = rgbaOrder ? p[2] : p[0];
                    const unsigned a = src.format() == QImage::Format_RGB32 ? 255u : p[3];
                    if (premult) { sr += r; sg += g; sb += b; }
                    else { sr += r * a; sg += g * a; sb += b * a; }
                    sa += a;
                }
            }
            uchar *d = o + x * 4;
            if (sa == 0) {
                d[0] = d[1] = d[2] = d[3] = 0;
            } else if (premult) {
                // premultiplied sums -> straight: divide by the alpha average
                d[0] = static_cast<uchar>(std::min<unsigned>(255, sr * 255 / sa));
                d[1] = static_cast<uchar>(std::min<unsigned>(255, sg * 255 / sa));
                d[2] = static_cast<uchar>(std::min<unsigned>(255, sb * 255 / sa));
                d[3] = static_cast<uchar>(sa / (kGrid * kGrid));
            } else {
                d[0] = static_cast<uchar>(sr / sa);
                d[1] = static_cast<uchar>(sg / sa);
                d[2] = static_cast<uchar>(sb / sa);
                d[3] = static_cast<uchar>(sa / (kGrid * kGrid));
            }
        }
    }
    return out;
}

QImage LivePreviewProvider::requestImage(const QString &id, QSize *size,
                                         const QSize &requestedSize)
{
    QString name = id;
    const int q = name.indexOf(QLatin1Char('?'));
    if (q >= 0)
        name.truncate(q);
    QImage out;

    const std::string bufferName = name.isEmpty()
        ? std::string("__live_preview__")
        : name.toStdString();
    if (auto o = pr::RenderEngine::Instance().GetOutput(bufferName); o.ok()) {
        if (auto *fb = dynamic_cast<pr::FrameBufferOutput *>(o.value().get())) {
            const auto frame = fb->LastFrameSnapshot();
            if (frame && !frame->empty()) {
                // ZERO COPY: the image VIEWS the engine's immutable snapshot (the cleanup hands the
                // shared snapshot back when the consumer is done), not an 8 MB copy per tile per frame.
                auto *holder = new std::shared_ptr<const pr::Frame>(frame);
                out = QImage(reinterpret_cast<const uchar *>(frame->pixels.data()),
                             frame->width, frame->height, frame->width * 4,
                             QImage::Format_RGBA8888,
                             [](void *p) { delete static_cast<std::shared_ptr<const pr::Frame> *>(p); },
                             holder);
            }
        }
    }

    // THE SANITY LINE IS A ONCE-A-SECOND PROBE, not a per-request scan: every tile asks for a frame every
    // revision, and the 16x16 luma sweep plus two std::format strings ran for each of those requests.
    static std::mutex scanMutex;
    static std::unordered_map<std::string, qint64> lastScanMs;
    bool scanNow = false;
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        std::lock_guard<std::mutex> lock(scanMutex);
        qint64 &last = lastScanMs[bufferName];
        if (nowMs - last >= 1000) {
            last = nowMs;
            scanNow = true;
        }
    }
    if (scanNow) {
        // The preview is re-requested every frame, so this line is gated on the frame's
        // SHAPE (size, black or not). Mean luma moves by a rounding step on a still
        // picture and made it a per-frame writer; the numbers still print on a change.
        static bps::logging::ChangeGate sanityGate;
        std::string state;
        std::string signature;
        if (out.isNull()) {
            state = "no frame";
            signature = state;
        } else {
            long long sum = 0;
            int count = 0;
            int maxLuma = 0;
            int maxAlpha = 0;
            const int stepY = std::max(1, out.height() / 16);
            const int stepX = std::max(1, out.width() / 16);
            for (int y = 0; y < out.height(); y += stepY) {
                const unsigned char *row = out.constScanLine(y);
                for (int x = 0; x < out.width(); x += stepX) {
                    const unsigned char *px = row + x * 4;   // RGBA8888
                    const int luma = (px[0] * 299 + px[1] * 587 + px[2] * 114) / 1000;
                    sum += luma;
                    ++count;
                    if (luma > maxLuma) maxLuma = luma;
                    if (px[3] > maxAlpha) maxAlpha = px[3];
                }
            }
            const double mean = count > 0 ? static_cast<double>(sum) / count : 0.0;
            const char *black = maxLuma == 0 ? "yes" : "no";
            state = std::format("{}x{} meanLuma={:.1f} maxLuma={} maxAlpha={} allBlack={}",
                                out.width(), out.height(), mean, maxLuma, maxAlpha, black);
            signature = std::format("{}x{} allBlack={}", out.width(), out.height(), black);
        }
        if (sanityGate.Changed(bufferName, signature))
            qInfo("livepreview frame sanity: buffer '%s' — %s", bufferName.c_str(), state.c_str());
    }

    if (out.isNull()) {
        out = QImage(1, 1, QImage::Format_ARGB32);
        out.fill(Qt::transparent);
    }

    // Downscale only, and only to what the tile asked for (QML passes the tile's size as sourceSize): a
    // full 1920x1080 ARGB per tile was uploaded to the GPU for a 240-pixel preview. This runs on the image
    // loader thread now (the tiles' Images are asynchronous), not the interface thread.
    if (requestedSize.width() > 0 && requestedSize.height() > 0
        && (requestedSize.width() < out.width() || requestedSize.height() < out.height()))
        out = shrinkForPreview(out, requestedSize, Qt::KeepAspectRatio);
    if (size)
        *size = out.size();
    return out;
}