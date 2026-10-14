#include "services/CallTrace.h"
#include "services/LiveOutputService.h"

#include "services/ShowConverter.h"
#include "services/StyleSpecBuilder.h"
#include "services/DesignLibraryService.h"

#include <QSet>
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "models/OutputListModel.h"

#include "modules/display/DisplayEngine.hpp"
#include "modules/display/Providers.hpp"
#include "modules/broadcast/BroadcastEngine.hpp"

#include <QCoreApplication>
#include <QTimer>
#include <QThread>
#include <QAudioOutput>
#include <QFile>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// NDI progress notes are NOT toasts.
//
// main.cpp's message handler turns every qWarning into a UI toast via
// EventBus. That is right for real problems, and wrong for the NDI feed's
// own narration: "sending started", "frames flowing", "2 monitor(s)
// connected", "waiting for the first frame" — five toasts on every GO LIVE,
// none of them actionable, all of them covering the screen the operator is
// trying to work in.
//
// They still belong in the ENGINE LOG (that is the file you read when NDI
// misbehaves), so they keep their level and their text — they just stop
// being published as events. qCWarning with a category makes that a property
// of the call site instead of a list of string prefixes to maintain in
// main.cpp: the handler recognises the category and skips the publish.
//
// Everything that IS actionable keeps qWarning and therefore still toasts:
// "NDI SendFrame FAILED", "NDI is not usable", "media player error",
// "file not found", every selftest FAIL line.
// ---------------------------------------------------------------------------
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcNdiProgress, "vgr.ndi.progress");

#include <utility>

namespace pl = bps::presentation;
namespace pr = bps::rendering;

namespace {

// Pushes lastMediaFrame_ (a QImage this service already owns/decodes — a
// real QVideoSink frame or a loaded still) into the engine-side
// CompositorState as plain RGBA8 bytes, so SceneBuilder's worker thread can
// composite it too, not just the QML preview tile's image provider. A null
// image clears the engine's copy instead of pushing empty bytes.
void pushMediaFrameToCompositor(const QImage &frame)
{
    // ONE-SHOT TRANSITION LOG, never per frame (a video hands a frame in at its
    // own frame rate — a per-frame line would bury engine.log). A media layer
    // that renders black is either a frame the ENGINE never received or one it
    // received and mis-drew; this line, together with the engine-side
    // "media layer" line SceneBuilder writes when it builds the scene, is what
    // tells the two apart in ONE run. Both greppable by "media hand-off".
    static bool hadFrame = false;
    if (frame.isNull()) {
        pl::CompositorState::Instance().ClearMedia();
        if (hadFrame) {
            hadFrame = false;
            qInfo("media hand-off: cleared (no frame) — engine media layer will be blank");
        }
        return;
    }
    // Format_RGBA8888 is byte order R,G,B,A on every platform — exactly the
    // engine's own RGBA8 convention (see LivePreviewProvider::requestImage's
    // comment on the reverse conversion).
    const QImage rgba = frame.format() == QImage::Format_RGBA8888
        ? frame : frame.convertToFormat(QImage::Format_RGBA8888);
    pl::CompositorState::MediaFrame mf;
    mf.width = static_cast<uint32_t>(rgba.width());
    mf.height = static_cast<uint32_t>(rgba.height());
    // Row-by-row: QImage's bytesPerLine() can pad past width*4, so a
    // straight constBits()..sizeInBytes() copy would smear padding bytes
    // into a "tightly packed" buffer CompositorState's readers assume.
    mf.rgba.resize(static_cast<size_t>(mf.width) * mf.height * 4);
    for (int y = 0; y < rgba.height(); ++y) {
        std::memcpy(mf.rgba.data() + static_cast<size_t>(y) * mf.width * 4,
                    rgba.constScanLine(y), static_cast<size_t>(mf.width) * 4);
    }
    pl::CompositorState::Instance().SetMediaFrame(std::move(mf));
    if (!hadFrame) {
        hadFrame = true;
        qInfo("media hand-off: %dx%d (%zu bytes RGBA) sent to the engine",
              rgba.width(), rgba.height(), mf.rgba.size());
    }
}

// Resolves the current activeOverlays_ list ({id,name} pairs — see
// LiveOutputService::takeOverlay) into engine-ready CompositorState
// snapshots and pushes them, called after every mutation
// (takeOverlay/clearOverlay/clearAllOverlays) so SceneBuilder's worker
// thread always sees the SAME overlay stack the QML tile's own Repeaters
// composite. Resolution (OverlayLibraryService::design + block conversion)
// happens HERE, on the GUI thread that owns those QML services — the
// engine-side reader never calls back into them, only ever reads the
// already-resolved plain data CompositorState hands it.
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
                        // "stale id self-heals" convention taken/preview ids use
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

// THE PREVIEW READS THE ENGINE'S OWN RULES, it does not keep copies of them.
//
// This file used to carry its own slideContentType / familyTemplateIndexFor /
// styleAdmits / slideContentText — near-exact mirrors of SceneBuilder's, with
// comments admitting as much. Two copies of the rule that decides a slide's
// family, its style slot, its bound text and whether a style's gates admit it
// is exactly how a tile comes to paint one thing and an output another: the
// preview and the builder now call ONE implementation
// (modules/presentation/SlideRules). Nothing here may re-derive them.
//
// The STYLE is the other half of that promise. This file also spelled out
// "style row -> the engine's OutputStyleSpec" for itself (bakePreviewTemplate
// plus its own field list and inherit/fallback pass), beside two copies of the
// same composition in OutputListModel. The three had drifted: the preview's
// spec never carried the style's category and fell back to the Bible layout on
// a different condition than the engine's did, so a tile could render a layout
// the outputs were not rendering. That composition is now ONE function
// (services/StyleSpecBuilder), called by the spec pushed for the active output,
// by each other styled output's spec, and here.
//
// What this file still owns is the PREVIEW-SHAPED plumbing around those rules:
// composing the block list the tile draws (ShowConverter's mapping) and asking
// the engine which layout owns a slide (SceneBuilder::ComposeSlide below).

pl::OutputStyleSpec outputStyleForPreview(const QString &styleId)
{
    return StyleSpecBuilder::specForStyleId(styleId);
}

// One slide as the QML preview shape: { valid, title, blocks, background } —
// the same { blocks, background } maps DesignPreview draws everywhere else
// (ShowConverter::blockToVariant is the single QML<->engine block mapping).
//
// THE COMPOSITION IS THE ENGINE'S. Which layout owns this slide (the style's
// per-family template, the whole-style one, a built-in preset, the slide's own
// blocks), what the template blocks bound to, and what background the output
// paints are all decided ONCE, by SceneBuilder::ComposeSlide — the same call
// every real output's scene is built from — and this function only maps that
// decision into the QML shapes the tile draws. It used to make these decisions
// itself, in a mirror of SceneBuilder's branch chain: the two drifted, and a
// preview that reads a slide differently from the frame going on air is exactly
// the "it shows on the monitor but the output is black" class of report.
QVariantMap slideToVariantMap(const pl::Slide *slide, const pl::OutputStyleSpec &style)
{
    if (!slide)
        return {};
    const pl::SceneBuilder::SlideComposition comp =
        pl::SceneBuilder::ComposeSlide(*slide, style, /*applyGates=*/true);

    // ---- THE STYLE'S OWN FACTS, answered here so nobody else has to ---------
    // A surface that draws this output needs more than the slide's picture: it
    // needs to know whether this STYLE paints anything of its own (so it can
    // leave a transparent/empty frame to its own convention — the checkerboard
    // or a keyable window), whether its background image exists on disk AND is
    // not stepped aside for this slide, and whether the style CHOSE
    // transparency (which reads the same in pixels as naming nothing at all).
    //
    // These are the answers the monitor tile and the output window each used to
    // work out for themselves, from the model's style role plus their own copy of
    // `isClearColor` / `styled` / the clear-on-text image gate — two surfaces, two
    // derivations, and one of them (the image gate) already disagreed with the
    // engine about what "content on air" means. They come from the ONE call that
    // composes the frame the outputs consume, so a preview cannot answer
    // differently from the frame: same spec, same composition, same source.
    const QString styleImage = QString::fromStdString(style.backgroundImage);
    const bool styleImageOnDisk = !styleImage.isEmpty() && QFile::exists(styleImage);
    // A colour that really paints: "" names none, "transparent" (ParseColor
    // alpha 0) is the keyable choice, and a 0-alpha colour paints nothing.
    const bool stylePaintsColor = !style.backgroundColor.empty()
        && pl::StyleBuilder::ParseColor(style.backgroundColor).a > 0.0f;
    const bool stylePaints = stylePaintsColor || styleImageOnDisk;
    // The frame has nothing of the style's OWN in it — either the style paints
    // nothing, or its only paint (the image) is stepped aside for this slide. A
    // surface bases its own transparency on this.
    const bool stylePaintless = !stylePaints
        || (styleImageOnDisk && comp.styleImageSuppressed);
    const bool styleImagePainted = styleImageOnDisk && !comp.styleImageSuppressed;
    // WOULD A BLOCK RENDER OF THIS FRAME BE A STAND-IN? The camera / media /
    // audio / image decision rides the composition too, so it is answered here
    // once instead of by each surface for itself: the monitor tile and the
    // output window each tested their own composed blocks for those kinds and
    // handed the frame priority when they found one. Same question, two copies —
    // the shape of rule this whole file is about. A REFUSED frame carries no
    // blocks at all, and a preset's are its two text regions, so neither can
    // want the frame.
    const std::vector<pl::ContentBlock> &drawn =
        comp.source == pl::SceneBuilder::SlideComposition::Source::SlideBlocks
            ? slide->blocks : comp.blocks;
    const bool framePriority = pl::SlideRules::NeedsDistributedFrame(drawn);
    const QVariantMap styleFacts{
        { QStringLiteral("styleColor"), QString::fromStdString(style.backgroundColor) },
        { QStringLiteral("styleImage"), styleImage },
        { QStringLiteral("stylePaints"), stylePaints },
        { QStringLiteral("stylePaintless"), stylePaintless },
        { QStringLiteral("styleImagePainted"), styleImagePainted },
        // The distributed frame must win over a block render for this slide —
        // see SlideRules::NeedsDistributedFrame. Both preview surfaces read this
        // one answer (gated on their own `hasFrame`: whether a frame exists at
        // all is the service's state, not the slide's).
        { QStringLiteral("framePriority"), framePriority },
        // The style CHOSE "transparent" (as opposed to naming no colour): a
        // surface that shows transparency as transparency needs the difference,
        // and only the composition has it.
        { QStringLiteral("explicitTransparent"), comp.explicitTransparent },
    };

    // The background, as a QML string: an EXPLICITLY transparent style is
    // "transparent" (the tile's checkerboard belongs there), a colour that won
    // is that colour, and NOTHING NAMED is "" — the tile's own layering (its
    // style colour, its style image, the checkerboard) decides what an unpainted
    // frame looks like, exactly as the engine leaves such a frame unpainted.
    const QString background = comp.explicitTransparent
        ? QStringLiteral("transparent")
        : QString::fromStdString(comp.background);

    if (comp.source == pl::SceneBuilder::SlideComposition::Source::Refused) {
        QVariantMap refused = styleFacts;
        refused.insert(QStringLiteral("valid"), true);
        refused.insert(QStringLiteral("title"), QString());
        refused.insert(QStringLiteral("contentType"), QString::fromStdString(comp.contentType));
        refused.insert(QStringLiteral("blocks"), QVariantList{});
        refused.insert(QStringLiteral("background"), background);
        // The style image rule still applies to a refused frame (the engine
        // paints it on the same flag — see the note on the styled return below).
        refused.insert(QStringLiteral("suppressStyleImage"), comp.styleImageSuppressed);
        // WHY THIS IS EMPTY, not just that it is: the monitor tile reads this to
        // say "Style blocks Media" on the tile itself, so a deliberately refused
        // output can never be mistaken for a broken one — the exact confusion a
        // background-only frame invites.
        refused.insert(QStringLiteral("refusedByStyle"), true);
        return refused;
    }

    QVariantList blocks;
    using Source = pl::SceneBuilder::SlideComposition::Source;
    if (comp.source == Source::FamilyTemplate || comp.source == Source::StyleTemplate) {
        // The style's template, with the slide's content already bound in.
        blocks = ShowConverter::blocksToVariant(comp.blocks);
    } else if (comp.source == Source::FamilyPreset || comp.source == Source::StylePreset) {
        // A BUILT-IN LAYOUT PRESET: the engine draws its two text regions as
        // Title/Body text objects on these rectangles, so the tile draws the
        // same two regions — synthesized as text blocks on the canvas grid the
        // preview works in (754x428), from the preset's own fractions.
        // The text style is the engine's default for a preset region (no
        // TextStyleComponent on those objects): the builtin font, 24 OUTPUT
        // pixels — which on the canvas grid is 24 * 428 / 1080, unglamorous
        // but what a legacy preset actually renders on air, and the tile says
        // what the output says.
        const double kCanvasW = 754.0, kCanvasH = 428.0, kEngineStageH = 1080.0;
        auto presetRegion = [&](const pl::StyleBuilder::LayoutRect &r, const QString &text) {
            QVariantMap region{
                { QStringLiteral("key"), QString() },
                { QStringLiteral("kind"), QStringLiteral("text") },
                { QStringLiteral("text"), text },
                { QStringLiteral("x"), r.x * kCanvasW },
                { QStringLiteral("y"), r.y * kCanvasH },
                { QStringLiteral("width"), r.w * kCanvasW },
                { QStringLiteral("height"), r.h * kCanvasH },
                { QStringLiteral("bind"), QString() },
                { QStringLiteral("meta"), QVariantMap{
                    { QStringLiteral("fontSize"), 24.0 * kCanvasH / kEngineStageH },
                    { QStringLiteral("color"), QStringLiteral("#ffffff") },
                    { QStringLiteral("align"), QStringLiteral("left") },
                    { QStringLiteral("verticalAlign"), QStringLiteral("top") },
                } },
                { QStringLiteral("style"), QVariantMap{
                    { QStringLiteral("backgroundColor"), QStringLiteral("transparent") },
                } },
            };
            blocks.append(region);
        };
        if (!slide->title.empty() && comp.preset.showTitle)
            presetRegion(comp.preset.title, QString::fromStdString(slide->title));
        if (!slide->text.empty())
            presetRegion(comp.preset.body, QString::fromStdString(slide->text));
    } else {
        // Source::SlideBlocks — the slide's own blocks carry the layout.
        blocks = ShowConverter::blocksToVariant(slide->blocks);
    }

    QVariantMap composed = styleFacts;
    composed.insert(QStringLiteral("valid"), true);
    composed.insert(QStringLiteral("title"), QString::fromStdString(slide->title));
    composed.insert(QStringLiteral("contentType"), QString::fromStdString(comp.contentType));
    composed.insert(QStringLiteral("blocks"), blocks);
    composed.insert(QStringLiteral("background"), background);
    // DOES THE STYLE'S OWN BACKGROUND IMAGE STEP ASIDE FOR THIS SLIDE?
    // The engine's rule (clearBackgroundOnText, and only for a slide that
    // CARRIES content — a blank go-live carrier keeps the branded backdrop),
    // handed over so the monitor tile and the output window paint the style
    // image exactly when the engine does. Both surfaces used to re-derive it
    // from their own idea of "has content", which is how a blank carrier
    // lost the image on the tile while the output still baked it.
    composed.insert(QStringLiteral("suppressStyleImage"), comp.styleImageSuppressed);
    // ...AND THE FACTS A PREVIEW RE-ENTERS WITH. Every preview surface hands this
    // map straight back through previewWithStyle() -> ShowConverter::slideFromVariant(),
    // so the slide's body and its fill vocabulary have to be IN it. They were
    // not: the monitor tile re-composed a slide whose body was gone and fell back
    // to guessing it from the design's block ORDER, which drew the date code where
    // the sermon text belonged — while the real NDI/HDMI frame, built from the
    // engine's own Slide (which still had them), drew it correctly. Three
    // surfaces, two answers, from one pick: the tile disagreed with the output it
    // monitors. Same list as the fill's own writer, so the spelling cannot drift.
    composed.insert(QStringLiteral("body"), QString::fromStdString(slide->text));
    composed.insert(QStringLiteral("fill"),
                    ShowConverter::fillFromMeta(ShowConverter::metaOf(*slide)));
    return composed;
}

QVariantMap slideToVariantMap(const pl::Slide *slide)
{
    return slideToVariantMap(slide, pl::PresentationEngine::Instance().ActiveOutputStyle());
}

} // namespace

