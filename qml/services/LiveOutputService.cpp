#include "services/LiveOutputService.h"

#include "services/ShowConverter.h"
#include "services/DesignLibraryService.h"

#include <QSet>
#include "modules/presentation/CompositorState.hpp"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
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
#include <QAudioOutput>
#include <QFile>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <cstring>
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
    if (frame.isNull()) {
        pl::CompositorState::Instance().ClearMedia();
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

// The slide's CONTENT text — what a bound "text" block shows. Mirrors
// SceneBuilder's SlideContentText: slide.text first; when empty (content
// tabs go live with the verse riding in their OWN template's blocks and
// only the reference in `title`), the first text-bearing slide block's
// text; else the title.
std::string slideContentText(const pl::Slide *slide)
{
    if (!slide->text.empty())
        return slide->text;
    for (const pl::ContentBlock &b : slide->blocks)
        if (b.kind == "text" && !b.text.empty() && b.text.front() != '{')
            return b.text;
    return slide->title;
}

// The slide's content family ("scripture" | "table" | …), as tagged by
// ShowConverter in the slide's meta — mirrors SceneBuilder's
// slideContentType. Empty = untagged = SHOWS content (it follows the shows
// family slot).
std::string slideContentType(const pl::Slide *slide)
{
    if (auto meta = bps::json::Parse(slide->metaJson); meta.ok())
        if (const bps::json::Value *v = meta.value().Find("contentType"); v && v->type() == bps::json::Value::Type::String)
            return std::string(v->asString());
    return {};
}

// The OutputStyleSpec::familyTemplateKeys/Blocks slot a content family maps
// to — EXACTLY SceneBuilder's own FamilyTemplateIndexFor (shows | media |
// scripture | table, in the show* gates' order). Untagged slides are SHOWS
// content (imported/Quick-Lyrics slides carry no tag) — they follow the
// shows slot; only an unknown tag falls through to 4.
size_t familyTemplateIndexFor(const std::string &contentType)
{
    if (contentType == "media")     return 1;
    if (contentType == "scripture") return 2;
    if (contentType == "table")     return 3;
    return 0;   // "shows" — and untagged slides, which are shows content
}

// One slide as the QML preview shape: { valid, title, blocks, background } —
// the same { blocks, background } maps DesignPreview draws everywhere else
// (ShowConverter::blockToVariant is the single QML<->engine block mapping).
//
// STYLE COMPOSITION, mirrored from SceneBuilder::BuildSlideScene so the
// monitor tile shows what the OUTPUT shows: when the active output's style
// wears an engine template (baked blocks in the spec), the preview composes
// THE STYLE'S template (slide content bound in, unfilled {scripture_*}
// placeholders blanked) INSTEAD of the slide's own blocks, and the style's
// background colour sits UNDER the slide's own (a slide background still
// wins unless it is unset). A template-less style leaves the slide as-is
// and the tile's own colour fallback handles the rest.
QVariantMap slideToVariantMap(const pl::Slide *slide)
{
    if (!slide)
        return {};

    const pl::OutputStyleSpec style = pl::PresentationEngine::Instance().ActiveOutputStyle();
    QString background = QString::fromStdString(slide->background);
    QVariantList blocks;

    // Per-family rule, mirrored from SceneBuilder: a FAMILY-specific template
    // (the Styles screen's own "Template for Shows/Media/Scripture/Table"
    // row — style.familyTemplateBlocks[familyIdx]) wins outright when the
    // style carries one for this slide's family; otherwise fall back to the
    // whole-style default (style.templateBlocks, gated by contentType so a
    // scripture-keyed style doesn't steamroll The Table's own layout);
    // otherwise the slide's own blocks carry the look. Previously this only
    // ever checked the whole-style default — a per-family pick (e.g. "Big"
    // set as the Shows family's template) silently never reached this
    // preview, even though SceneBuilder's real engine-composited frame
    // already honored it correctly.
    // A genuinely blank slide (the "go live with nothing" fallback: no
    // blocks, no title, no text) must NOT bake a family/whole-style template
    // in — doing so produced non-empty `blocks` (the template's own bound-
    // to-nothing decorative blocks) for content that is really empty,
    // which made hasSlidePreview read true downstream and suppressed the
    // style's own idle background image in its place — reported live as
    // "go live with nothing shows a black window instead of the bg."
    const bool slideHasContent = !slide->blocks.empty() || !slide->title.empty()
        || !slide->text.empty();
    const std::string slideType = slideContentType(slide);
    const size_t familyIdx = familyTemplateIndexFor(slideType);
    const std::vector<pl::ContentBlock> *activeTemplateBlocks = nullptr;
    if (slideHasContent) {
        if (familyIdx < 4 && !style.familyTemplateBlocks[familyIdx].empty())
            activeTemplateBlocks = &style.familyTemplateBlocks[familyIdx];
        else if (!style.templateBlocks.empty()
                 && (style.contentType.empty() || slideType.empty() || slideType == style.contentType))
            activeTemplateBlocks = &style.templateBlocks;
    }

    if (activeTemplateBlocks) {
        const QString styleBg = QString::fromStdString(style.backgroundColor);
        if (!styleBg.isEmpty() && styleBg != QLatin1String("transparent")
            && (background.isEmpty() || background == QLatin1String("transparent")
                || !style.clearBackgroundOnText))
            background = styleBg;
        for (const pl::ContentBlock &b : *activeTemplateBlocks) {
            pl::ContentBlock bound = b;
            if (b.kind == "text") {
                if (!b.bind.empty()) {
                    std::string value = pl::SlideResolver::BoundValue(*slide, b.bind);
                    if (value.empty()) {
                        if (b.bind == "text")
                            value = slideContentText(slide);
                        else if (b.bind == "ref")
                            value = slide->title;
                    }
                    bound.text = std::move(value);
                } else if (b.text.find('{') != std::string::npos) {
                    bound.text.clear();
                }
            }
            blocks.append(ShowConverter::blockToVariant(bound));
        }
    } else {
        for (const pl::ContentBlock &b : slide->blocks)
            blocks.append(ShowConverter::blockToVariant(b));
    }

    return QVariantMap{
        { QStringLiteral("valid"), true },
        { QStringLiteral("title"), QString::fromStdString(slide->title) },
        { QStringLiteral("blocks"), blocks },
        { QStringLiteral("background"), background },
    };
}

} // namespace

