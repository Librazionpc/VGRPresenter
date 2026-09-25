#include "services/LiveOutputService.h"

#include "services/ShowConverter.h"
#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include <QTimer>

namespace pl = bps::presentation;
namespace pr = bps::rendering;

namespace {

// One slide as the QML preview shape: { valid, title, blocks, background } —
// the same { blocks, background } maps DesignPreview draws everywhere else
// (ShowConverter::blockToVariant is the single QML<->engine block mapping).
QVariantMap slideToVariantMap(const pl::Slide *slide)
{
    if (!slide)
        return {};
    QVariantList blocks;
    for (const pl::ContentBlock &b : slide->blocks)
        blocks.append(ShowConverter::blockToVariant(b));
    return QVariantMap{
        { QStringLiteral("valid"), true },
        { QStringLiteral("title"), QString::fromStdString(slide->title) },
        { QStringLiteral("blocks"), blocks },
        { QStringLiteral("background"), QString::fromStdString(slide->background) },
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
    Q_UNUSED(id)   // only the cache-busting query matters

    // The engine-side Preview output holds the last distributed frame (the
    // same frame the real outputs received). Grab the output by name.
    static const char *kPreviewName = "__live_preview__";
    QImage out;

    if (auto o = pr::RenderEngine::Instance().GetOutput(kPreviewName); o.ok()) {
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