LiveOutputService *LiveOutputService::s_instance = nullptr;

// ---- NDI send worker (runs on the dedicated NDI QThread) ------------------
//
// Everything heavy about the NDI program feed happens here, OFF the GUI
    // thread: the ~8 MB frame copy into the provider's send buffer, the
// force-alpha pass over every pixel, the LAZY SENDER CREATION (the first
// SendFrame after a reset builds the real NDI SDK sender — the single
// heaviest step of GO LIVE with an NDI main output, hundreds of ms), and
// the SDK send itself. The GUI thread only resolves WHICH frame to send
// (roster lookup + buffer read) and queues it here.
//
// FEED THREAD ONLY (ndiWorkerTick is its only caller). One send at a time by
// construction: the feed thread's clock is the only thing that runs this, and
// a beat that lands inside the previous period is skipped before it gets here.
// State feedback (frame counter, slow-send reports) crosses back through
// LiveOutputService's mutex-guarded members, which the GUI reads.
void LiveOutputService::ndiWorkerSendFrame(void *providerVoid,
                                           std::shared_ptr<const pr::Frame> frame,
                                           QString senderName, float fps)
{
    auto *provider = static_cast<bps::display::NdiDisplayProvider *>(providerVoid);
    auto &svc = LiveOutputService::instance();
    if (!provider || !frame || frame->empty())
        return;

    // Identity BEFORE the send: SetSenderName only takes effect before the
    // sender's first SendFrame (NDI can't rename a live sender), and the
    // lazy creation below happens inside this very call — so the name set
    // here is the one the new sender is born with. THE LAZY CREATION (the
    // real NDI SDK sender build) is the multi-hundred-ms step that used to
    // freeze the GUI at GO LIVE — it happens HERE now.
    if (!senderName.isEmpty())
        provider->SetSenderName(senderName.toStdString());
    provider->SetFrameRate(fps);

    QElapsedTimer clock;
    clock.start();
    bps::display::RenderFrameView view;
    view.width = frame->width;
    view.height = frame->height;
    view.pixels = frame->pixels.data();
    const bool sent = provider->SendFrame(view).ok();
    const qint64 sendMs = clock.elapsed();

    // Feedback to the GUI side (mutex-guarded members; the GUI reads them
    // for the pill and its own rate watch).
    std::lock_guard<std::mutex> lock(svc.ndiStateMutex_);
    if (sent) {
        svc.ndiFramesSent_ = provider->FramesSent();
        if (!svc.ndiFirstFrameLogged_) {
            svc.ndiFirstFrameLogged_ = true;
            qCWarning(lcNdiProgress, "LiveOutputService: NDI frames flowing as '%s' — discovery on a receiving monitor can take 10-30s",
                     provider->SenderName().c_str());
        }
        svc.ndiSendFailedLogged_ = false;
        // Slow-send report. The feed's own pacing IS the backpressure (a send
        // slower than the period means the beats it overruns are skipped, on
        // the feed thread — the advertised rate never changes), so this only
        // has to say that the machine cannot hold the configured rate.
        if (sendMs > qMax(50, fps > 0.0f ? int(1000.0f / fps) : 100)) {
            if (!svc.ndiSlowLogged_) {
                svc.ndiSlowLogged_ = true;
                svc.ndiFastStreak_ = 0;
                qCWarning(lcNdiProgress, "LiveOutputService: NDI send is slow (%lld ms, sender '%s') — the feed skips the beats it overruns; check the network and the configured rate",
                         sendMs, provider->SenderName().c_str());
            }
        } else if (++svc.ndiFastStreak_ >= 30) {
            svc.ndiSlowLogged_ = false;   // sustained recovery re-arms the incident log
        }
    } else if (!svc.ndiSendFailedLogged_) {
        svc.ndiSendFailedLogged_ = true;
        qWarning("LiveOutputService: NDI SendFrame FAILED (sender '%s') — check the engine log for the provider's own reason",
                 provider->SenderName().c_str());
    }
}

// One beat of the feed — FEED THREAD ONLY (the worker's own timer calls this,
// and everything it touches is either thread-local or mutex-guarded).
//
// This is where the feed's cadence is decided, and the whole point of the split
// above: nothing the GUI thread does can move it. A tick that arrives inside the
// previous interval (a send slower than the period, or the timer coalescing
// after a stall) is skipped rather than fired late-and-again — a two-frame burst
// is exactly what a receiver counts as dropped frames.
void LiveOutputService::ndiWorkerTick()
{
    NdiSendPlan plan;
    {
        std::lock_guard<std::mutex> lock(ndiPlanMutex_);
        plan = ndiPlan_;
    }
    // Nothing to feed (stopped, no enabled NDI row, the output gone): sleep the
    // clock rather than tick into an empty plan 30-60x/s, and drop the pacing
    // clock with it — the next feed is a NEW session and must not inherit this
    // one's gap between sends.
    if (!plan.want || !plan.provider || !plan.buffer) {
        if (ndiSendTimer_ && ndiSendTimer_->isActive())
            ndiSendTimer_->stop();
        ndiSendClock_.invalidate();
        return;
    }
    const float fps = plan.fps > 0.0f ? plan.fps : 30.0f;
    const int periodMs = qMax(1, int(std::llround(1000.0 / double(fps))));
    // The rate the plan asks for is followed HERE, so a Settings · Outputs
    // change reaches the wire within a poll (the plan republishes at 10 Hz)
    // without the GUI thread ever touching this timer.
    if (ndiSendTimer_ && ndiSendTimer_->interval() != periodMs)
        ndiSendTimer_->setInterval(periodMs);
    if (ndiSendClock_.isValid() && ndiSendClock_.elapsed() < qint64(periodMs * 0.9))
        return;                       // inside the previous period: drop this beat

    const auto frame = plan.buffer->LastFrameSnapshot();
    if (!frame || frame->empty())
        return;                       // the live loop has not rendered one yet
    ndiSendClock_.restart();
    ndiWorkerSendFrame(static_cast<void *>(plan.provider), frame, plan.senderName, fps);
}

// The three NDI property readers — the worker writes the state, the GUI
// (QML bindings + pollTick) reads it. One small mutex, never held across
// the send itself.
bool LiveOutputService::ndiSending() const
{
    std::lock_guard<std::mutex> lock(ndiStateMutex_);
    return ndiSending_;
}
QString LiveOutputService::ndiSendingOutputName() const
{
    std::lock_guard<std::mutex> lock(ndiStateMutex_);
    return ndiSendingOutput_;
}
qulonglong LiveOutputService::ndiFramesSent() const
{
    std::lock_guard<std::mutex> lock(ndiStateMutex_);
    return ndiFramesSent_;
}
bool LiveOutputService::ndiWarming() const
{
    std::lock_guard<std::mutex> lock(ndiStateMutex_);
    return ndiWarming_;
}
QString LiveOutputService::ndiWarmingOutputName() const
{
    std::lock_guard<std::mutex> lock(ndiStateMutex_);
    return ndiWarmingOutput_;
}
void LiveOutputService::setNdiWarming(bool on, const QString &name)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(ndiStateMutex_);
        if (ndiWarming_ != on || (on && ndiWarmingOutput_ != name)) {
            ndiWarming_ = on;
            ndiWarmingOutput_ = on ? name : QString();
            changed = true;
        }
    }
    if (changed)
        emit ndiChanged();
}

LiveOutputService::LiveOutputService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;

    // IDLE PREVIEW: while a show is open but nothing is on air, ask the engine
    // to keep producing frames (LiveOutputController::StartIdlePreview) so the
    // Settings resource meters breathe instead of reading zero. Idempotent and
    // a no-op while live, so a plain 1Hz nudge is all it takes — the controller
    // owns the worker's lifetime (and stops it seconds after no show is open).
    previewTimer_ = new QTimer(this);
    previewTimer_->setInterval(1000);
    connect(previewTimer_, &QTimer::timeout, this, []() {
        if (EngineBridge::instance().booted())
            (void)bps::live::LiveOutputController::Instance().StartIdlePreview();
    });
    previewTimer_->start();
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
    // CppOwnership: QML must not GC the singleton the engine-side loop
    // signals into (same contract as TelemetryService).
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