LiveOutputService *LiveOutputService::s_instance = nullptr;

LiveOutputService::LiveOutputService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
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
    // A staged ad-hoc pick (stageSlides() — a slide double-clicked in
    // ShowCenter, say) wins over the open document: the whole point of
    // staging is that the user's next real GO LIVE press commits whatever
    // is sitting in the tile right now, not whatever happens to be open
    // in the editor behind it.
    if (!stagedSlidesRaw_.isEmpty()) {
        const QString name = stagedName_;
        const QVariantList slides = stagedSlidesRaw_;
        stagedName_.clear();
        stagedSlidesRaw_.clear();
        stagedSlide_ = QVariantMap{};
        emit stagedChanged();
        goLiveWithSlides(name, slides);
        return;
    }

    auto r = bps::live::LiveOutputController::Instance().StartFromOpenShow();
    if (!r.ok()) {
        // No open document (nothing staged either, or this would already
        // have returned above) — rather than refuse outright, go live with
        // a single BLANK slide instead: the active output's own style
        // (background colour/image) composites and shows on the real
        // output the SAME way it would under real content, since
        // SceneBuilder paints the style's background unconditionally,
        // before/regardless of any blocks. A holding screen with the
        // church's own branding before a service starts is a real, common
        // need — GO LIVE with nothing queued should show that, not an
        // error toast and a black screen.
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
        pollTick();
    }
}

