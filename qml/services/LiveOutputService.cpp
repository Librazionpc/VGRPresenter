#include "services/LiveOutputService.h"

#include "services/ShowConverter.h"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SceneBuilder.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include <QTimer>

namespace pl = bps::presentation;
namespace pr = bps::rendering;

namespace {

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
// slideContentType. Empty = untagged (the style's template then applies).
std::string slideContentType(const pl::Slide *slide)
{
    if (auto meta = bps::json::Parse(slide->metaJson); meta.ok())
        if (const bps::json::Value *v = meta.value().Find("contentType"); v && v->type() == bps::json::Value::Type::String)
            return std::string(v->asString());
    return {};
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

    // Per-family rule, mirrored from SceneBuilder: the style's template only
    // restyles its own contentType (a scripture-keyed style must not
    // steamroll The Table's tab-template layout) — otherwise the slide's
    // own blocks carry the look.
    const std::string slideType = slideContentType(slide);
    const bool styleTemplateApplies = !style.templateBlocks.empty()
        && (style.contentType.empty() || slideType.empty() || slideType == style.contentType);

    if (styleTemplateApplies) {
        const QString styleBg = QString::fromStdString(style.backgroundColor);
        if (!styleBg.isEmpty() && styleBg != QLatin1String("transparent")
            && (background.isEmpty() || background == QLatin1String("transparent")
                || !style.clearBackgroundOnText))
            background = styleBg;
        for (const pl::ContentBlock &b : style.templateBlocks) {
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
    auto r = bps::live::LiveOutputController::Instance().StartFromOpenShow();
    if (!r.ok()) {
        // Surface the honest reason (no show open / no slides) as a property,
        // not a silent no-op — the Show screen toasts on it.
        qWarning("LiveOutputService: goLive failed: %s", r.error().message.c_str());
        emit onAirChanged();
        return;
    }

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
        emit onAirChanged();
        return;
    }
    auto r = bps::live::LiveOutputController::Instance().StartFromSlides(name.toStdString(),
                                                                         content.slides);
    if (!r.ok()) {
        qWarning("LiveOutputService: goLiveWithSlides failed: %s", r.error().message.c_str());
        emit onAirChanged();
        return;
    }
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

void LiveOutputService::stop()
{
    bps::live::LiveOutputController::Instance().StopLive();
    if (live_) {
        live_ = false;
        emit liveChanged();
    }
    // Off air: the slide preview goes with it (also on a failed start).
    onAirSlide_ = QVariantMap{};
    emit onAirChanged();
    if (poll_)
        poll_->stop();
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

// ---------------------------------------------------------------------------
// Poll (onAir titles + frame counter while live)
// ---------------------------------------------------------------------------
void LiveOutputService::pollTick()
{
    if (!live_)
        return;

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