// ---------------------------------------------------------------------------
// Go live / stop
// ---------------------------------------------------------------------------
void LiveOutputService::goLive()
{
    // Breadcrumb: GO LIVE is the heaviest QML-facing call in the app (compile +
    // prepare + output bring-up), so a stall or exception here names itself in
    // engine.log instead of leaving a heartbeat line as the last word.
    VGR_CALL_TRACE("LiveOutputService.goLive");
    // A staged ad-hoc pick (stageSlides() — a slide double-clicked in
    // ShowCenter, say) wins over the open document: what is sitting in the
    // tile right now is what a real GO LIVE press sends, not whatever happens
    // to be open in the editor behind it. It stays staged afterwards (see the
    // branch below) — GO LIVE decides only what leaves the outputs.
    if (!stagedSlidesRaw_.isEmpty()) {
        // SEND IT, DON'T CONSUME IT. GO LIVE is the OUTPUT gate: it puts the
        // picture the preview is ALREADY showing on the outputs and decides
        // nothing about the preview. Clearing the staged slot here (what this
        // used to do) dropped the slide the tile was showing and sent the next
        // GO LIVE down to the blank holding slide — content the operator never
        // asked for, and the reason a GO LIVE / STOP pair looked like it was
        // committing and discarding picks. The stage belongs to the preview;
        // only Clear all, the slide-clear button, or a new pick change it.
        goLiveWithSlides(stagedName_, stagedSlidesRaw_);
        return;
    }

    // Transition toast BEFORE the (synchronous, GUI-thread) engine start:
    // the first frame is the heaviest (style load + scene build + full-
    // frame rasterize), so the click must feel acknowledged immediately —
    // and a fast STOP press right after is safe by design now.
    EventBus::instance().notify(QStringLiteral("Going live…"),
                                QStringLiteral("info"), QStringLiteral("Go Live"));
    auto r = bps::live::LiveOutputController::Instance().StartFromOpenShow();
    if (!r.ok()) {
        // No open show to mirror — but the preview is still showing something:
        // the COMPOSITOR's own layers (a taken camera/screen, media on air, any
        // overlays) over the active style's background. Those layers hang off
        // the scene, so they only reach the outputs with a slide under them;
        // sending the ONE blank carrier slide transmits exactly what the
        // preview shows and nothing more.
        //
        // This is deliberately NOT the old "blank holding screen" decision: the
        // carrier has no content of its own (no blocks, no title, transparent),
        // so GO LIVE is not committing or inventing a pick, and it does not
        // touch the preview — a staged slide stays staged, the tile keeps its
        // pixels, and the next GO LIVE re-sends the same thing. The style's own
        // background paints over it, because SceneBuilder composites the active
        // style unconditionally.
        goLiveBlank();
        return;
    }

    // This live session's NDI sender opens under the FIRST enabled NDI
    // output's own name (recreated when the name changed since last time —
    // NDI can't rename a live sender; see armNdiSenderForSession).
    armNdiSenderForSession();

    if (!live_) {
        live_ = true;
        emit liveChanged();
        refreshOnAirSlide();
        emit onAirChanged();
        if (!poll_) {
            poll_ = new QTimer(this);
            poll_->setInterval(100);   // 10Hz — slide titles AND a live-looking preview
            connect(poll_, &QTimer::timeout, this, &LiveOutputService::pollTick);
        }
        poll_->start();
        startNdiSendClock();   // the NDI feed's own clock, paced by the output's refresh rate
        pollTick();
    }
}

// Any-content go-live: convert the QML slide maps to engine Slides and hand
// them to the controller. A failed start surfaces through the same qWarning +
// onAirChanged path as goLive (the UI toasts off the failure reason).
void LiveOutputService::goLiveWithSlides(const QString &name, const QVariantList &slides)
{
    VGR_CALL_TRACE("LiveOutputService.goLiveWithSlides");
    bps::presentation::Presentation content;
    content.name = name.toStdString();
    for (const QVariant &v : slides) {
        const QVariantMap raw = v.toMap();
        auto slide = ShowConverter::slideFromVariant(raw);
        // ONE-SHOT DIAGNOSTIC (this log is the boot/behaviour source of truth):
        // "the outputs draw the code where the text belongs" can only be argued
        // from the DATA the engine was handed — whether the fill's body and its
        // whole vocabulary actually arrived, or the scene builder is once again
        // inferring something. One line per slide, per go-live.
        qInfo("live slide: title='%s' rawHasBody=%d bodyChars=%d textChars=%d blocks=%d fill=%s",
              slide.title.c_str(),
              raw.contains(QStringLiteral("body")) ? 1 : 0,
              static_cast<int>(raw.value(QStringLiteral("body")).toString().size()),
              static_cast<int>(slide.text.size()),
              static_cast<int>(slide.blocks.size()),
              raw.value(QStringLiteral("fill")).toMap().isEmpty() ? "no" : "yes");
        content.slides.push_back(std::move(slide));
    }
    if (content.slides.empty()) {
        qWarning("LiveOutputService: goLiveWithSlides: no slides in '%s'", name.toUtf8().constData());
        EventBus::instance().notify(QStringLiteral("Nothing selected to put on air"),
                                    QStringLiteral("warning"), QStringLiteral("Go Live"));
        emit onAirChanged();
        return;
    }
    auto &controller = bps::live::LiveOutputController::Instance();
    const bool replacingLiveContent = controller.IsLive();
    auto r = controller.StartFromSlides(name.toStdString(), content.slides);
    if (!r.ok()) {
        const QString reason = QString::fromStdString(r.error().message);
        qWarning("LiveOutputService: goLiveWithSlides failed: %s", r.error().message.c_str());
        EventBus::instance().notify(reason, QStringLiteral("warning"), QStringLiteral("Go Live"));
        emit onAirChanged();
        return;
    }
    // This live session's NDI sender opens under the FIRST enabled NDI
    // output's own name (see armNdiSenderForSession).
    armNdiSenderForSession();

    if (!live_) {
        live_ = true;
        emit liveChanged();
    }
    // A live replacement is applied by the render worker. Let pollTick read
    // the new runtime state after its replacement guard clears instead of
    // racing the worker's presentation swap here on the UI thread.
    if (!replacingLiveContent) {
        refreshOnAirSlide();
        emit onAirChanged();
    }
    if (!poll_) {
        poll_ = new QTimer(this);
        poll_->setInterval(100);
        connect(poll_, &QTimer::timeout, this, &LiveOutputService::pollTick);
    }
    if (!poll_->isActive()) {
        poll_->start();
        startNdiSendClock();   // the NDI feed's own clock, paced by the output's refresh rate
        pollTick();
    }
}

// Records a pick for the Main Output tile WITHOUT going live — see the
// header comment on stagedSlide for why this exists (double-click used to
// call goLiveWithSlides directly, which no user action gated). The staged
// slide is already in the QML block shape (the same { blocks, background,
// title } maps ShowCenter/ScripturePane/TheTablePane hand to
// goLiveWithSlides), so it needs no engine round-trip to preview — only
// actually going live converts it to real engine Slides.
//
// WHILE LIVE a pick is NOT staged — it goes STRAIGHT ON AIR (the FreeShow
// model, and the reported bug: with the show live, clicking a verse only
// ever updated the tile preview while the engine kept rendering the blank
// holding slide the GO LIVE press committed — tiles showed text, the real
// output and NDI showed the style poster forever, with zero Scene-built
// lines in the log). When not live, staging stays preview, exactly as
// before.
void LiveOutputService::stageSlides(const QString &name, const QVariantList &slides)
{
    if (live_) {
        goLiveWithSlides(name, slides);   // commit the pick to the engine now
        stagedName_.clear();
        stagedSlidesRaw_.clear();
        stagedSlide_ = QVariantMap{};     // tiles must read the NEW onAirSlide,
        emit stagedChanged();             // not a stale staged copy over it
        return;
    }
    stagedName_ = name;
    stagedSlidesRaw_ = slides;
    // Through the SAME active-style template composition onAirSlide/
    // previewWithActiveStyle already apply — the staged tile used to show
    // the slide's own raw, unstyled blocks, then visibly jump to bold/
    // larger template text the instant GO LIVE actually committed it (live
    // reported: "the main output preview got bolder" after pressing GO
    // LIVE). Staging should already show what it will look like once live.
    stagedSlide_ = slides.isEmpty() ? QVariantMap{} : previewWithActiveStyle(slides.first().toMap());
    emit stagedChanged();
}

// ---- CLEAR ALL / SHOW ALL ------------------------------------------------
// The toolbar's Clear all, and its undo in one place. The button used to run
// its four clears itself in QML, which is why adding an undo would have meant
// two places to keep in step; the snapshot and the clears live together here.
void LiveOutputService::clearAll()
{
    VGR_CALL_TRACE("LiveOutputService.clearAll");
    ClearedSnapshot snap;
    // The CONTENT slot: what was actually on the monitor. Live, that is the
    // on-air presentation; off air it is the staged pick (stop() puts the
    // last on-air pick there, so this covers "I stopped, then cleared").
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
    snap.overlays = activeOverlays_;
    snap.valid = !snap.contentSlidesRaw.isEmpty() || !snap.inputLabel.isEmpty()
                 || !snap.overlays.isEmpty();
    cleared_ = snap;
    emit clearedChanged();

    // The same four clears the button ran itself, in the same order and with
    // the same gates. Note what is NOT here: live_.
    if (!inputLabel_.isEmpty())
        clearInput();
    if (!activeOverlays_.isEmpty())
        clearAllOverlays();
    // The on-air slide only means something while live (off air it already
    // sits in the preview slot, cleared as content above).
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

    // The layer order the tiles composite: the input underneath, then the
    // overlays, then the content.
    if (!snap.inputLabel.isEmpty())
        takeInput(snap.inputLabel, snap.inputKind, snap.inputMode);
    for (const QVariant &v : snap.overlays) {
        const QVariantMap o = v.toMap();
        takeOverlay(o.value(QStringLiteral("id")).toString(),
                    o.value(QStringLiteral("name")).toString());
    }
    if (snap.contentSlidesRaw.isEmpty())
        return;
    // Where it goes back depends on where it was: content that was ON AIR
    // goes straight back out (the user asked for exactly this), while content
    // that was only being previewed returns to the preview slot — a restore
    // must never be the thing that puts frames on a real screen.
    if (snap.contentWasOnAir && live_)
        goLiveWithSlides(snap.contentTitle, snap.contentSlidesRaw);
    else {
        stagedName_ = snap.contentTitle;
        stagedSlidesRaw_ = snap.contentSlidesRaw;
        stagedSlide_ = snap.contentStyled;
        emit stagedChanged();
    }
}

void LiveOutputService::clearStaged()
{
    if (stagedSlidesRaw_.isEmpty() && stagedSlide_.isEmpty())
        return;
    stagedName_.clear();
    stagedSlidesRaw_.clear();
    stagedSlide_ = QVariantMap{};
    emit stagedChanged();
}

// ---------------------------------------------------------------------------
// Taken video input — the output preview's camera/screen layer
// ---------------------------------------------------------------------------
// The monitor tiles already composite raster layers (the style's PNG/JPG
// background, the distributed frame) as plain QML Image items over the
// image providers; a taken INPUT rides the same path — its frames come
// from the PAL's videopreview taps (EngineBridge, owner "output"), the
// SAME tap class the Settings dialogs' previews use, so the device
// lifecycle (idempotent starts, owner-counted release) is already proven.
void LiveOutputService::takeInput(const QString &label, const QString &kind, const QString &mode)
{
    if (kind != QLatin1String("camera") && kind != QLatin1String("screen")
        && kind != QLatin1String("ndi")) {
        // Honest refusal: media has no compositor layer of its own — it
        // takes through takeMedia() instead (the pane routes that already).
        qWarning("LiveOutputService: input kind '%s' has no output-preview tap", kind.toUtf8().constData());
        emit inputChanged();
        return;
    }
    if (label.isEmpty())
        return;

    // ONE compositor layer: taking an input takes the media file off air
    // (the two holds are mutually exclusive — the last gesture wins).
    clearMedia();

    // Re-taking while taken: release the previous hold first (one layer).
    if (inputLabel_ == label && inputKind_ == kind) {
        // Same source re-picked: just keep the tap alive (mode changes are
        // handled by the caller's new tap below).
    } else if (!inputLabel_.isEmpty()) {
        if (inputKind_ == QLatin1String("screen"))
            EngineBridge::instance().stopScreenPreview(inputLabel_, QStringLiteral("output"));
        else if (inputKind_ == QLatin1String("ndi"))
            EngineBridge::instance().stopNdiPreview(inputLabel_, QStringLiteral("output"));
        else
            EngineBridge::instance().stopVideoPreview(inputLabel_, QStringLiteral("output"));
    }

    inputLabel_ = label;
    inputKind_ = kind;
    // Kept for Clear all's undo only (see clearAll/restoreCleared): restoring
    // the take must re-open the same camera at the same capture mode, and the
    // mode is not derivable from anything else the service stores.
    inputMode_ = mode;
    bool started = false;
    if (kind == QLatin1String("screen")) {
        // startScreenPreview reports whether the label resolved to a live
        // window/monitor and the tap started — a refused take (window closed,
        // title drifted) unwinds the take here instead of parking a
        // warm-up placeholder that can never fill.
        started = EngineBridge::instance().startScreenPreview(label, QStringLiteral("output"));
    } else if (kind == QLatin1String("ndi")) {
        // The engine's NDI receiver (the BroadcastEngine's provider
        // runtime-loads the vendor SDK). A refusal (runtime missing/SDK
        // load failure) unwinds the take exactly like a refused screen tap
        // — the roster row keeps its idle glyph instead of a placeholder
        // that can never fill.
        started = EngineBridge::instance().startNdiPreview(label, QStringLiteral("output"));
    } else {
        EngineBridge::instance().startVideoPreview(label, mode, QStringLiteral("output"));
        started = true;   // camera refusals surface via the provider's empty frames
    }
    if (!started) {
        inputLabel_.clear();
        inputKind_.clear();
        emit inputChanged();
        return;
    }

    if (!inputPump_) {
        inputPump_ = new QTimer(this);
        inputPump_->setInterval(66);   // ~15 fps, matching the tap's production rate
        connect(inputPump_, &QTimer::timeout, this, &LiveOutputService::pumpTick);
    }
    inputPump_->start();
    // inputLive_ flips true when the provider's first frame decodes — the
    // service can't see the provider's cache, so the QML side confirms via
    // confirmInputFrame() (warm-up honest: the pill/placeholder shows until
    // then).
    // The RESOLVED PAL device id, not the roster label — CompositorState's
    // reader (SceneBuilder, engine-side) calls straight into
    // IVideo::PreviewFramePixels(deviceId), which knows nothing about QML
    // roster labels.
    const QString resolvedId = EngineBridge::instance().resolvedPreviewDeviceId(label);
    pl::CompositorState::Instance().SetTakenInput(resolvedId.toStdString(), kind.toStdString());
    emit inputChanged();
}