// Any-content go-live: convert the QML slide maps to engine Slides and hand
// them to the controller. A failed start surfaces through the same qWarning +
// onAirChanged path as goLive (the UI toasts off the failure reason).
void LiveOutputService::goLiveWithSlides(const QString &name, const QVariantList &slides)
{
    bps::presentation::Presentation content;
    content.name = name.toStdString();
    for (const QVariant &v : slides)
        content.slides.push_back(ShowConverter::slideFromVariant(v.toMap()));
    if (content.slides.empty()) {
        qWarning("LiveOutputService: goLiveWithSlides: no slides in '%s'", name.toUtf8().constData());
        EventBus::instance().notify(QStringLiteral("Nothing selected to put on air"),
                                    QStringLiteral("warning"), QStringLiteral("Go Live"));
        emit onAirChanged();
        return;
    }
    auto r = bps::live::LiveOutputController::Instance().StartFromSlides(name.toStdString(),
                                                                         content.slides);
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
    refreshOnAirSlide();
    emit onAirChanged();
    if (!poll_) {
        poll_ = new QTimer(this);
        poll_->setInterval(100);
        connect(poll_, &QTimer::timeout, this, &LiveOutputService::pollTick);
    }
    if (!poll_->isActive()) {
        poll_->start();
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
    if (kind == QLatin1String("screen"))
        EngineBridge::instance().startScreenPreview(label, QStringLiteral("card"));
    else if (kind == QLatin1String("ndi"))
        EngineBridge::instance().startNdiPreview(label, QStringLiteral("card"));
    else
        EngineBridge::instance().startVideoPreview(label, mode, QStringLiteral("card"));
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
    inputLabel_.clear();
    inputKind_.clear();
    inputLive_ = false;
    producing_.remove(inputLabel_);
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
    bps::live::LiveOutputController::Instance().StopLive();
    if (live_) {
        live_ = false;
        emit liveChanged();
    }
    // Off air: the slide preview goes with it (also on a failed start).
    onAirSlide_ = QVariantMap{};
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
    // The NDI feed rides the poll — off air it stops with it.
    stopNdiFeed();
    // An NDI INPUT taken while going off air also stops feeding the engine's
    // compositor (the pump stops with it — one last explicit clear keeps the
    // layer honest if the take is ever re-taken without clearInput first).
    clearNdiInputFrame();
}

void LiveOutputService::goLiveBlank()
{
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
        // audio itself plays through the system's default output device,
        // which is exactly what the output tile's L/R meters already
        // capture (EngineBridge.outputLevels, a real WASAPI loopback tap on
        // that same default device) — those bars move on their own once
        // this actually plays, no separate wiring needed.
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
    if (!live_)
        return;
    const bool moved = direction < 0 ? previous() : next();
    if (!moved)
        emit passageStepRequested(direction);
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
void LiveOutputService::refreshOnAirSlide()
{
    onAirSlide_ = live_ ? slideToVariantMap(pl::PresentationEngine::Instance().CurrentSlide())
                        : QVariantMap{};
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
// tagging (slideContentType) reads it off pl::Slide::metaJson.
QVariantMap LiveOutputService::previewWithActiveStyle(const QVariantMap &slide) const
{
    const pl::Slide engineSlide = ShowConverter::slideFromVariant(slide);
    return slideToVariantMap(&engineSlide);
}

// ---------------------------------------------------------------------------
// Poll (onAir titles + frame counter while live)
// ---------------------------------------------------------------------------
void LiveOutputService::pollTick()
{
    if (!live_)
        return;

    // NDI monitor-connection telemetry rides the live poll (the bridge
    // caps itself at ~1 Hz internally and emits only on change).
    EngineBridge::instance().refreshNdiConnections();

    auto &ctrl = bps::live::LiveOutputController::Instance();
    const qulonglong sent = ctrl.FramesSent();
    if (sent != framesSent_) {
        framesSent_ = sent;
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
    // read, so what the network gets is what the wall shows, at the poll's
    // 10 Hz.
    pushNdiFrame();

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
        if (next != onAirSlide_) {
            onAirSlide_ = next;
            emit onAirChanged();
        }
    }
}

// ---------------------------------------------------------------------------
// The NDI program sender — the display engine's NdiDisplayProvider IS the
// output ("ndi-program" device, real NDI SDK via the BroadcastEngine with an
// automatic software-loopback fallback). This reads the FIRST enabled NDI
// output's frame (its own styled buffer, or the shared preview feed for an
// unstyled output — the same source the monitor tile shows) and hands it to
// the provider on every poll while live. The provider sends the engine's native RGBA unconverted ('RGBA'
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
    // shows nothing" has a reason in the log.
    if (want && !EngineBridge::instance().ndiAvailable()) {
        if (!ndiGateLogged_) {
            ndiGateLogged_ = true;
            qWarning("LiveOutputService: NDI output requested but NDI is not usable (%s) — no frames will be sent",
                     qUtf8Printable(EngineBridge::instance().ndiStatus()));
        }
    } else {
        ndiGateLogged_ = false;
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
        return;
    }

    // The frame: THIS output's own gated buffer when styled, else the
    // shared preview feed (FrameBufferRole's exact keying — qHash(name)).
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
            qWarning("LiveOutputService: NDI output '%s' waiting for buffer '%s' (live loop warming up)",
                     qUtf8Printable(ndiOutputName), buffer.c_str());
        }
        if (ndiSending_) {
            ndiSending_ = false;
            ndiSendingOutput_.clear();
            emit ndiChanged();
        }
        return;
    }
    auto *fb = dynamic_cast<pr::FrameBufferOutput *>(out.value().get());
    if (!fb || !fb->Enabled())
        return;
    const pr::Frame frame = fb->LastFrame();
    if (frame.empty()) {
        if (!ndiWaitLogged_) {
            ndiWaitLogged_ = true;
            qWarning("LiveOutputService: NDI output waiting for the live loop's first rendered frame");
        }
        return;
    }

    // The Output row's own name IS the network-visible NDI source name —
    // SetSenderName only takes effect before the sender's first SendFrame
    // (NDI can't rename a live sender; the SDK identity is fixed at
    // creation), so the sender is recreated per live session via
    // armNdiSenderForSession() and the name is re-asserted every tick ahead
    // of the send (catches a mid-session rename of the roster row: the next
    // session picks it up). A blank name falls back to the provider's own
    // default ("VGR Program") rather than broadcasting under an empty
    // string.
    const QString senderName = ndiSenderDisplayName(ndiOutputName);
    if (!senderName.isEmpty())
        provider->SetSenderName(senderName.toStdString());

    // The provider owns the wire format + send; frames only flow while live
    // (pollTick gates this call).
    //
    // FreeShow-style send backpressure, adapted to a SYNCHRONOUS sender:
    // their grandiose worker caps in-flight encodes (MAX_INFLIGHT_SENDS=3);
    // our GUI-thread send never queues, but a SLOW one would stall this
    // thread — so the send is clocked and, past half the poll budget, the
    // NEXT tick is skipped (half rate) and the incident logged once. The
    // skip has a hard ceiling: a pathological sender can halve the feed,
    // never stop it.
    if (ndiBackoff_) {
        ndiBackoff_ = false;
        return;
    }
    bps::display::RenderFrameView view;
    view.width = frame.width;
    view.height = frame.height;
    view.pixels = frame.pixels.data();
    if (!ndiClockStarted_) {
        ndiSendClock_.start();
        ndiClockStarted_ = true;
    }
    ndiSendClock_.restart();
    const bool sent = provider->SendFrame(view).ok();
    const qint64 sendMs = ndiSendClock_.elapsed();
    if (sent) {
        ndiSendingOutput_ = ndiOutputName;   // the pill's LIVE row (re-asserted per frame: rename-safe)
        if (!ndiSending_) {
            ndiSending_ = true;
            qWarning("LiveOutputService: NDI program sending started (%s)",
                     provider->SenderName().c_str());   // qWarning: the engine log sink keeps WARN+ only
        }
        ndiWaitLogged_ = false;   // frames flow — the waiting notes re-arm
        ndiFramesSent_ = provider->FramesSent();
        emit ndiChanged();
        // A later failure logs again — success clears the outage flag
        // instead of it staying permanently tripped after the first one.
        ndiSendFailedLogged_ = false;
        // First-frame diagnostics: mDNS discovery (Studio Monitor's source
        // list) can take tens of seconds; without this line "the output is
        // not working" vs "my monitor just hasn't found the sender yet" is
        // indistinguishable from a launch log. Fires once, right after the
        // first successful send this outage.
        if (!ndiFirstFrameLogged_) {
            ndiFirstFrameLogged_ = true;
            qWarning("LiveOutputService: NDI frames flowing as '%s' — discovery on a receiving monitor can take 10-30s",
                     provider->SenderName().c_str());
        }
        // The backpressure trip: over half the poll's 100 ms budget in ONE
        // synchronous send. Skip the next tick (feed drops to ~5 fps) so the
        // GUI thread keeps breathing; logged once per incident.
        if (sendMs > 50) {
            ndiBackoff_ = true;
            if (!ndiSlowLogged_) {
                ndiSlowLogged_ = true;
                qWarning("LiveOutputService: NDI send is slow (%lld ms, sender '%s') — backing off to ~5 fps to keep the UI responsive",
                         sendMs, provider->SenderName().c_str());
            }
        } else {
            ndiSlowLogged_ = false;   // recovered — the next incident logs again
        }
    } else if (!ndiSendFailedLogged_) {
        ndiSendFailedLogged_ = true;
        qWarning("LiveOutputService: NDI SendFrame FAILED (sender '%s') — check the engine log for the provider's own reason",
                 provider->SenderName().c_str());
    }

    // Connected-monitor telemetry, ~1 Hz (every 10th tick): the SDK's own
    // connection count, logged on every CHANGE. During the green-screen
    // hunt this number was the missing witness — "2 monitor(s)" in the UI
    // but nothing in engine.log. Now: a monitor connecting logs
    // "monitors connected: N", and a drop to 0 while frames keep flowing
    // is exactly the discovery/firewall drop worth surfacing.
    if (++ndiTick_ % 10 == 0) {
        const int connected = bps::broadcast::BroadcastEngine::Instance()
                                  .SenderConnectedReceivers(provider->SenderId());
        if (connected != ndiReceiversSeen_) {   // -1 = provider can't tell — stays silent
            const bool first = ndiReceiversSeen_ < 0;
            ndiReceiversSeen_ = connected;
            qWarning("LiveOutputService: NDI sender '%s' %s %d monitor(s) connected",
                     provider->SenderName().c_str(),
                     first ? "currently has" : (connected > 0 ? "now has" : "has NO monitors left —"),
                     connected);
        }
    }
}

