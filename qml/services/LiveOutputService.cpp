#include "services/LiveOutputService.h"

#include "modules/presentation/LiveOutputController.hpp"
#include "modules/presentation/PresentationEngine.hpp"
#include "modules/rendering/RenderOutputs.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include <QTimer>

namespace pl = bps::presentation;
namespace pr = bps::rendering;

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
        emit onAirChanged();
        if (!poll_) {
            poll_ = new QTimer(this);
            poll_->setInterval(250);   // 4Hz — enough for slide-title changes
            connect(poll_, &QTimer::timeout, this, &LiveOutputService::pollTick);
        }
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
        emit onAirChanged();
    }
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
        emit onAirChanged();
        return true;
    }
    return false;
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
        emit onAirChanged();
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
                           frame.width * 4, QImage::Format_ARGB32);
                // Pack order is 0xAABBGGRR (little-endian BGRA in memory) —
                // ARGB32 on a little-endian host reads exactly that byte
                // order, so a straight wrap is correct. Copy: the frame's
                // pixels die with the lock inside LastFrame().
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