// One pump tick of the NDI input feed: while an NDI source is TAKEN (output
// take, owner "output") its newest engine frame is pushed into
// CompositorState's media layer — the scene builder's compositor pass
// (AddCompositorLayers) draws whatever sits there UNDER the on-air content,
// the exact layer the media decoder feeds. Card previews don't push (they
// render straight off the QML provider, like every camera card).
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

// Release the NDI input's compositor hold (ClearTakenInput covers the PAL
// tap side; the frame side clears only when THIS feed is what filled it).
void LiveOutputService::clearNdiInputFrame()
{
    if (!ndiInputLive_)
        return;
    pl::CompositorState::Instance().ClearMedia();
    ndiInputLive_ = false;
}

// KEPT FOR COMPATIBILITY, DELIBERATELY EMPTY: frame detection moved into
// the service's pump (reading the provider's decode cache). The previous
// QML round-trip — tile's Image onStatusChanged → confirmInputFrame() →
// inputRev bump → the SAME tile's inputSource URL — was a write-in-binding
// feedback edge and QML flagged it as a binding loop.
void LiveOutputService::confirmInputFrame(const QString &label)
{
    Q_UNUSED(label)
}

// ---------------------------------------------------------------------------
// Overlays on air — multiple, stacked (see the header's activeOverlays doc).
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

// ---------------------------------------------------------------------------
// The shared input pump — while a take (owner "output") OR a card preview
// (owner "card") is held, this ticks at ~15 Hz: flips inputLive on the taken
// input's first decoded frame (SERVICE-SIDE, off the provider's decode-once
// cache — the previous QML confirmInputFrame() round-trip was a write-in-
// binding feedback edge QML flagged as a binding loop; QML only reads now),
// then bumps inputRev so every consuming Image re-fetches (the provider
// answers warm-up requests with a 1×1 transparent that keeps placeholders
// up without ever flipping inputLive). Stops itself when nothing is held.
void LiveOutputService::pumpTick()
{
    // The taken input's frame is pulled ONCE per tick and shared by both
    // consumers — first-frame detection and (for NDI) the compositor push.
    // The QML provider's own request per rev bump pulls independently; each
    // ReceiveFrame is non-blocking and returns the newest frame, so none of
    // the pulls can queue or stall behind each other.
    QImage takenFrame;
    if (!inputLabel_.isEmpty()) {
        takenFrame = inputFrame(inputLabel_, inputKind_);
        if (!inputLive_ && !takenFrame.isNull() && takenFrame.width() > 1) {
            inputLive_ = true;
            qInfo("LiveOutputService: first frame decoded for '%s' (%dx%d)",
                  qUtf8Printable(inputLabel_), takenFrame.width(), takenFrame.height());
        }
    }
    // Per-label production (card previews' thumbnails light off this —
    // inputLive only tracks the OUTPUT take). Freshly-taken labels enter
    // producing_ on their first frame; released labels are pruned in
    // clearInput/endPreviewInput.
    for (const QString &held : cardPreviews_.keys()) {
        if (producing_.contains(held))
            continue;
        const QImage frame = inputFrame(held, cardPreviews_.value(held).toMap()
                                                .value(QStringLiteral("kind")).toString());
        if (!frame.isNull() && frame.width() > 1)
            producing_.insert(held);
    }
    if (!inputLabel_.isEmpty() && !producing_.contains(inputLabel_)
        && !takenFrame.isNull() && takenFrame.width() > 1)
        producing_.insert(inputLabel_);
    // Unconditional rev bump while ANY tap is held — tiles and cards share
    // the nonce, so both re-fetch.
    inputRev_++;
    // NDI taken as the OUTPUT input feeds the engine's compositor layer
    // (the same one the media decoder fills) with the frame pulled above.
    if (!takenFrame.isNull())
        pushNdiInputFrame(takenFrame);
    emit inputChanged();
    if (inputLabel_.isEmpty() && cardPreviews_.isEmpty())
        inputPump_->stop();
}

bool LiveOutputService::inputProducing(const QString &label) const
{
    return producing_.contains(label);
}

// The newest frame of a held tap, kind-aware: camera/screen pulls through
// the PAL's decode-once cache (previewFrameFor), NDI through the engine
// receiver's convert-on-demand (previewNdiFrameFor). The pump's first-frame
// detection reads through this one switch, so a kind can never watch the
// wrong tap family.
QImage LiveOutputService::inputFrame(const QString &label, const QString &kind) const
{
    if (kind == QLatin1String("ndi"))
        return EngineBridge::instance().previewNdiFrameFor(label);
    return EngineBridge::instance().previewFrameFor(label);
}

// Release ONLY an NDI take — the "ndi" feature switch's teardown path
// (EngineBridge::applyNdiFeatureState). No-op for camera/screen takes and
// card previews (those ride PAL taps the feature gate doesn't touch).
void LiveOutputService::clearNdiInput()
{
    if (inputKind_ != QLatin1String("ndi") || inputLabel_.isEmpty())
        return;
    clearInput();
}

// ---------------------------------------------------------------------------
// Internal card previews — ONE click on a Media card shows its live feed in
// the card's own state window (owner "card"); the OUTPUT take (double-click,
// owner "output", purple border) stays a separate, deliberate gesture. Both
// ride the SAME owner-counted PAL taps, so a card preview and the output
// take can hold the same source at once without fighting.
// ---------------------------------------------------------------------------
void LiveOutputService::previewInput(const QString &label, const QString &kind, const QString &mode)
{
    if (label.isEmpty() || (kind != QLatin1String("camera") && kind != QLatin1String("screen")
                            && kind != QLatin1String("ndi")))
        return;
    // Refuse UNREACHABLE sources quietly: a closed window / unplugged camera
    // must show the card's slashed "can't reach" state, not a toast.
    if (!EngineBridge::instance().inputSourceReachable(label, kind)) {
        qInfo("LiveOutputService: '%s' is not reachable right now (window closed / device gone)",
              qUtf8Printable(label));
        emit inputChanged();
        return;
    }
    bool started = true;
    if (kind == QLatin1String("screen"))
        started = EngineBridge::instance().startScreenPreview(label, QStringLiteral("card"));
    else if (kind == QLatin1String("ndi"))
        started = EngineBridge::instance().startNdiPreview(label, QStringLiteral("card"));
    else
        EngineBridge::instance().startVideoPreview(label, mode, QStringLiteral("card"));
    if (!started) {
        // Refused (window closed / SDK load failure between the reachability
        // check above and the actual take) — same unwind as takeInput(): no
        // card entry, no pump, so the card keeps its idle/slashed state
        // instead of parking a placeholder that can never fill.
        emit inputChanged();
        return;
    }
    cardPreviews_[label] = QVariantMap{
        { QStringLiteral("kind"), kind },
        { QStringLiteral("mode"), mode },
    };
    if (!inputPump_) {
        inputPump_ = new QTimer(this);
        inputPump_->setInterval(66);
        connect(inputPump_, &QTimer::timeout, this, &LiveOutputService::pumpTick);
    }
    if (!inputPump_->isActive())
        inputPump_->start();
    emit inputChanged();
}

void LiveOutputService::endPreviewInput(const QString &label)
{
    if (!cardPreviews_.contains(label))
        return;
    const QVariantMap entry = cardPreviews_.take(label).toMap();
    const QString kind = entry.value("kind").toString();
    if (kind == QLatin1String("screen"))
        EngineBridge::instance().stopScreenPreview(label, QStringLiteral("card"));
    else if (kind == QLatin1String("ndi"))
        EngineBridge::instance().stopNdiPreview(label, QStringLiteral("card"));
    else
        EngineBridge::instance().stopVideoPreview(label, QStringLiteral("card"));
    producing_.remove(label);
    emit inputChanged();
}

QString LiveOutputService::inputFailure(const QString &label, const QString &kind) const
{
    if (label.isEmpty())
        return {};
    // ONLY A HELD TAP has a reason to give: a released one is not failing, it is
    // closed, and a stale reason on a card nobody is previewing would be a lie.
    if (!cardPreviews_.contains(label) && label != inputLabel_)
        return {};
    // An NDI receiver reports its refusal through empty frames (its runtime is
    // loaded by the engine's own provider) — there is no per-tap reason to ask.
    if (kind == QLatin1String("ndi"))
        return {};
    return EngineBridge::instance().previewFailure(label);
}

bool LiveOutputService::inputAvailable(const QString &label, const QString &kind)
{
    // A row that names no device has nothing to be available: the same "no" the
    // cached probe gives, and (see EngineBridge::inputSourceAlive) no ping either.
    if (label.isEmpty())
        return false;
    // The one difference from inputHealthy: this one may ASK (see the header).
    // Reachability of the DEVICE is the question — a window is "there" while it
    // is open, a camera while it is plugged in — so the answer is the bridge's
    // own probe, which pings when its cache cannot say.
    return EngineBridge::instance().inputSourceAlive(label, kind);
}

QString LiveOutputService::inputDelivery(const QString &label, const QString &kind)
{
    // The engine owns every tap this could be about (the card thumbnails, the
    // monitor holds, the output take and the input meters all live in the
    // bridge's tables), so this is a read-through — and a read-through that
    // OPENS NOTHING: it reports what the taps already deliver.
    if (label.trimmed().isEmpty())
        return {};
    return EngineBridge::instance().inputDelivery(label, kind);
}

bool LiveOutputService::inputHealthy(const QString &label, const QString &kind) const
{
    // Unreachable BY RESOLUTION (window closed / device unplugged): the
    // slash state. A held tap that simply hasn't produced its first frame
    // yet (warm-up) still counts as healthy — reachability is the question,
    // not instantaneous throughput.
    return EngineBridge::instance().inputSourceReachable(label, kind);
}

void LiveOutputService::clearInput()
{
    if (inputLabel_.isEmpty())
        return;
    // NOTE: media and the input take are separate holds (only one can exist
    // at a time — takeInput()/takeMedia() clear each other), so clearing the
    // input never touches media state.
    if (inputKind_ == QLatin1String("screen"))
        EngineBridge::instance().stopScreenPreview(inputLabel_, QStringLiteral("output"));
    else if (inputKind_ == QLatin1String("ndi"))
        EngineBridge::instance().stopNdiPreview(inputLabel_, QStringLiteral("output"));
    else
        EngineBridge::instance().stopVideoPreview(inputLabel_, QStringLiteral("output"));
    // Remove BEFORE clearing — producing_.remove(inputLabel_) after
    // inputLabel_.clear() removed the empty-string key instead of the real
    // one, leaving a permanently-stale entry in producing_ and making
    // inputProducing(label) keep answering true for a tap that no longer
    // exists.
    producing_.remove(inputLabel_);
    inputLabel_.clear();
    inputKind_.clear();
    inputMode_.clear();
    inputLive_ = false;
    clearNdiInputFrame();   // the compositor frame side of an NDI take
    pl::CompositorState::Instance().ClearTakenInput();
    // A held card preview keeps the pump (and its rev bumps) alive.
    if (inputPump_ && cardPreviews_.isEmpty())
        inputPump_->stop();
    emit inputChanged();
}