// Stop the feed when the loop stops (pollTick no longer runs).
void LiveOutputService::stopNdiFeed()
{
    if (ndiSending_) {
        ndiSending_ = false;
        ndiSendingOutput_.clear();
        emit ndiChanged();
    }
    // Reset the incident state: backpressure, its log latch, and receiver
    // telemetry all belong to the live session that just ended — the next
    // one starts clean (and re-reports its monitors from scratch).
    ndiBackoff_ = false;
    ndiSlowLogged_ = false;
    ndiReceiversSeen_ = -1;
}

// The network-visible NDI source name: "<AppName> . <Row name>" ("VGRPresenter
// . New Screen 2") — how Studio Monitor lists a machine's sources — falling
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
    return app.isEmpty() ? resolved : QStringLiteral("%1 . %2").arg(app, resolved);
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
    if (!name.isEmpty())
        provider->SetSenderName(name.toStdString());
    provider->ResetSender();   // recreate on the next SendFrame under the new name
    qWarning("LiveOutputService: NDI sender reset for this live session — it will broadcast as '%s' once frames flow",
             qUtf8Printable(name.isEmpty() ? QStringLiteral("VGR Program") : name));
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
            const pr::Frame frame = fb->LastFrame();
            if (!frame.empty()) {
                QImage ref(reinterpret_cast<const uchar *>(frame.pixels.data()),
                           frame.width, frame.height,
                           frame.width * 4, QImage::Format_RGBA8888);
                // The engine packs colors 0xAABBGGRR (Color::Pack — R in the
                // LOW byte). Format_RGBA8888 reads bytes R,G,B,A — exactly
                // that layout on little-endian. (ARGB32 was WRONG here: it
                // reads BGRA in memory and swapped red/blue.) Copy: the
                // frame's pixels die with the lock inside LastFrame().
                out = ref.copy();
            }
        }
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