// ---------------------------------------------------------------------------
// Env-gated boot self-test: VGR_OUTPUT_INPUT_TEST=1 drives the WHOLE
// taken-input chain automatically — enumerate → take the first real window
// → wait through the tap's warm-up → read the provider's decode cache → log
// a PASS/FAIL verdict. Exists so the input-preview pipeline can be verified
// from a launch log alone (no clicks, no dialog, no QML binding involved —
// the layer the binding loop poisoned). Inert without the env var.
void LiveOutputService::runEnvSelfTest()
{
    if (!qEnvironmentVariableIsSet("VGR_OUTPUT_INPUT_TEST"))
        return;
    QTimer::singleShot(2500, &instance(), [] {
        QString label;
        const QVariantList screen = EngineBridge::instance().screenDevices();
        for (const QVariant &v : screen) {
            const QVariantMap d = v.toMap();
            // First real window (not our own app's window — the engine's
            // enumeration excludes self by pid, the roster label here is
            // just belt-and-braces).
            if (d.value("id").toString().startsWith(QStringLiteral("win:"))) {
                label = d.value("label").toString();
                if (label != QCoreApplication::applicationName())
                    break;
            }
        }
        if (label.isEmpty()) {
            qWarning("LiveOutputService[selftest] FAIL: no window source to take");
            return;
        }
        qInfo("LiveOutputService[selftest] taking window '%s'", qUtf8Printable(label));
        instance().takeInput(label, QStringLiteral("screen"), QString());
        if (instance().inputLabel().isEmpty()) {
            qWarning("LiveOutputService[selftest] FAIL: take was refused");
            return;
        }
        QTimer::singleShot(4000, &instance(), [label] {
            // The service's OWN live flag — flipped by the decode-cache poll,
            // i.e. proof the tap produced real pixels end to end.
            if (instance().inputLive())
                qInfo("LiveOutputService[selftest] PASS: frames decoded for '%s'",
                      qUtf8Printable(label));
            else
                qWarning("LiveOutputService[selftest] FAIL: no frames decoded for '%s'",
                         qUtf8Printable(label));
            instance().clearInput();   // phase boundary — release before the next probe
            // RE-ACTIVATION PROBE — the new gesture model: activation is a
            // DOUBLE-click (one deliberate toggle in QML), so the service
            // must survive a same-source re-activation without losing the
            // tap or the frames (a stray rapid re-activation can also come
            // from a double-toggled card). Take again, take the SAME source
            // once more immediately, and expect the take — and the frames —
            // to still be alive 2.5 s later.
            QTimer::singleShot(400, &instance(), [label] {
                instance().takeInput(label, QStringLiteral("screen"), QString());
                instance().takeInput(label, QStringLiteral("screen"), QString());   // immediate re-activation
                const bool held = instance().inputLabel() == label;
                QTimer::singleShot(2500, &instance(), [label, held] {
                    if (held && instance().inputLive())
                        qInfo("LiveOutputService[selftest] REACTIVATE PASS: same-source re-activation kept the tap, frames flowing");
                    else
                        qWarning("LiveOutputService[selftest] REACTIVATE FAIL: held=%d live=%d",
                                 held ? 1 : 0, instance().inputLive() ? 1 : 0);
                    instance().clearInput();   // the self-test leaves NO side effects
                    // METER PHASE: start the FIRST audio input's meter tap and
                    // wait through warm-up + a capture window. FAIL = the tap
                    // never produced a non-zero channelCount (no signal — the
                    // "meter doesn't animate" report), PASS = real peaks.
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
                    QTimer::singleShot(1200, &instance(), [audioLabel] {
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
                            QVariantList peaks = snap.value("peaks").toList();
                            qInfo("LiveOutputService[selftest] METER PASS: '%s' tap live, %d channels, peak=%.3f",
                                  qUtf8Printable(audioLabel), cc,
                                  peaks.isEmpty() ? 0.0 : peaks.first().toDouble());
                        } else
                            qWarning("LiveOutputService[selftest] METER FAIL: '%s' produced no snapshot (no signal)",
                                     qUtf8Printable(audioLabel));
                        EngineBridge::instance().stopInputMeter(audioLabel);
                    });
                });
            });
        });
    });
}

void LiveOutputService::stop()
{
    // ASYNC STOP (the "press GO LIVE then immediately STOP" crash): the
    // controller's StopLive JOINS the render worker, and on this thread that
    // froze the whole GUI whenever the worker was mid-first-frame (AppHangB1
    // at 20:13, 0.0.3). StopLiveAsync hands the join to a reaper thread —
    // this function returns at once and the UI keeps pumping.
    const bool wasLive = live_;
    bps::live::LiveOutputController::Instance().StopLiveAsync();
    if (live_) {
        live_ = false;
        emit liveChanged();
    }
    // OFF AIR KEEPS THE PICTURE. STOP is the OUTPUT gate, not a content
    // action — live report: "the GO LIVE button always clears the screen; its
    // only responsibility is the outgoing frames, it has nothing to do with
    // the content on the monitor preview". This used to blank onAirSlide_ on
    // the way out, so pressing STOP threw the monitor tile back to the empty
    // checkerboard and the pick was gone for good (a second GO LIVE then fell
    // through to the blank holding slide).
    //
    // The slide that was just on air moves back into the STAGED slot — the
    // same place stageSlides() puts a pick that is only being previewed —
    // which is exactly "on the monitor but not on the outputs":
    //   * the tile keeps the identical picture (OutputMonitorTile already
    //     prefers stagedSlideRaw, and it is the same raw slide the tile was
    //     reading a moment ago);
    //   * GO LIVE again re-commits that same pick (goLive()'s staged branch)
    //     instead of broadcasting a blank holding screen;
    //   * the toolbar's staged gate keeps Clear all / the slide-clear button
    //     reachable off air, so retained content is never unclearable;
    //   * and a new pick staged afterwards simply replaces it, as always.
    // stagedSlide_ takes onAirSlide_ (both are the slide run through the
    // active style, so the swap changes no pixels).
    if (!onAirSlideRaw_.isEmpty()) {
        stagedName_ = onAirTitle_;
        stagedSlidesRaw_ = QVariantList{ onAirSlideRaw_ };
        stagedSlide_ = onAirSlide_;
        emit stagedChanged();
    }
    onAirSlide_ = QVariantMap{};
    onAirSlideRaw_ = QVariantMap{};
    // onAirTitle_/onAirIndex_/onAirTotal_ are ONLY otherwise written by
    // pollTick(), which stops running the moment live_ flips false (below) —
    // without this reset they held their last live values forever, so
    // onAirTotal stayed > 0 after every stop() and any QML gate reading it
    // (the MonitorWall toolbar's slide-clear button, "is anything on air")
    // never noticed the output had actually gone dark.
    onAirTitle_.clear();
    onAirIndex_ = -1;
    onAirTotal_ = 0;
    emit onAirChanged();
    if (poll_)
        poll_->stop();
    // The NDI feed has its own clock now — off air it stops with it.
    stopNdiSendClock();
    stopNdiFeed();
    // An NDI INPUT taken while going off air also stops feeding the engine's
    // compositor (the pump stops with it — one last explicit clear keeps the
    // layer honest if the take is ever re-taken without clearInput first).
    clearNdiInputFrame();
    // Transition toast: the teardown is now asynchronous, so say so — the
    // user pressed STOP and the button flips instantly, but the engine's
    // off-air cleanup finishes a moment later in the background.
    if (wasLive)
        EventBus::instance().notify(QStringLiteral("Going off air…"),
                                    QStringLiteral("info"), QStringLiteral("Go Live"));
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

// ---------------------------------------------------------------------------
// MEDIA ON AIR — a real decoder (QMediaPlayer → QVideoSink), composited by
// every monitor tile as one more QML image layer (image://mediaplay) UNDER
// the on-air content — the SAME layer convention the taken camera input
// rides. The engine's render pipeline composites rasters only (a "media"
// slide block draws as a name-on-a-tile placeholder), so the decode happens
// service-side and the file's AUDIO rides the player's own WASAPI output —
// the machine's mix, which the program-mix loopback tap already meters.
// ---------------------------------------------------------------------------
void LiveOutputService::takeMedia(const QString &path, const QString &name)
{
    const QString clean = path.startsWith(QStringLiteral("file:///"))
        ? QUrl(path).toLocalFile() : path;
    if (clean.isEmpty() || !QFileInfo::exists(clean)) {
        qWarning("LiveOutputService: takeMedia: file not found: %s", qUtf8Printable(clean));
        emit mediaChanged();
        return;
    }

    static const char *videoExts[] = { "mp4", "mov", "mkv", "avi", "webm", "m4v", "wmv" };
    static const char *imageExts[] = { "png", "jpg", "jpeg", "bmp", "gif", "webp" };
    // Same set MediaLibrary.cpp's own classifier recognizes as audio (see
    // its ExtToKind) — a file indexed there as "audio" must not turn out
    // to be refused here.
    static const char *audioExts[] = { "mp3", "wav", "wave", "flac", "aac", "m4a", "m4b", "ogg", "oga",
                                       "opus", "wma", "aif", "aiff", "aifc", "mka", "amr", "ac3", "weba" };
    const QString ext = QFileInfo(clean).suffix().toLower();
    bool isVideo = false;
    bool isImage = false;
    bool isAudio = false;
    for (const char *e : videoExts)
        if (ext == QLatin1String(e)) { isVideo = true; break; }
    for (const char *e : imageExts)
        if (ext == QLatin1String(e)) { isImage = true; break; }
    for (const char *e : audioExts)
        if (ext == QLatin1String(e)) { isAudio = true; break; }
    if (!isVideo && !isImage && !isAudio) {
        qWarning("LiveOutputService: takeMedia: '%s' has an unrecognized extension",
                 qUtf8Printable(clean));
        emit mediaChanged();
        return;
    }

    // ONE compositor layer: taking media releases a held input take (the
    // two holds are mutually exclusive — the last gesture wins).
    if (!inputLabel_.isEmpty())
        clearInput();

    if (!mediaPlayer_) {
        mediaSink_ = new QVideoSink(this);
        mediaAudio_ = new QAudioOutput(this);
        mediaPlayer_ = new QMediaPlayer(this);
        mediaPlayer_->setAudioOutput(mediaAudio_);
        mediaPlayer_->setVideoOutput(mediaSink_);
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
        // End of file: rewind to the head and hold on the last frame — a
        // countdown/service video parks instead of blinking off air.
        connect(mediaPlayer_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus st) {
            if (st == QMediaPlayer::EndOfMedia) {
                mediaPlayer_->pause();
                mediaPlayer_->setPosition(0);
                mediaState_ = QStringLiteral("paused");
                emit mediaChanged();
            }
        });
        connect(mediaSink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &f) {
            if (!f.isValid())
                return;
            lastMediaFrame_ = f.toImage();
            if (!lastMediaFrame_.isNull())
                mediaRev_++;
            pushMediaFrameToCompositor(lastMediaFrame_);
            emit mediaTick();
        });
    }

    mediaPath_ = clean;
    mediaName_ = name.isEmpty() ? QFileInfo(clean).completeBaseName() : name;
    mediaIsVideo_ = isVideo;
    mediaIsAudio_ = isAudio;
    lastMediaFrame_ = QImage();   // warm-up: the tiles show their placeholder
    pushMediaFrameToCompositor(lastMediaFrame_);
    mediaOnAir_ = true;
    if (isVideo || isAudio) {
        // Audio rides the SAME QMediaPlayer/QAudioOutput path video already
        // uses — it just never feeds mediaSink_ a video frame, so mediaRev_
        // never bumps and the tiles' frame layer stays honestly empty. The
        // audio plays straight through Qt's own QAudioOutput (see mediaAudio_
        // above), NOT through the engine's AudioMixer: that mixer's render path
        // was unreachable and has been removed (2026-10, see the removal note
        // in EngineBridge.cpp), so there is no program-mix meter for the tile's
        // L/R bars to read. Those bars show the loudest TAPPED input while the
        // output is on air and nothing else.
        mediaPlayer_->setSource(QUrl::fromLocalFile(clean));
        mediaAudio_->setMuted(mediaMuted_);
        mediaState_ = QStringLiteral("playing");
        mediaPlayer_->play();
    } else {
        // An IMAGE paints through the same provider: decode it once here (GUI
        // thread, one file, no player involved). A failed decode leaves the
        // take up but frameless — the tile's placeholder stays honest.
        lastMediaFrame_ = QImage(clean);
        if (!lastMediaFrame_.isNull())
            mediaRev_++;
        pushMediaFrameToCompositor(lastMediaFrame_);
        mediaState_ = QStringLiteral("playing");   // a still "plays" forever
        mediaPosition_ = 0;
        mediaDuration_ = 0;
    }
    qInfo("LiveOutputService: media on air: '%s' (%s)", qUtf8Printable(mediaName_),
          isVideo ? "video" : (isAudio ? "audio" : "image"));
    emit mediaChanged();
    emit mediaTick();
}

void LiveOutputService::clearMedia()
{
    if (!mediaOnAir_)
        return;
    if (mediaPlayer_)
        mediaPlayer_->stop();
    mediaOnAir_ = false;
    mediaPath_.clear();
    mediaName_.clear();
    mediaState_ = QStringLiteral("stopped");
    mediaPosition_ = 0;
    mediaDuration_ = 0;
    lastMediaFrame_ = QImage();
    pl::CompositorState::Instance().ClearMedia();
    qInfo("LiveOutputService: media taken off air");
    emit mediaChanged();
    emit mediaTick();
}

void LiveOutputService::mediaTogglePlay()
{
    // Video and audio both have a real QMediaPlayer transport; an image
    // has none (mediaPlayer_ is never even pointed at it).
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
    if (mediaAudio_)
        mediaAudio_->setMuted(mediaMuted_);
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
    const QImage &frame = LiveOutputService::instance().lastMediaFrame();
    if (frame.isNull()) {
        // Taken but not decoded yet (or off air): 1×1 transparent — the QML
        // side reads it as "no picture" (no warning spam), the placeholder
        // stays up.
        static const QImage empty = [] {
            QImage e(1, 1, QImage::Format_ARGB32);
            e.fill(Qt::transparent);
            return e;
        }();
        if (size)
            *size = QSize(1, 1);
        return empty;
    }
    if (size)
        *size = frame.size();
    // Scale-to-request like the other providers (a ~360px pane asking for
    // 1080p frames every tick would burn the GUI thread on blits).
    if (requestedSize.isValid() && !requestedSize.isEmpty()
        && requestedSize != frame.size())
        return frame.scaled(requestedSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return frame;
}

// ---------------------------------------------------------------------------
// Navigation while live
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

// The preview toolbar's ‹ › (FreeShow's OutputHelper.advanceOutputs): a step
// inside the on-air set is plain navigation; at either END of the set (a
// single-verse pick makes BOTH ends) the step becomes a PASSAGE step — the
// tab whose content is on air re-picks the neighbouring passage and replays
// it, exactly like the preview pane's own ‹ › pills.
void LiveOutputService::stepPassage(int direction)
{
    if (!live_) {
        // A STAGED pick is what the tile is showing and the wall's toolbar is
        // on screen for it, so ‹ › must reach the tab that owns it — the step
        // re-stages the neighbouring passage. Nothing staged: nothing to step.
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
    // Same title test liveIsOurs() makes, against the staged name — the pane
    // stages its own referenceText as the pick's name, so the two agree.
    return !live_ && !stagedSlidesRaw_.isEmpty()
           && !title.isEmpty() && stagedName_ == title;
}

// A tab announcing (or withdrawing) itself as a stepper of the tile's content
// — see the header. Only a TRANSITION of the aggregate is signalled, so the
// wall's arrows are not relit on every tab's every announcement.
void LiveOutputService::setWallStepper(const QString &id, bool active)
{
    if (id.isEmpty())
        return;
    const bool before = wallSteppable();
    if (active)
        wallSteppers_.insert(id, true);
    else
        wallSteppers_.remove(id);
    if (wallSteppable() != before)
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

// ---------------------------------------------------------------------------
// The on-air slide as design blocks (the QML preview feed)
// ---------------------------------------------------------------------------
QVariantList LiveOutputService::onAirSlidesRaw() const
{
    QVariantList out;
    if (!live_)
        return out;
    auto &pres = pl::PresentationEngine::Instance();
    const pl::Presentation *active = pres.Runtime().ActivePresentation();
    if (!active)
        return out;
    for (const pl::Slide &s : active->slides) {
        if (s.hidden)
            continue;
        out.append(slideToVariantMap(&s, pl::OutputStyleSpec{}));
    }
    return out;
}

void LiveOutputService::refreshOnAirSlide()
{
    const pl::Slide *slide = live_ ? pl::PresentationEngine::Instance().CurrentSlide() : nullptr;
    onAirSlide_ = slide ? slideToVariantMap(slide) : QVariantMap{};
    onAirSlideRaw_ = slide ? slideToVariantMap(slide, pl::OutputStyleSpec{}) : QVariantMap{};
}

QVariantMap LiveOutputService::onAirSlideAt(int index) const
{
    if (!live_ || index < 0)
        return {};
    auto &pres = pl::PresentationEngine::Instance();
    const pl::Presentation *active = pres.Runtime().ActivePresentation();
    if (!active || active->slides.empty())
        return {};
    // The index is a VISIBLE index (CurrentIndex() skips hidden slides, and
    // that is what onAirIndex carries) — walk the slides skipping hidden ones
    // so a thumbnail strip's indices line up with the on-air ones.
    int visible = -1;
    for (const pl::Slide &s : active->slides) {
        if (s.hidden)
            continue;
        ++visible;
        if (visible == index)
            return slideToVariantMap(&s);
    }
    return {};
}

// A library preview's own slide (ShowCenter's grid/list thumbnails), run
// through the exact same slideToVariantMap composition onAirSlide/
// stagedSlide already use — so a show's preview shows what it will
// actually look like once on air (the active output's style/template),
// not just its own raw, unstyled blocks. Round-trips the QML block shape
// through a real engine Slide only because slideToVariantMap's contentType
// tagging (SlideRules::ContentType) reads it off pl::Slide::metaJson.
QVariantMap LiveOutputService::previewWithActiveStyle(const QVariantMap &slide) const
{
    const pl::Slide engineSlide = ShowConverter::slideFromVariant(slide);
    return slideToVariantMap(&engineSlide);
}

QVariantMap LiveOutputService::previewWithStyle(const QVariantMap &slide,
                                                 const QString &styleId) const
{
    const pl::Slide engineSlide = ShowConverter::slideFromVariant(slide);
    return slideToVariantMap(&engineSlide, outputStyleForPreview(styleId));
}

// ---------------------------------------------------------------------------
// Poll (onAir titles + frame counter while live)
// ---------------------------------------------------------------------------
void LiveOutputService::pollTick()
{
    if (!live_)
        return;

    auto &ctrl = bps::live::LiveOutputController::Instance();
    if (ctrl.IsLiveContentReplacementPending())
        return;

    // NDI monitor-connection telemetry rides the live poll (the bridge
    // caps itself at ~1 Hz internally and emits only on change).
    EngineBridge::instance().refreshNdiConnections();

    // TWO COUNTERS, ONE CACHE-BUSTER. The engine publishes twice per
    // iteration: PreviewFrames() the moment the MAIN pass has written the
    // live preview, FramesSent() once every per-output pass has also landed.
    // Watching only the late one made the monitor preview wait out the
    // (heavier) per-output passes — the reported "I click a paragraph and it
    // delays before it updates on the monitor preview". Watching both costs
    // one refetch more per content change and shows the click immediately.
    const qulonglong sent = ctrl.FramesSent();
    const qulonglong previewed = ctrl.PreviewFrames();
    if (sent != framesSent_ || previewed != previewFrames_) {
        framesSent_ = sent;
        previewFrames_ = previewed;
        // Bump the cache-buster too — without this frameRev stayed 0 forever,
        // hasFrame was never true, and every monitor tile showed the
        // checkerboard while LIVE (the emit alone changes nothing: QML
        // re-evaluates the URL and gets the identical string).
        ++frameRev_;
        emit frameRevChanged();
    }

    // ---- NDI program sender ------------------------------------------------
    // The FIRST enabled NDI output's feed also goes to the network while the
    // show is live (regardless of which output is active — a projector on
    // air must not mute the NDI program feed): the display engine's
    // NdiDisplayProvider sends each RGBA frame UNCONVERTED ('RGBA' fourCC)
    // and hands it to
    // the BroadcastEngine (real NDI when the runtime is installed, the
    // software loopback otherwise). The sender is the engine's own — this
    // only feeds it from the SAME per-output buffers the monitor tiles
    // read, so what the network gets is what the wall shows.
    //
    // The SEND itself never rides this poll: the feed has its own clock on the
    // feed thread, armed at the output's configured Refresh rate, so the feed
    // runs at the setting's rate instead of this poll's 10 Hz — and no GUI-side
    // hiccup can punch a hole in it. What runs here is the cheap half: publish
    // WHAT to feed (see pollTickRecheckNdi), which is how a roster edit, a
    // rename or a rate change reaches the wire within one poll.
    pollTickRecheckNdi();

    // Watch the NDI feed's real send rate against its configured Refresh rate
    // (drift warning + the runtime halving/restore toasts).
    checkNdiFeedRate();

    // The engine's loop advances slides itself; mirror the runtime's state.
    auto &pres = pl::PresentationEngine::Instance();
    const pl::Slide *slide = pres.CurrentSlide();
    const QString title = slide ? QString::fromStdString(slide->title) : QString();
    const int idx = int(pres.CurrentIndex());
    const int total = int(pres.Runtime().ActivePresentation()
                              ? pres.Runtime().ActivePresentation()->slides.size()
                              : 0);
    if (title != onAirTitle_ || idx != onAirIndex_ || total != onAirTotal_) {
        onAirTitle_ = title;
        onAirIndex_ = idx;
        onAirTotal_ = total;
        refreshOnAirSlide();
        emit onAirChanged();
    } else {
        // Same title/index/total, but the slide's BLOCKS can still have moved
        // (a document edit re-synced while live, a style rebuild) — the poll
        // re-reads them so the QML preview stays honest. Cheap when unchanged:
        // a property write with no matching QML binding change is free.
        const QVariantMap next = slideToVariantMap(slide);
        const QVariantMap nextRaw = slideToVariantMap(slide, pl::OutputStyleSpec{});
        if (next != onAirSlide_ || nextRaw != onAirSlideRaw_) {
            onAirSlide_ = next;
            onAirSlideRaw_ = nextRaw;
            emit onAirChanged();
        }
    }
}

// ---------------------------------------------------------------------------
// The NDI program sender — the display engine's NdiDisplayProvider IS the
// output ("ndi-program" device, real NDI SDK via the BroadcastEngine with an
// automatic software-loopback fallback). This reads the FIRST enabled NDI
// output's frame (its own styled buffer, or the shared preview feed for an
// unstyled output — the same source the monitor tile shows) and PUBLISHES it
// as the feed's plan; the feed thread's own clock does the sending at the
// output's configured Refresh rate (see ndiWorkerTick), and the 10 Hz poll only
// republishes what changed. The provider sends the engine's native RGBA
// unconverted ('RGBA'
// fourCC — color decisions belong to the SDK's pipeline) and owns the
// sender's network identity. (The removed pass converted RGBA→UYVY
// in software — both wasted CPU and, mis-decoded as zero-YUV, the
// dark-green screen receivers showed while frames were "flowing".)
//
// NOT active-output-gated: the NDI feed runs whenever the show is live,
// even with Main Output (a projector) active — the whole point of a program
// NDI feed is that it carries the show BESIDE the physical screens, not
// instead of them. (This was the "monitor stayed green while live" bug: the
// sender only ever pushed when the NDI row itself was active, so with a
// projector on air the monitor kept showing whatever it had connected to
// last — the self-test's synthetic frame.)
void LiveOutputService::pushNdiFrame()
{
    // First ENABLED output whose kind is NDI wins (row order = roster order,
    // so the first NDI row is deterministic); a disabled one can't go live
    // and must not feed either. Styled vs unstyled mirrors FrameBufferRole
    // exactly (its tile and this feed always show the same picture).
    bool want = false;
    int ndiRow = -1;
    QString ndiOutputName;
    const auto *model = OutputListModel::instance();
    if (model) {
        for (int i = 0; i < model->rowCount(); ++i) {
            const QVariantMap out = model->getOutput(i);
            if (out.value("kind").toString() == QLatin1String("NDI")
                && out.value("isEnabled").toBool()) {
                want = true;
                ndiRow = i;
                ndiOutputName = out.value("name").toString();
                break;
            }
        }
    }
    // Feature switch off: the sender can't exist (the BroadcastEngine
    // refuses NDI-shaped sends). Log it ONCE per outage so "my NDI monitor
    // shows nothing" has a reason in the log — and then STOP, before any
    // frame is copied or queued.
    //
    // This used to only LOG the gate and fall through to the send. The
    // worker then called the provider's SendFrame, the BroadcastEngine
    // refused it ("NDI is switched off"), and the failure surfaced as a
    // misleading "NDI SendFrame FAILED — check the engine log" plus a
    // spurious reset of the sending state. Worse, the provider's lazy sender
    // creation ran first: an enabled NDI output row with the NDI feature OFF
    // (a real configuration — the row outlives the feature toggle) tried to
    // build an SDK sender it was not allowed to have, every single tick.
    const bool ndiUsable = EngineBridge::instance().ndiAvailable();
    // "unknown" is NOT a verdict — it is the state the runtime probe sits in
    // until its worker answers. That probe is ASYNC (see
    // EngineBridge::probeNdiAsync) and a device refresh re-runs it (a settings
    // visit, a display-mode change), so for the few ms between the refresh and
    // the answer ndiAvailable() reads false with an EMPTY status. Warning on
    // that window toasted a false
    //   "NDI output requested but NDI is not usable () — no frames will be sent"
    // every time one landed mid-live — the log shows the warning at 48.024 and
    // "NDI program sending started" 36 ms later, i.e. the feed was fine and
    // the operator got a crash-flavoured error box for a probe in flight.
    // Only a REAL verdict (disabled / notInstalled / error) earns the warning.
    const QString ndiState = EngineBridge::instance().ndiState();
    const bool ndiVerdict = ndiState != QLatin1String("unknown");
    if (want && !ndiUsable) {
        if (ndiVerdict && !ndiGateLogged_) {
            ndiGateLogged_ = true;
            qWarning("LiveOutputService: NDI output requested but NDI is not usable (%s) — no frames will be sent",
                     qUtf8Printable(EngineBridge::instance().ndiStatus()));
        }
    } else {
        ndiGateLogged_ = false;
    }
    if (want && !ndiUsable) {
        // Tear the feed state down and stop. The gate re-opens by itself:
        // ndiGateLogged_ and this early return both clear the moment the
        // feature is switched back on (or the probe answers Ready), and the
        // next tick resumes sending. A probe still in flight takes this same
        // exit without a word — the answer is milliseconds away.
        if (ndiSending_) {
            std::lock_guard<std::mutex> lock(ndiStateMutex_);
            ndiSending_ = false;
            ndiSendingOutput_.clear();
        }
        setNdiWarming(false, QString());   // NDI unusable: nothing is warming
        clearNdiPlan();                    // ...and nothing to feed, either
        emit ndiChanged();
        return;
    }

    auto *provider = []() -> bps::display::NdiDisplayProvider * {
        auto p = bps::display::DisplayEngine::Instance().Provider("Ndi");
        return p ? dynamic_cast<bps::display::NdiDisplayProvider *>(p.get()) : nullptr;
    }();

    if (!want || !provider) {
        if (ndiSending_) {
            ndiSending_ = false;
            ndiSendingOutput_.clear();
            emit ndiChanged();
        }
        setNdiWarming(false, QString());   // no NDI row wants feeding
        clearNdiPlan();
        return;
    }

    // The frame: THIS output's own gated buffer when styled, else the
    // shared preview feed (FrameBufferRole's exact keying — qHash(name)).
    // GUI side reads the LAST frame from the engine's buffer and hands a
    // COPY to the worker (the engine reuses its buffer for the next frame,
    // so the pixels must be owned across threads). QImage is implicitly
    // shared — the detach inside the copy constructor is the one allocation.
    const QString styleId = model && ndiRow >= 0
        ? model->getOutput(ndiRow).value("styleId").toString() : QString();
    const std::string buffer = styleId.isEmpty()
        ? std::string(bps::live::LiveOutputController::kPreviewName)
        : QStringLiteral("__out_%1__").arg(qHash(ndiOutputName)).toStdString();
    auto out = pr::RenderEngine::Instance().GetOutput(buffer);
    if (!out.ok()) {
        // One-shot per outage: a missing buffer means the live loop hasn't
        // registered it yet (GO LIVE just pressed) — silence here read as
        // "NDI broken" when it was "NDI warming up".
        if (!ndiWaitLogged_) {
            ndiWaitLogged_ = true;
            qCWarning(lcNdiProgress, "LiveOutputService: NDI output '%s' waiting for buffer '%s' (live loop warming up)",
                     qUtf8Printable(ndiOutputName), buffer.c_str());
            // EXPLICIT toast (not the qWarning→toast path — these progress
            // lines are category-suppressed in main.cpp): the operator needs
            // to know the feed is WARMING UP, not broken, when a receiver
            // shows nothing for the first second or two after GO LIVE. One
            // per outage, re-armed once frames flow.
            EventBus::instance().notify(
                QStringLiteral("NDI output '%1' is warming up — waiting for the live loop's first frame. Sending starts automatically.").arg(ndiOutputName),
                QStringLiteral("info"), QStringLiteral("NDI"), QStringLiteral("outputs.ndi.warmingUp"));
        }
        // The live loop has not registered the buffer yet (GO LIVE just
        // pressed): an enabled NDI row WANTS feeding and has no frame on the
        // wire — the inline "warming up" state Settings · Outputs shows.
        setNdiWarming(true, ndiOutputName);
        clearNdiPlan();
        if (ndiSending()) {
            {
                std::lock_guard<std::mutex> lock(ndiStateMutex_);
                ndiSending_ = false;
                ndiSendingOutput_.clear();
            }
            emit ndiChanged();
        }
        return;
    }
    auto fb = std::dynamic_pointer_cast<pr::FrameBufferOutput>(out.value());
    if (!fb || !fb->Enabled()) {
        clearNdiPlan();
        return;
    }

    // PUBLISH WHAT TO FEED. This is the GUI thread's whole part in the feed —
    // a value copy under its own mutex — and the feed thread's own clock does
    // the sending (see ndiWorkerTick). Nothing here touches pixels, and nothing
    // here can be late in a way the wire notices: the plan is republished every
    // poll, so a rename, a rate change or a disabled row lands within 100 ms.
    //
    // The Output row's own name IS the network-visible NDI source name
    // (re-resolved per poll; the sender's SDK identity is fixed at creation,
    // so a mid-session rename lands next session). The rate advertised is the
    // CONFIGURED Refresh rate — the same number the feed's clock is armed at —
    // and it no longer drops to half under backpressure: a receiver is told the
    // truth, and the beats a slow send overruns are simply skipped.
    NdiSendPlan plan;
    plan.want = true;
    plan.provider = provider;
    plan.buffer = fb;
    plan.senderName = ndiSenderDisplayName(ndiOutputName);
    const float configuredFps = ndiConfiguredFps();
    plan.fps = configuredFps > 0.0f ? configuredFps : 30.0f;
    setNdiPlan(plan);

    // The frame may not exist yet (GO LIVE just pressed): an enabled NDI row
    // wants feeding and has nothing on the wire — the inline "warming up"
    // state Settings · Outputs shows. One note and one toast per outage; the
    // feed thread starts sending by itself the moment a frame is there.
    const auto peek = fb->LastFrameSnapshot();
    if (!peek || peek->empty()) {
        if (!ndiWaitLogged_) {
            ndiWaitLogged_ = true;
            qCWarning(lcNdiProgress, "LiveOutputService: NDI output waiting for the live loop's first rendered frame");
            EventBus::instance().notify(
                QStringLiteral("NDI output '%1' is warming up — waiting for the live loop's first frame. Sending starts automatically.").arg(ndiOutputName),
                QStringLiteral("info"), QStringLiteral("NDI"), QStringLiteral("outputs.ndi.warmingUp"));
        }
        setNdiWarming(true, ndiOutputName);   // buffer exists, no frame yet
        if (ndiSending()) {
            {
                std::lock_guard<std::mutex> lock(ndiStateMutex_);
                ndiSending_ = false;
                ndiSendingOutput_.clear();
            }
            emit ndiChanged();
        }
        return;
    }

    // The pill lights on the GUI the moment there is something to feed (the
    // send itself is the feed thread's job and may take a moment on its first
    // frame — the SDK sender is created there); the worker's own first-frame
    // log covers the frames-flowing beat.
    {
        std::lock_guard<std::mutex> lock(ndiStateMutex_);
        ndiSendingOutput_ = ndiOutputName;   // the pill's LIVE row (rename-safe)
        if (!ndiSending_) {
            ndiSending_ = true;
            qCWarning(lcNdiProgress, "LiveOutputService: NDI program sending started (%s)",
                     qUtf8Printable(plan.senderName.isEmpty() ? QStringLiteral("VGR Program") : plan.senderName));
        }
    }
    ndiWaitLogged_ = false;            // frames are flowing — the waiting notes re-arm
    setNdiWarming(false, QString());   // on the wire: no longer warming
    emit ndiChanged();
}

// Stop the feed when the loop stops (the send clock and the poll are both
// dead by then — stop() stops the clock first).
void LiveOutputService::stopNdiFeed()
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(ndiStateMutex_);
        if (ndiSending_) {
            ndiSending_ = false;
            ndiSendingOutput_.clear();
            changed = true;
        }
    }
    if (changed)
        emit ndiChanged();
    // Reset the incident state: the slow-send latch and the receiver telemetry
    // belong to the live session that just ended — the next one starts clean
    // (and re-reports its monitors from scratch). Under the same lock the feed
    // thread writes them with: it can be mid-send right now.
    {
        std::lock_guard<std::mutex> lock(ndiStateMutex_);
        ndiSlowLogged_ = false;
        ndiFastStreak_ = 0;
    }
    // The plan goes with the session: an empty plan is what stops the feed
    // thread's own clock (see ndiWorkerTick).
    clearNdiPlan();
}

// The feed thread, one per process, created on demand (GUI side).
void LiveOutputService::ensureNdiWorker()
{
    if (ndiThread_)
        return;
    ndiThread_ = new QThread(this);
    ndiThread_->setObjectName("ndi-send");
    ndiWorker_ = new QObject();          // context only; lives on ndiThread_
    ndiWorker_->moveToThread(ndiThread_);
    // AboutTo-quit ordering is handled by shutdownNdiWorker() from
    // main.cpp's aboutToQuit — BEFORE the engine's own teardown.
    ndiThread_->start();
}

void LiveOutputService::clearNdiPlan()
{
    {
        std::lock_guard<std::mutex> lock(ndiPlanMutex_);
        ndiPlan_ = NdiSendPlan{};
    }
    ndiClockArmed_ = false;   // no plan: the feed stops its own clock next beat
}

void LiveOutputService::setNdiPlan(const NdiSendPlan &plan)
{
    {
        std::lock_guard<std::mutex> lock(ndiPlanMutex_);
        ndiPlan_ = plan;
    }
    if (!plan.want) {
        ndiClockArmed_ = false;     // the feed thread stops its own clock
        return;
    }
    if (ndiClockArmed_)
        return;                     // the feed's clock is already running
    ensureNdiWorker();
    if (!ndiWorker_)
        return;
    ndiClockArmed_ = true;
    // The clock is CREATED on the feed thread (a QTimer belongs to the thread
    // that runs it) and armed there; its interval follows the plan from inside
    // ndiWorkerTick, so the GUI never has to reach in twice. The arm is queued
    // once per feed, not once per poll: the plan itself is what republishes.
    QMetaObject::invokeMethod(ndiWorker_, [this] {
        if (!ndiSendTimer_) {
            ndiSendTimer_ = new QTimer(ndiWorker_);
            ndiSendTimer_->setTimerType(Qt::PreciseTimer);
            connect(ndiSendTimer_, &QTimer::timeout, ndiWorker_, [this] { ndiWorkerTick(); });
        }
        if (!ndiSendTimer_->isActive())
            ndiSendTimer_->start(0);   // the tick itself sets the real interval
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// The NDI send clock — paced at the FIRST enabled NDI output's configured
// Refresh rate (Settings · Outputs). pushNdiFrame used to ride the 10 Hz
// GUI poll, which made that select decorative: the feed ran at the poll's
// rate whatever the setting said. Now the setting IS the cadence — the
// timer's interval is derived from the roster value, and the provider
// advertises the same number in every frame's metadata, so receivers'
// frame clocks finally agree with both the setting and the real send rate.
// ---------------------------------------------------------------------------
float LiveOutputService::ndiConfiguredFps()
{
    const auto *model = OutputListModel::instance();
    if (!model)
        return 0.0f;
    for (int i = 0; i < model->rowCount(); ++i) {
        const QVariantMap out = model->getOutput(i);
        if (out.value("kind").toString() != QLatin1String("NDI")
            || !out.value("isEnabled").toBool())
            continue;
        // The row EXISTS — an unusable/blank refresh value falls back to
        // the engine's own default cadence rather than returning 0 (which
        // would stop the clock and kill the feed entirely), through THE
        // ENGINE'S OWN RULE (bps::display::AcceptedOutputFrameRate): the
        // provider advertises whatever that call accepts, so the clock has to
        // ask it rather than keep its own copy of the bound.
        const QString rate = out.value("refresh").toString();
        bool ok = false;
        const float fps = rate.left(rate.indexOf(QLatin1Char(' '))).toFloat(&ok);
        return bps::display::AcceptedOutputFrameRate(ok ? fps : 0.0f);
    }
    return 0.0f;   // no enabled NDI row at all
}

// aboutToQuit: retire the NDI worker thread BEFORE the engine's teardown —
// quitting the loop first means no new send job can start, and wait() joins
// whatever send is mid-flight. Without this the SDK/provider could be torn
// down under a running send (the class of quit-crash the indexer fix solved
// for The Table).
void LiveOutputService::shutdownNdiWorker()
{
    if (!ndiThread_)
        return;
    ndiThread_->quit();
    // BOUNDED join: quit() only stops the worker's event loop — a send already
    // RUNNING cannot be interrupted, and an NDI SDK send on a stalled network
    // can take arbitrarily long. An unbounded wait() froze the GUI thread at
    // quit (Windows: AppHangB1, process killed — "it crashes when I close
    // it"), so bound it.
    if (!ndiThread_->wait(2000)) {
        // Still mid-send. Do NOT delete the thread/worker: destroying them
        // under a running send is exactly the quit-crash the join was added to
        // prevent. Leak both deliberately — the process is exiting, and the
        // thread dies with it. Re-parent so nothing here dangles.
        ndiWorker_->setParent(nullptr);
        ndiThread_->setParent(nullptr);
        ndiWorker_ = nullptr;
        ndiThread_ = nullptr;
        ndiSendTimer_ = nullptr;   // its thread owns it; the process is exiting
        ndiClockArmed_ = false;
        qWarning("LiveOutputService: NDI send worker did not stop within 2s — leaving it detached rather than hanging the UI");
        return;
    }
    // The worker's own QTimer dies with it (delete ndiWorker_ destroys its
    // children) — drop the raw pointer with the object it belonged to.
    ndiSendTimer_ = nullptr;
    delete ndiWorker_;      // plain context object; its thread is gone
    ndiWorker_ = nullptr;
    delete ndiThread_;      // `this`-parented, but explicit for clarity
    ndiThread_ = nullptr;
    ndiClockArmed_ = false;
}

void LiveOutputService::startNdiSendClock()
{
    // Bring the feed thread up and publish the current plan immediately: the
    // 10 Hz poll would otherwise leave the opening frame waiting for its first
    // tick. WHAT to send is resolved there (the roster is GUI-owned); WHEN is
    // the feed thread's own clock from that point on.
    ensureNdiWorker();
    pushNdiFrame();
}

void LiveOutputService::stopNdiSendClock()
{
    // An empty plan is the signal: the feed thread skips and sleeps its own
    // clock on the next beat (see ndiWorkerTick) — no cross-thread stop call is
    // needed, and none could race the plan it is stopping. The feed's own
    // pacing clock is cleared there, on its own thread (nothing here touches a
    // member the feed thread owns).
    clearNdiPlan();
    // The drift watch's window dies with the clock — a fresh session must not
    // measure across the gap between two feeds.
    ndiDriftClock_.invalidate();
    ndiDriftSlowWindows_ = 0;
    ndiDriftWarned_ = false;
    ndiRateTier_ = -1;
    ndiLastConfiguredFps_ = 0.0f;
    setNdiWarming(false, QString());   // off air: nothing is warming
}

// The poll's NDI watch: publish WHAT to feed. Every beat of the 10 Hz poll
// re-resolves the first enabled NDI output (the roster is GUI-owned) and hands
// the feed thread a fresh value — so a row enabled mid-live, a rename, a
// disabled output or a changed Refresh rate all reach the wire within a poll,
// with no clock to re-arm and nothing to keep in sync by hand. pushNdiFrame's
// own !want paths publish an empty plan, which is what stops the feed.
void LiveOutputService::pollTickRecheckNdi()
{
    if (!live_)
        return;
    pushNdiFrame();
}

// ---- Output frame-rate watch -----------------------------------------------
// One measured number drives two things, both off the GUI thread's 10Hz poll
// (the counters live behind ndiStateMutex_, so nothing here touches the
// worker's state directly):
//
//  1. a RUNTIME RATE-CHANGE toast — the feed stepping DOWN to (about) half
//     under backpressure, or recovering to the configured rate. The halving
//     is implicit now (the feed's own pacing skips every beat a slow send
//     overruns, so a send held at its period or longer settles at about half
//     the setting; the old explicit half-rate flag was retired), so it is
//     observed from the actual send rate rather than a flag.
//  2. a SUSTAINED DRIFT warning for a rate that is neither the configured
//     rate nor a clean half — the "cannot keep up at all" case.
//
// A window whose CONFIGURED rate just changed is rebaselined silently: the
// Settings · Outputs refresh-rate toast already covers a settings-only
// change, so this watcher must not add a second toast for it.
void LiveOutputService::checkNdiFeedRate()
{
    const bool sending = ndiSending();
    const qulonglong frames = ndiFramesSent();

    // A stopped feed has no rate to measure: drop the window and every streak
    // so the next session starts clean and cannot inherit a toast.
    if (!sending) {
        ndiDriftClock_.invalidate();
        ndiDriftFrames_ = frames;
        ndiDriftSlowWindows_ = 0;
        ndiDriftWarned_ = false;
        ndiRateTier_ = -1;
        ndiLastConfiguredFps_ = 0.0f;
        return;
    }

    if (!ndiDriftClock_.isValid()) {
        ndiDriftFrames_ = frames;   // first sample establishes the baseline
        ndiDriftClock_.start();
        return;
    }
    const qint64 elapsed = ndiDriftClock_.elapsed();
    if (elapsed < kDriftWindowMs)
        return;

    // Roll the window (defensive on the counter: it never decreases, but a
    // provider reset mid-session would, and must not read as a negative rate).
    const qulonglong delta = frames >= ndiDriftFrames_ ? frames - ndiDriftFrames_ : 0;
    ndiDriftFrames_ = frames;
    ndiDriftClock_.restart();

    const float configured = ndiConfiguredFps();
    if (configured <= 0.0f) {
        ndiRateTier_ = -1;
        ndiLastConfiguredFps_ = 0.0f;
        return;
    }

    const double measured = double(delta) * 1000.0 / double(elapsed);

    // Did the SETTING move since the last window? (Compared before we store
    // it.) True means the rate change came from Settings, not the wire.
    const bool configuredChanged = ndiLastConfiguredFps_ > 0.0f
                                   && !qFuzzyCompare(configured, ndiLastConfiguredFps_);
    ndiLastConfiguredFps_ = configured;

    const QString name = ndiSendingOutputName();
    const QString label = name.isEmpty() ? QStringLiteral("NDI") : name;

    // ---- 1. runtime rate-change toast (on a real step) -------------------
    const int tier = measured >= configured * 0.75 ? 0
                   : (measured >= configured * 0.35 && measured <= configured * 0.7 ? 1 : 2);
    const int previousTier = ndiRateTier_;
    ndiRateTier_ = tier;
    if (!configuredChanged && previousTier >= 0 && tier != previousTier) {
        if (previousTier == 0 && tier == 1) {
            EventBus::instance().notify(
                QStringLiteral("NDI feed '%1' dropped to about %2 fps (about half of %3 Hz) — the sender is running behind. It returns to full rate automatically.")
                    .arg(label).arg(qRound(measured)).arg(qRound(configured)),
                QStringLiteral("warning"), QStringLiteral("Outputs"),
                QStringLiteral("outputs.ndi.rateReduced"));
        } else if (previousTier == 1 && tier == 0) {
            EventBus::instance().notify(
                QStringLiteral("NDI feed '%1' is back at the configured rate (about %2 Hz).")
                    .arg(label).arg(qRound(configured)),
                QStringLiteral("success"), QStringLiteral("Outputs"),
                QStringLiteral("outputs.ndi.rateRestored"));
        }
    }

    // ---- 2. sustained drift warning --------------------------------------
    // A clean backoff half (tier 1) is already announced by the change toast
    // above, so the warning here is reserved for the genuinely-stuck feed
    // (tier 2), sustained across consecutive windows.
    if (tier != 2) {
        ndiDriftSlowWindows_ = 0;
        ndiDriftWarned_ = false;
        return;
    }
    if (++ndiDriftSlowWindows_ < kDriftWindowsToWarn || ndiDriftWarned_)
        return;
    ndiDriftWarned_ = true;
    EventBus::instance().notify(
        QStringLiteral("NDI output '%1' is running at about %2 fps but is set to %3 Hz — the feed can't keep up. Lower the refresh rate or check the machine's load.")
            .arg(label).arg(qRound(measured)).arg(qRound(configured)),
        QStringLiteral("warning"), QStringLiteral("Outputs"),
        QStringLiteral("outputs.framerate.drift"));
}

// The network-visible NDI source name: "<AppName> - <Row name>"
// ("VGRPresenter - New Screen 2") — how Studio Monitor lists a machine's sources — falling
// back to the bare row name when the app name is unavailable, and to "" (the
// provider's own "VGR Program" default) when the row has no name at all.
QString LiveOutputService::ndiSenderDisplayName(const QString &rowName)
{
    QString resolved = rowName;
    if (resolved.isEmpty()) {
        const auto *model = OutputListModel::instance();
        if (!model)
            return {};
        for (int i = 0; i < model->rowCount(); ++i) {
            const QVariantMap out = model->getOutput(i);
            if (out.value("kind").toString() == QLatin1String("NDI")
                && out.value("isEnabled").toBool()) {
                resolved = out.value("name").toString();
                break;
            }
        }
    }
    if (resolved.isEmpty())
        return {};
    const QString app = QCoreApplication::applicationName();
    return app.isEmpty() ? resolved : QStringLiteral("%1 - %2").arg(app, resolved);
}

// Per-live-session NDI sender identity (see the header comment): NDI's SDK
// fixes a sender's name at creation, so the name that reached the network
// LAST session would stick forever — the receiver saw "VGR Program" (the
// self-test's name, or the default) instead of this output's row name.
// Recreating the sender on every GO LIVE lets each session open under its
// output's CURRENT name. A no-op when nothing changed since the last arm.
void LiveOutputService::armNdiSenderForSession()
{
    const QString name = ndiSenderDisplayName();
    if (name == ndiArmedName_)
        return;   // same output, same name — the sender can keep its identity
    ndiArmedName_ = name;
    auto *provider = []() -> bps::display::NdiDisplayProvider * {
        auto p = bps::display::DisplayEngine::Instance().Provider("Ndi");
        return p ? dynamic_cast<bps::display::NdiDisplayProvider *>(p.get()) : nullptr;
    }();
    if (!provider)
        return;
    // Name + reset ride the WORKER thread (FreeShow's model: the main thread
    // never touches the SDK): ResetSender destroys the old SDK sender — a
    // synchronous SDK call that on the GUI thread stalled GO LIVE — and the
    // next send on the worker recreates it under the new name. The worker is
    // created here if absent so the reset lands after any queued job on a
    // live session (event-loop order keeps the sequence right).
    if (!ndiThread_) {
        ndiThread_ = new QThread(this);
        ndiThread_->setObjectName("ndi-send");
        ndiWorker_ = new QObject();
        ndiWorker_->moveToThread(ndiThread_);
        ndiThread_->start();
    }
    const QString senderName = name.isEmpty() ? QStringLiteral("VGR Program") : name;
    QMetaObject::invokeMethod(ndiWorker_, [this, provider, name, senderName]() {
        if (!name.isEmpty())
            provider->SetSenderName(name.toStdString());
        provider->ResetSender();
        qCWarning(lcNdiProgress, "LiveOutputService: NDI sender reset for this live session — it will broadcast as '%s' once frames flow",
                 qUtf8Printable(senderName));
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// The preview frame feed
// ---------------------------------------------------------------------------
QImage LivePreviewProvider::requestImage(const QString &id, QSize *size,
                                         const QSize &requestedSize)
{
    // `id` is the URL path: "" = the shared PREVIEW feed (every tile's
    // default), "__out_<hash>__" = ONE output's own gated buffer (the
    // per-output pass the engine renders for that output's style). Strip the
    // ?query, resolve the buffer, read its last frame.
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
                QImage ref(reinterpret_cast<const uchar *>(frame->pixels.data()),
                           frame->width, frame->height,
                           frame->width * 4, QImage::Format_RGBA8888);
                // The engine packs colors 0xAABBGGRR (Color::Pack — R in the
                // LOW byte). Format_RGBA8888 reads bytes R,G,B,A — exactly
                // that layout on little-endian. (ARGB32 was WRONG here: it
                // reads BGRA in memory and swapped red/blue.) Copy: the
                // frame's pixels die with the lock inside LastFrame().
                out = ref.copy();
            }
        }
    }

    // ONE-SHOT FRAME SANITY, per buffer per STATE — never per request (this
    // runs at the tiles' frame cadence). It lives in the provider because the
    // provider is the ONLY place that sees the ENGINE's own pixels, and it is
    // what separates the two halves of "the outputs are black":
    //   * allBlack=1 (or "no frame") here ⇒ the engine really is rendering a
    //     black frame — the fault is upstream of every output, in the scene;
    //   * a real picture here with black NDI/HDMI ⇒ the engine is fine and the
    //     output path is dropping it (send/present), not the render.
    // Sampled with a stride (~16x16 samples), so it costs nothing per frame.
    {
        static std::mutex sanityMutex;
        static std::map<std::string, std::string> lastSanity;
        std::string state;
        if (out.isNull()) {
            state = "no frame";
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
            state = std::format("{}x{} meanLuma={:.1f} maxLuma={} maxAlpha={} allBlack={}",
                                out.width(), out.height(), mean, maxLuma, maxAlpha,
                                maxLuma == 0 ? "yes" : "no");
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(sanityMutex);
            std::string &last = lastSanity[bufferName];
            if (last != state) {
                last = state;
                changed = true;
            }
        }
        if (changed)
            qInfo("livepreview frame sanity: buffer '%s' — %s", bufferName.c_str(), state.c_str());
    }

    if (out.isNull()) {
        // Not live (or no frame yet): 1x1 transparent — the QML side reads it
        // as "no picture" the same way MediaTile does (no warning spam).
        out = QImage(1, 1, QImage::Format_ARGB32);
        out.fill(Qt::transparent);
    }

    if (requestedSize.width() > 0 && requestedSize.height() > 0)
        out = out.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (size)
        *size = out.size();
    return out;
}
