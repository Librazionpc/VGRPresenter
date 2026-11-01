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

// NDI progress uses a logging category so it stays in the engine log without
// becoming a toast. Actionable failures still use qWarning and reach the UI.
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcNdiProgress, "vgr.ndi.progress");

#include <utility>

namespace pl = bps::presentation;
namespace pr = bps::rendering;

namespace {

// File-scope metadata follows the media frame through the compositor.
std::string g_mediaFitOnAir;

// Computed once per take and copied with each frame; scanning every video frame
// for transparency would be needlessly expensive.
bool g_mediaHasAlphaOnAir = false;

// An alpha channel alone does not mean any pixel is transparent.
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

// The one label a taken file's meter lands under in EngineBridge::inputLevels
// (see applyMediaTake). Reserved: it names the take that is UP, not a roster row.
const QString kMediaMeterLabel = QStringLiteral("Media on air");

void pushMediaFrameToCompositor(const QImage &frame)
{
    // Log only state transitions; the paired SceneBuilder log distinguishes a
    // missing handoff from a frame the engine received but rendered incorrectly.
    static bool hadFrame = false;
    if (frame.isNull()) {
        pl::CompositorState::Instance().ClearMedia();
        if (hadFrame) {
            hadFrame = false;
            qInfo("media hand-off: cleared (no frame) — engine media layer will be blank");
        }
        return;
    }
    // The compositor expects tightly packed RGBA bytes.
    const QImage rgba = frame.format() == QImage::Format_RGBA8888
        ? frame : frame.convertToFormat(QImage::Format_RGBA8888);
    pl::CompositorState::MediaFrame mf;
    mf.width = static_cast<uint32_t>(rgba.width());
    mf.height = static_cast<uint32_t>(rgba.height());
    // Copy rows separately because QImage may pad bytesPerLine().
    mf.rgba.resize(static_cast<size_t>(mf.width) * mf.height * 4);
    for (int y = 0; y < rgba.height(); ++y) {
        std::memcpy(mf.rgba.data() + static_cast<size_t>(y) * mf.width * 4,
                    rgba.constScanLine(y), static_cast<size_t>(mf.width) * 4);
    }
    // The media item's fit takes precedence over the output default.
    mf.fit = g_mediaFitOnAir;
    // Carry the take's transparency result with the frame for all render surfaces.
    mf.hasAlpha = g_mediaHasAlphaOnAir;
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

// Preview composition and style specs come from the engine's shared rules;
// this file only maps the result to the QML preview shape.

// `slideTag` is the group the composed slide carries: a group that names a
// template composes through it (StyleSpecBuilder consolidates that rule), so the
// picture a tile draws is the one the output would render for the same slide.
pl::OutputStyleSpec outputStyleForPreview(const QString &styleId, const QString &slideTag = QString())
{
    return StyleSpecBuilder::specForStyleId(styleId, slideTag);
}

// Map the engine's slide composition to the QML preview shape. Keep layout and
// background decisions in SceneBuilder so preview and output stay consistent.
QVariantMap slideToVariantMap(const pl::Slide *slide, const pl::OutputStyleSpec &style)
{
    if (!slide)
        return {};
    const pl::SceneBuilder::SlideComposition comp =
        pl::SceneBuilder::ComposeSlide(*slide, style, /*applyGates=*/true);

    // Publish style facts from the same composition used by real outputs.
    const QString styleImage = QString::fromStdString(style.backgroundImage);
    const bool styleImageOnDisk = !styleImage.isEmpty() && QFile::exists(styleImage);
    // Empty and fully transparent colors do not paint.
    const bool stylePaintsColor = !style.backgroundColor.empty()
        && pl::StyleBuilder::ParseColor(style.backgroundColor).a > 0.0f;
    const bool stylePaints = stylePaintsColor || styleImageOnDisk;
    // A take suppresses the style background in both preview and output.
    const bool stylePaintless = comp.clearedByTake || !stylePaints
        || (styleImageOnDisk && comp.styleImageSuppressed);
    const bool styleImagePainted = !comp.clearedByTake && styleImageOnDisk
        && !comp.styleImageSuppressed;
    // Whether a distributed frame takes priority is part of the composition.
    const std::vector<pl::ContentBlock> &drawn =
        comp.source == pl::SceneBuilder::SlideComposition::Source::SlideBlocks
            ? slide->blocks : comp.blocks;
    const bool framePriority = pl::SlideRules::NeedsDistributedFrame(drawn);
    const QVariantMap styleFacts{
        // A take replaces the style color.
        { QStringLiteral("styleColor"), comp.clearedByTake
            ? QStringLiteral("transparent")
            : QString::fromStdString(style.backgroundColor) },
        { QStringLiteral("styleImage"), styleImage },
        // A take owns the frame, so the style paints nothing.
        { QStringLiteral("stylePaints"), stylePaints && !comp.clearedByTake },
        { QStringLiteral("stylePaintless"), stylePaintless },
        { QStringLiteral("styleImagePainted"), styleImagePainted },
        // Shared frame-priority decision; frame availability is tracked separately.
        { QStringLiteral("framePriority"), framePriority },
        // Preserve an explicit transparent choice separately from no color.
        { QStringLiteral("explicitTransparent"), comp.explicitTransparent },
        // All preview surfaces use the composition's transparency result.
        { QStringLiteral("frameTransparent"), comp.frameTransparent },
        // Clear style layers while a take owns the frame.
        { QStringLiteral("clearedByTake"), comp.clearedByTake },
        // Use the media item's fit, defaulting to contain; output fit is ignored.
        { QStringLiteral("mediaFit"), QString::fromStdString(
              g_mediaFitOnAir.empty() ? std::string("contain") : g_mediaFitOnAir) },
    };

    // Preserve explicit transparency; otherwise pass through the composed color.
    const QString background = comp.explicitTransparent
        ? QStringLiteral("transparent")
        : QString::fromStdString(comp.background);

    // A cleared slide has no style or slide content.
    if (comp.source == pl::SceneBuilder::SlideComposition::Source::Cleared) {
        QVariantMap cleared = styleFacts;
        cleared.insert(QStringLiteral("styleColor"), QStringLiteral("transparent"));
        cleared.insert(QStringLiteral("stylePaints"), false);
        cleared.insert(QStringLiteral("stylePaintless"), true);
        cleared.insert(QStringLiteral("styleImagePainted"), false);
        // Cleared frames are transparent.
        cleared.insert(QStringLiteral("frameTransparent"), true);
        cleared.insert(QStringLiteral("valid"), true);
        cleared.insert(QStringLiteral("title"), QString());
        cleared.insert(QStringLiteral("contentType"), QString::fromStdString(comp.contentType));
        cleared.insert(QStringLiteral("blocks"), QVariantList{});
        cleared.insert(QStringLiteral("background"), QStringLiteral("transparent"));
        cleared.insert(QStringLiteral("suppressStyleImage"), true);
        // Let the preview distinguish an intentional clear from a failure.
        cleared.insert(QStringLiteral("cleared"), true);
        // Preserve the clear marker when the preview is converted back to a slide.
        cleared.insert(QStringLiteral("clearStage"), true);
        return cleared;
    }

    if (comp.source == pl::SceneBuilder::SlideComposition::Source::Refused) {
        QVariantMap refused = styleFacts;
        refused.insert(QStringLiteral("valid"), true);
        refused.insert(QStringLiteral("title"), QString());
        refused.insert(QStringLiteral("contentType"), QString::fromStdString(comp.contentType));
        refused.insert(QStringLiteral("blocks"), QVariantList{});
        refused.insert(QStringLiteral("background"), background);
        // Apply the same style-image rule as the engine.
        refused.insert(QStringLiteral("suppressStyleImage"), comp.styleImageSuppressed);
        // Explain the intentional refusal in the preview.
        refused.insert(QStringLiteral("refusedByStyle"), true);
        return refused;
    }

    QVariantList blocks;
    using Source = pl::SceneBuilder::SlideComposition::Source;
    if (comp.source == Source::FamilyTemplate || comp.source == Source::StyleTemplate) {
        // The style's template, with the slide's content already bound in.
        blocks = ShowConverter::blocksToVariant(comp.blocks);
    } else if (comp.source == Source::FamilyPreset || comp.source == Source::StylePreset) {
        // Convert preset regions to preview text blocks using the engine's
        // default 24-pixel output font scaled to the preview canvas.
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
        // Slide blocks already define the layout.
        blocks = ShowConverter::blocksToVariant(slide->blocks);
    }

    QVariantMap composed = styleFacts;
    composed.insert(QStringLiteral("valid"), true);
    composed.insert(QStringLiteral("title"), QString::fromStdString(slide->title));
    composed.insert(QStringLiteral("contentType"), QString::fromStdString(comp.contentType));
    composed.insert(QStringLiteral("blocks"), blocks);
    composed.insert(QStringLiteral("background"), background);
    // Use the engine's background suppression decision, including blank carriers.
    composed.insert(QStringLiteral("suppressStyleImage"), comp.styleImageSuppressed);
    // Preserve body and fill when preview surfaces convert this map back to a slide.
    composed.insert(QStringLiteral("body"), QString::fromStdString(slide->text));
    composed.insert(QStringLiteral("fill"),
                    ShowConverter::fillFromMeta(ShowConverter::metaOf(*slide)));
    composed.insert(QStringLiteral("clearStage"), slide->clearStage);
    return composed;
}

QVariantMap slideToVariantMap(const pl::Slide *slide)
{
    return slideToVariantMap(slide, pl::PresentationEngine::Instance().ActiveOutputStyle());
}

// One NDI program sender exists today. Honor the row the operator marked LIVE
// when that row is NDI; otherwise keep the first enabled NDI row as the
// sidecar feed while HDMI (or another output) is active.
QVariantMap preferredNdiOutput(const OutputListModel *model)
{
    if (!model)
        return {};
    QVariantMap firstEnabled;
    for (int i = 0; i < model->rowCount(); ++i) {
        QVariantMap out = model->getOutput(i);
        if (out.value("kind").toString() != QLatin1String("NDI")
            || !out.value("isEnabled").toBool())
            continue;
        out.insert(QStringLiteral("_row"), i);
        if (firstEnabled.isEmpty())
            firstEnabled = out;
        if (out.value("active").toBool())
            return out;
    }
    return firstEnabled;
}

} // namespace

LiveOutputService *LiveOutputService::s_instance = nullptr;

// Listen on ONE NDI output's own mix. GUI thread: this only registers a listener
// with the mixer (AudioMixer::AddMixTap), which renders that output's mix — the
// sum of the audio buses wired to its node — on the audio render thread. The
// listener only copies into a bounded queue; the send happens on the feed thread
// (see ndiWorkerSendAudio), because an SDK send must never run in the callback.
void LiveOutputService::listenOnNdiOutput(const QString &nodeId,
                                          const std::shared_ptr<NdiAudio> &audio)
{
    if (!audio || audio->mixTap != 0 || nodeId.isEmpty())
        return;
    audio->nodeId = nodeId.toStdString();
    // The callback OWNS a reference to the state: a tap can outlive this service
    // (the engine is torn down after the UI), and it must then write into its own
    // queue rather than into freed memory.
    auto state = audio;
    audio->mixTap = bps::audio::AudioEngine::Instance().AddMixTap(
        audio->nodeId,
        [state](const float *samples, uint32_t frames, uint32_t channels,
                uint32_t rateHz, uint64_t startFrame) {
            if (samples == nullptr || frames == 0 || channels == 0 || rateHz == 0)
                return;
            std::lock_guard<std::mutex> lock(state->mutex);
            // Drop the OLDEST blocks once the backlog is a second long: a sender
            // that stalls keeps its latency bounded instead of building a delay
            // the operator can only escape by restarting.
            const uint64_t backlog = static_cast<uint64_t>(rateHz) * kNdiAudioBacklogMs / 1000ull;
            while (!state->queue.empty() && state->queuedFrames + frames > backlog) {
                state->queuedFrames -= state->queue.front().frames;
                state->queue.pop_front();
            }
            NdiAudioBlock block;
            block.samples.assign(samples, samples + static_cast<size_t>(frames) * channels);
            block.frames = frames;
            block.channels = channels;
            block.rateHz = rateHz;
            block.startFrame = startFrame;
            state->queuedFrames += frames;
            state->queue.push_back(std::move(block));
        });
    qInfo("LiveOutputService: NDI output '%s' now carries its own audio mix (tap %llu)",
          audio->nodeId.c_str(), static_cast<unsigned long long>(audio->mixTap));
}

void LiveOutputService::silenceNdiOutput(const std::shared_ptr<NdiAudio> &audio)
{
    if (!audio || audio->mixTap == 0)
        return;
    const std::string nodeId = audio->nodeId;
    // RemoveMixTap takes the lock the render thread holds WHILE it calls a tap, so
    // when this returns no callback for this listener is running — the state can
    // then be emptied and dropped without a race.
    bps::audio::AudioEngine::Instance().RemoveMixTap(audio->mixTap);
    audio->mixTap = 0;
    {
        std::lock_guard<std::mutex> lock(audio->mutex);
        audio->queue.clear();
        audio->queuedFrames = 0;
    }
    qInfo("LiveOutputService: NDI output '%s' no longer carries audio (%llu block(s), %llu frame(s) sent)",
          nodeId.c_str(), static_cast<unsigned long long>(audio->blocksSent.load()),
          static_cast<unsigned long long>(audio->framesSent.load()));
}

// NDI sends run on the dedicated feed thread; the GUI only resolves and queues
// frames. This worker owns the SDK send and its per-output diagnostics.
void LiveOutputService::ndiWorkerSendFrame(void *providerVoid,
                                           std::shared_ptr<const pr::Frame> frame,
                                           QString senderName, float fps)
{
    auto *provider = static_cast<bps::display::NdiDisplayProvider *>(providerVoid);
    auto &svc = LiveOutputService::instance();
    if (!provider || !frame || frame->empty())
        return;

    // Set the identity before first send; NDI fixes the name when it creates
    // the sender. Sender creation and sending stay off the GUI thread.
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

    // Feedback to the GUI side — ATOMICS, not a lock. The pill and the rate
    // watch read these from QML bindings, so a lock here would be a lock the
    // GUI thread could be holding while it does something slow, taken on the
    // FEED thread around every send. The counter is atomic and the two incident
    // flags were only ever this thread's to decide (the GUI only clears them),
    // so neither side waits for the other.
    // Every note below is keyed by THIS sender, never by a flag shared with the
    // other outputs: seven enabled NDI rows are seven senders, and a single
    // latch reported only whichever one happened to send first (the reported
    // "it only ever tells me about New Screen 2"). Each output announces its own
    // first frame, its own failures and its own slow sends.
    const QString senderKey = QString::fromStdString(provider->SenderName());
    if (sent) {
        if (provider->FramesSent() == 1)
            qInfo("LiveOutputService: first frame sent for NDI output '%s'", qUtf8Printable(senderKey));
        if (!svc.ndiFirstFrameNames_.contains(senderKey)) {
            svc.ndiFirstFrameNames_.insert(senderKey);
            qCWarning(lcNdiProgress, "LiveOutputService: NDI frames flowing as '%s' — discovery on a receiving monitor can take 10-30s",
                     qUtf8Printable(senderKey));
        }
        svc.ndiSendFailedNames_.remove(senderKey);
        // Slow-send report per output: one busy sender must not hide (or be
        // hidden by) another's, which is what the shared latch did.
        if (sendMs > qMax(50, fps > 0.0f ? int(1000.0f / fps) : 100)) {
            if (!svc.ndiSlowNames_.contains(senderKey)) {
                svc.ndiSlowNames_.insert(senderKey);
                svc.ndiFastStreaks_.insert(senderKey, 0);
                qCWarning(lcNdiProgress, "LiveOutputService: NDI send is slow (%lld ms, sender '%s') — the feed skips the beats it overruns; check the network and the configured rate",
                         sendMs, qUtf8Printable(senderKey));
            }
        } else if (svc.ndiFastStreaks_.value(senderKey) + 1 >= 30) {
            svc.ndiFastStreaks_.insert(senderKey, 0);
            svc.ndiSlowNames_.remove(senderKey);   // sustained recovery re-arms this sender's note
        } else {
            svc.ndiFastStreaks_.insert(senderKey, svc.ndiFastStreaks_.value(senderKey) + 1);
        }
    } else if (!svc.ndiSendFailedNames_.contains(senderKey)) {
        svc.ndiSendFailedNames_.insert(senderKey);
        qWarning("LiveOutputService: NDI SendFrame FAILED (sender '%s') — check the engine log for the provider's own reason",
                 qUtf8Printable(senderKey));
    }
}

// The audio half of a send: this output's OWN mix, planar float32 as NDI wants
// it. FEED THREAD ONLY.
//
// The tap fills a bounded queue on the audio render thread (it must never block
// there, and the SDK send belongs to the feed thread), and this drains it. Every
// chunk waiting is sent as ONE block — the mix is contiguous in time, so there is
// no reason to pay an SDK call per 10 ms chunk — and a backlog longer than a
// second drops its oldest blocks in the tap itself, so a stalled network costs
// latency instead of memory.
void LiveOutputService::ndiWorkerSendAudio(void *providerVoid,
                                          const std::shared_ptr<NdiAudio> &audio)
{
    auto *provider = static_cast<bps::display::NdiDisplayProvider *>(providerVoid);
    if (!provider || !audio)
        return;
    // A sender that has not opened yet (no frame has gone out) has no id to send
    // to; the queue keeps filling and the first audio block goes with the first
    // picture that does.
    const std::string senderId = provider->SenderId();
    if (senderId.empty())
        return;

    std::vector<NdiAudioBlock> ready;
    {
        std::lock_guard<std::mutex> lock(audio->mutex);
        if (audio->queue.empty())
            return;
        // One rate/channel count is the normal case; a chunk that arrives in a new
        // format starts a new block rather than being stretched into the old one.
        const uint32_t rateHz = audio->queue.front().rateHz;
        const uint32_t channels = audio->queue.front().channels;
        uint32_t frames = 0;
        while (!audio->queue.empty() && audio->queue.front().rateHz == rateHz
               && audio->queue.front().channels == channels) {
            frames += audio->queue.front().frames;
            ready.push_back(std::move(audio->queue.front()));
            audio->queue.pop_front();
        }
        audio->queuedFrames -= frames;
    }
    if (ready.empty())
        return;

    uint32_t frames = 0;
    for (const auto &block : ready)
        frames += block.frames;
    if (frames == 0)
        return;
    const uint32_t channels = ready.front().channels;
    const uint32_t rateHz = ready.front().rateHz;

    // Interleaved (the mixer's layout) → PLANAR (NDI's channel_stride contract).
    std::vector<float> planar(static_cast<size_t>(frames) * channels, 0.0f);
    size_t atFrame = 0;
    for (const auto &block : ready) {
        for (uint32_t f = 0; f < block.frames; ++f) {
            for (uint32_t c = 0; c < channels; ++c) {
                const float v = c < block.channels
                    ? block.samples[static_cast<size_t>(f) * block.channels + c] : 0.0f;
                planar[static_cast<size_t>(c) * frames + atFrame + f] = v;
            }
        }
        atFrame += block.frames;
    }

    bps::broadcast::AudioFrameInfo info;
    info.sampleRate = static_cast<int>(rateHz);
    info.channels = static_cast<int>(channels);
    info.samples = static_cast<int>(frames);
    info.timestampMs = rateHz > 0
        ? static_cast<int64_t>(ready.front().startFrame * 1000ull / rateHz) : 0;
    (void)bps::broadcast::BroadcastEngine::Instance().SendAudioFrame(
        senderId, info, planar.data(), planar.size() * sizeof(float));
    audio->blocksSent.fetch_add(1, std::memory_order_relaxed);
    audio->framesSent.fetch_add(frames, std::memory_order_relaxed);
}

// One beat of the feed — FEED THREAD ONLY (the worker's own timer calls this,
// and everything it touches is either thread-local or an atomic the GUI reads).
//
// This is where the feed's cadence is decided, and the whole point of the split
// above: nothing the GUI thread does can move it. A tick that arrives inside the
// previous interval (a send slower than the period, or the timer coalescing
// after a stall) is skipped rather than fired late-and-again — a two-frame burst
// is exactly what a receiver counts as dropped frames.
void LiveOutputService::ndiWorkerTick()
{
    // The feed thread's OWN plan: handed over by handOverNdiPlan (a queued call
    // installs it on THIS thread), so reading it takes no lock and cannot be
    // made to wait on the GUI thread.
    const auto plans = ndiFeedPlan_;
    // Nothing to feed (stopped, no enabled NDI row, the output gone): sleep the
    // clock rather than tick into an empty plan 30-60x/s, and drop the pacing
    // clock with it — the next feed is a NEW session and must not inherit this
    // one's gap between sends.
    if (plans.empty()) {
        if (ndiSendTimer_ && ndiSendTimer_->isActive())
            ndiSendTimer_->stop();
        ndiSendClock_.invalidate();
        return;
    }
    // AUDIO FIRST, AND ON EVERY TICK. The picture is paced at the output's own
    // rate (the gate below skips the beats it does not need), but the mixer fills
    // a chunk every ~10 ms and nobody waits for a video frame to carry it: an
    // output with an audio wire sends its mix on each beat, including the ticks
    // where no new picture is due.
    for (const auto &plan : plans)
        if (plan.audio)
            ndiWorkerSendAudio(plan.provider.get(), plan.audio);
    for (const auto &plan : plans) {
        if (!plan.provider || !plan.buffer) continue;
        const float fps = plan.fps > 0.0f ? plan.fps : 30.0f;
        const int periodMs = qMax(1, int(std::llround(1000.0 / double(fps))));
        auto &last = ndiSendClocks_[plan.outputName];
        if (last.isValid() && last.elapsed() < qint64(periodMs * 0.9)) continue;
        const auto frame = plan.buffer->LastFrameSnapshot();
        if (!frame || frame->empty()) continue;
        last.restart();
        ndiWorkerSendFrame(plan.provider.get(), frame, plan.senderName, fps);
    }
}

// The five NDI property readers — QML bindings and the 10 Hz poll, all on the
// GUI thread. NOTHING TO WAIT FOR: the four pill fields are this thread's own
// state (see the members), and the frame counter is an atomic the feed thread
// publishes. These are the reads a Settings · Outputs pill animates with, so a
// lock here was a lock in the app's most frequently evaluated bindings.
bool LiveOutputService::ndiSending() const
{
    return ndiSending_;
}
QString LiveOutputService::ndiSendingOutputName() const
{
    return ndiSendingOutput_;
}
qulonglong LiveOutputService::ndiFramesSent() const
{
    return ndiFramesSent_.load(std::memory_order_relaxed);
}
bool LiveOutputService::ndiWarming() const
{
    return ndiWarming_;
}
QString LiveOutputService::ndiWarmingOutputName() const
{
    return ndiWarmingOutput_;
}
void LiveOutputService::setNdiWarming(bool on, const QString &name)
{
    // GUI thread only (see the members): a plain compare-and-assign, notifying
    // only on a real change.
    if (ndiWarming_ == on && (!on || ndiWarmingOutput_ == name))
        return;
    ndiWarming_ = on;
    ndiWarmingOutput_ = on ? name : QString();
    emit ndiChanged();
}

// Publish changed plans by value; only the feed thread accesses its copy.
void LiveOutputService::handOverNdiPlan(const std::vector<NdiSendPlan> &plans)
{
    if (ndiPlanPublished_ && plans == ndiSentPlan_)
        return;
    if (!ndiWorker_)
        return;   // no feed thread: the feed thread's own default already means "nothing to feed"
    ndiSentPlan_ = plans;
    ndiPlanPublished_ = true;
    QMetaObject::invokeMethod(ndiWorker_, [this, plans] { ndiFeedPlan_ = plans; }, Qt::QueuedConnection);
}

LiveOutputService::LiveOutputService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;

    // Keep idle preview frames available for the resource meters.
    previewTimer_ = new QTimer(this);
    previewTimer_->setInterval(1000);
    connect(previewTimer_, &QTimer::timeout, this, []() {
        if (EngineBridge::instance().booted())
            (void)bps::live::LiveOutputController::Instance().StartIdlePreview();
    });
    previewTimer_->start();

    // Poll from boot so monitor tiles also refresh during idle preview.
    ensurePollTimer();

    // Undo optimistic preview state if the engine worker later refuses a tap.
    connect(&EngineBridge::instance(), &EngineBridge::previewRefused,
            this, &LiveOutputService::onPreviewRefused);
}

// Undo this service's optimistic preview state after an asynchronous refusal.
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

    // Trace this expensive QML-facing operation for startup diagnostics.
    VGR_CALL_TRACE("LiveOutputService.goLive");
    // Send the staged preview selection, regardless of the open document.
    if (!stagedSlidesRaw_.isEmpty()) {
        // Staging belongs to the preview; going live does not consume it.
        goLiveWithSlides(stagedName_, stagedSlidesRaw_);
        return;
    }

    // Acknowledge before starting the engine, which can take time to render the
    // first frame. With no staged slide, send the composited preview layers under
    // a blank carrier; the open document is never implicitly taken live.
    EventBus::instance().notify(QStringLiteral("Going live…"),
                                QStringLiteral("info"), QStringLiteral("Go Live"));
    goLiveBlank();
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
    // This live session's NDI sender opens under the selected NDI output's
    // own name, or the first enabled NDI row when another transport is live.
    armNdiSenderForSession();
    // ...and the program goes back on the air: GO LIVE is what re-opens the
    // default endpoint's render stream after a STOP closed it. The output
    // endpoints were never closed, so this only re-arms the program stream.
    setProgramPlayout(true);
    // ANY OTHER CONTENT ENDS A BLACKOUT. The carrier is its own marker (a
    // clearStage slide is the blackout, not content), so a pick that goes on
    // air while blacked out drops what the blackout was hiding — otherwise a
    // later stop() would stage the OLD pick, or endBlackout would put it back
    // over the newer one.
    if (!content.slides.front().clearStage) {
        blackoutActive_ = false;
        blackoutRestoreTitle_.clear();
        blackoutRestoreSlides_.clear();
    }

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
    ensurePollTimer();          // already armed since boot — a no-op here
    startNdiSendClock();        // the NDI feed's own clock, paced by the output's refresh rate
    pollTick();
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
    stageSlidesWithPreview(name, slides, {});
}


// WHICH BLOCK HOLDS THE READING TEXT — the content block, i.e. the text block
// with the most characters. A slide can carry more than one text block (a
// title/reference above the body, a footer below), and this stepper used to
// treat ALL of them as one stream: the TITLE became "line 1" of the window, and
// the recompose then handed the whole window to whichever text block came first
// and blanked every other one — so the first press replaced the title with the
// body's words in the title's type. That is the reported "it also messes with
// the formatting of the text". The reading text is ONE block; every other block
// on the slide is the design and is not ours to touch. Length is the test, not
// position: a title is short and the body is long at every size, and a slide
// with a single text block is unaffected.
static int contentTextBlock(const QVariantMap &slide)
{
    const QVariantList blocks = slide.value(QStringLiteral("blocks")).toList();
    int best = -1;
    int bestLength = 0;
    for (int i = 0; i < blocks.size(); ++i) {
        const QVariantMap block = blocks.at(i).toMap();
        if (block.value(QStringLiteral("kind"), QStringLiteral("text")).toString()
            != QLatin1String("text"))
            continue;
        const int length = block.value(QStringLiteral("text")).toString().size();
        if (length > bestLength) {
            best = i;
            bestLength = length;
        }
    }
    return best;
}

// ONE MEASURED LINE, AS A SPAN OF THE SOURCE TEXT — not as a string.
//
// The span is what keeps the step's arithmetic on the SAME text the design
// draws: the passage is measured here (at the page's own box and type size) and
// the offset a step moves by is a count of these lines, while the renderer
// receives the passage itself — never a list of lines re-joined with hard
// breaks. A re-joined list would insert a break at every measured line, and the
// moment the measure disagreed with the picture by a pixel (a design's auto-fit
// size, a template's padding) each of those lines wrapped a SECOND time: the
// body came out as a narrow column of short lines, taller than the design's slot
// and spilling over its frame — the reported "it works but it messed with the
// template".
struct VisualSpan {
    int start = 0;   // first character of the line
    int end = 0;     // one past its last character (the break is NOT included)
};

// THE FONT A BLOCK IS DRAWN AT — its DECLARED size is not it.
//
// Every surface auto-sizes a text block with the same solve (DesignPreview's
// solveFit, the canvas's recomputeFit): the largest size whose laid-out block
// still fits the box, capped by the declared size unless the mode is `grow`.
// Measuring at the declared size therefore broke lines EARLIER than the picture
// does — a 30px-authored paragraph that the solver draws at 17px fits many more
// words across — so the step landed between two PAINTED lines and read as "not
// yet scrolling well" however carefully the text was cut. These are the
// solver's own numbers, in stage units, so a measured line is a painted line.
static double metaLineFactor(const QVariantMap &meta)
{
    bool ok = false;
    const double n = meta.value(QStringLiteral("lineHeight")).toDouble(&ok);
    return (ok && n > 0.0) ? n : 1.0;   // MetadataFields::naturalLineHeight
}

static double metaLineGap(const QVariantMap &meta)
{
    bool ok = false;
    const double n = meta.value(QStringLiteral("lineSpacing")).toDouble(&ok);
    return ok ? n : 0.0;                // MetadataFields::naturalLineSpacing
}

static double blockPadding(const QVariantMap &block)
{
    return block.value(QStringLiteral("style")).toMap()
        .value(QStringLiteral("padding")).toDouble();
}

// THE FONT A BLOCK DRAWS WITH, at an explicit pixel size: the same family,
// weight, italic, letter and word spacing the renderer uses, so the line breaks
// computed here are the breaks on screen.
// THE WEIGHT QML DRAWS WITH — DesignPreview's own rule, mirrored exactly:
// `font.weight: meta.bold ? Bold : fontWeight Regular/SemiBold/Bold, ELSE Medium`.
// A block that sets neither (the common case) is drawn at Medium, so measuring it
// at QFont's Normal default is a different face's advances on any family that has
// both — and a different advance is a different wrap, i.e. a table that does not
// describe the drawn column.
static QFont::Weight metaFontWeight(const QVariantMap &meta)
{
    if (meta.value(QStringLiteral("bold")).toBool())
        return QFont::Bold;
    const QString weight = meta.value(QStringLiteral("fontWeight")).toString();
    if (weight == QLatin1String("Regular"))
        return QFont::Normal;
    if (weight == QLatin1String("SemiBold"))
        return QFont::DemiBold;
    if (weight == QLatin1String("Bold"))
        return QFont::Bold;
    return QFont::Medium;
}

static QFont blockFontAt(const QVariantMap &block, double pixelSize)
{
    const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
    QFont font;
    // THE FACE THAT IS ACTUALLY DRAWN, not the name the design asked for: QML
    // draws the reading block with FontService.resolve (a missing family falls
    // back to the content face), so measuring with the raw name would wrap a
    // block in a font nobody paints whenever the design's font is not installed.
    const QString family = meta.value(QStringLiteral("fontFamilyResolved")).toBool()
        ? meta.value(QStringLiteral("resolvedFontFamily")).toString()
        : FontService::instance().resolve(meta.value(QStringLiteral("fontFamily")).toString());
    if (!family.isEmpty())
        font.setFamily(family);
    // TRUNCATED, not rounded: QML's own `font.pixelSize` setter stores the integer
    // part of the number it is handed (measured: a fit answer of 44.48 is drawn at
    // 44 px while a rounded twin measures it at 45 — a whole pixel of difference in
    // the size every advance is computed from).
    font.setPixelSize(qMax(1, static_cast<int>(pixelSize)));
    font.setWeight(metaFontWeight(meta));
    font.setItalic(meta.value(QStringLiteral("italic")).toBool());
    const double spacing = meta.value(QStringLiteral("letterSpacing")).toDouble();
    if (!qFuzzyIsNull(spacing))
        font.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    const double wordSpacing = meta.value(QStringLiteral("wordSpacing")).toDouble();
    if (!qFuzzyIsNull(wordSpacing))
        font.setWordSpacing(wordSpacing);
    return font;
}

// QT'S OWN LINE BREAKING, at one pixel size: QTextLayout is what a QML Text
// lays its words out with, so these spans are the lines of the picture — not a
// greedy approximation of them (words measured one advance at a time, which
// disagrees with the renderer over hyphenation, kerning, justification and the
// odd wide glyph).
static QVector<VisualSpan> layoutSpansAt(const QString &text, const QVariantMap &block,
                                        double pixelSize)
{
    const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
    const double width = block.value(QStringLiteral("width"), 160.0).toDouble() - blockPadding(block) * 2.0;
    const bool oneLine = meta.value(QStringLiteral("oneLine")).toBool();
    const QFont font = blockFontAt(block, pixelSize);

    QVector<VisualSpan> spans;
    int paragraphAt = 0;
    const QStringList paragraphs = text.split(QLatin1Char('\n'));
    for (const QString &paragraph : paragraphs) {
        if (paragraph.trimmed().isEmpty()) {
            spans.append({ paragraphAt, paragraphAt });
            paragraphAt += paragraph.size() + 1;
            continue;
        }
        QTextLayout layout(paragraph, font);
        QTextOption option;
        // THE BREAK RULE QML USES. `Text.WordWrap` is Qt's
        // WrapAtWordBoundaryOrAnywhere — a word too long for the line is broken
        // mid-word rather than left to overflow — while plain WordWrap only breaks
        // between words. On a passage with long words the two produce a different
        // line count for the same text and width (measured: 69 lines against the
        // 75 the drawn column had), so the table would sit on different words from
        // the picture.
        option.setWrapMode(oneLine || width <= 0.0 ? QTextOption::NoWrap
                                                   : QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid())
                break;
            line.setLineWidth(width > 0.0 ? width : 1.0e9);
            spans.append({ paragraphAt + line.textStart(),
                           paragraphAt + line.textStart() + line.textLength() });
        }
        layout.endLayout();
        paragraphAt += paragraph.size() + 1;
    }
    return spans;
}

// THE SIZE THE PICTURE WILL DRAW THAT TEXT AT — DesignPreview::solveFit, in
// stage units (see the note above blockFontAt).
static double fittedPixelSize(const QString &text, const QVariantMap &block)
{
    const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
    bool ok = false;
    const double declared = meta.value(QStringLiteral("fontSize")).toDouble(&ok);
    const double base = (ok && declared > 0.0) ? declared : 16.0;
    const double pad = blockPadding(block);
    const double width = block.value(QStringLiteral("width"), 160.0).toDouble() - pad * 2.0;
    const double height = block.value(QStringLiteral("height"), 40.0).toDouble() - pad * 2.0;
    const QString autoSize = meta.value(QStringLiteral("autoSize")).toString();
    const bool grow = autoSize.compare(QLatin1String("grow"), Qt::CaseInsensitive) == 0
                      || autoSize.compare(QLatin1String("growToFit"), Qt::CaseInsensitive) == 0;
    if (width <= 0.0 || height <= 0.0 || text.trimmed().isEmpty())
        return base;

    const double factor = metaLineFactor(meta);
    const double gap = metaLineGap(meta);
    double lo = 3.0, hi = 400.0;
    for (int i = 0; i < 11; ++i) {          // the preview's own eleven steps
        const double mid = (lo + hi) / 2.0;
        const QFont font = blockFontAt(block, mid);
        const QFontMetricsF metrics(font);
        const double natural = metrics.height();
        const double step = (qFuzzyIsNull(gap) || natural <= 0.0) ? factor : factor + gap / natural;
        const QVector<VisualSpan> spans = layoutSpansAt(text, block, mid);
        double widest = 0.0;
        for (const VisualSpan &span : spans)
            widest = qMax(widest, metrics.horizontalAdvance(
                                       text.mid(span.start, span.end - span.start)));
        if (widest <= width + 0.01 && spans.size() * natural * step <= height + 0.01)
            lo = mid;
        else
            hi = mid;
    }
    return grow ? lo : qMin(base, lo);
}

// THE LINES THE READER ACTUALLY SEES.
//
// This is the unit a line step moves by, and getting it wrong is what made the
// stepper "move by slides": a song imported from FreeShow — and every Bible
// verse — carries a whole verse as ONE authored line (`meta.group` verse text
// has no newlines at all; a lyric slide is often one paragraph), which the
// renderer then WRAPS into four to six lines inside its box. Counting newlines
// therefore answered "1 line" for a slide showing six, so there was never
// anywhere to step to and every press rolled the whole slide.
//
// So: lay the block's text out at the block's OWN box and at the size the
// picture draws it at (auto-size included — see fittedPixelSize), and step by
// the resulting VISUAL lines. A hard newline stays a break; a paragraph is
// broken by Qt's own layout engine, the one the preview and the label paint
// with. Measured in STAGE units, which is what a block's x/y/width and its font
// size are in, so no output-size guess enters the arithmetic.
// ...at a size THAT IS ALREADY CHOSEN. A passage is laid out at its page's fitted
// size (see styleRunLinesForText), not at a size fitted for the passage itself.
static QVector<VisualSpan> visualLinesOfTextAtSize(const QString &text, const QVariantMap &block,
                                                  double pixelSize)
{
    QVector<VisualSpan> lines = layoutSpansAt(text, block, pixelSize);

    // LEADING/TRAILING BLANK LINES ARE WHITESPACE, NOT READING: a FreeShow
    // lyric slide often begins with a line of spaces (it is the gap above the
    // title). Stepping into it would move the picture by a blank line — the
    // press would look like it did nothing. Interior blanks are kept: those are
    // stanza breaks the reader is meant to see.
    while (!lines.isEmpty() && text.mid(lines.first().start,
                                       lines.first().end - lines.first().start)
                                        .trimmed().isEmpty())
        lines.removeFirst();
    while (!lines.isEmpty() && text.mid(lines.last().start,
                                       lines.last().end - lines.last().start)
                                        .trimmed().isEmpty())
        lines.removeLast();
    return lines;
}

static QVector<VisualSpan> visualLinesOfText(const QString &text, const QVariantMap &block)
{
    // THE SIZE THE PICTURE USES, then Qt's own line breaking at that size: a
    // step lands on a line that is really painted, not on one this file guessed
    // at from the declared size.
    return visualLinesOfTextAtSize(text, block, fittedPixelSize(text, block));
}

// THE BOX THE READING TEXT IS DRAWN IN, for wrapping a slide whose text is
// carried by `body` rather than by a block (see readingLines): the LARGEST
// text block by area, in stage units — on a filled design that is the content
// slot, and a header band that merely spans the slide is wide but short.
// A slide with nothing measurable comes back empty, and its text stays a plain
// newline split.
static QVariantMap readingBoxForWrapping(const QVariantMap &slide)
{
    const QVariantList blocks = slide.value(QStringLiteral("blocks")).toList();
    QVariantMap best;
    double bestArea = 0.0;
    for (const QVariant &entry : blocks) {
        const QVariantMap block = entry.toMap();
        if (block.value(QStringLiteral("kind"), QStringLiteral("text")).toString()
            != QLatin1String("text"))
            continue;
        const double area = block.value(QStringLiteral("width"), 0.0).toDouble()
                            * block.value(QStringLiteral("height"), 0.0).toDouble();
        if (area > bestArea) {
            bestArea = area;
            best = block;
        }
    }
    return best;
}

// THE BLOCK THAT DRAWS THE PASSAGE — DesignPreview's own `readingBlock` rule:
// the longest non-empty `text` among the slide's TEXT blocks. This is the block
// the flow column is laid out in, so it owns BOTH the box the passage wraps at
// and the text whose fitted size the column is drawn at.
//
// WHY IT IS NOT readingBoxForWrapping: that one answers "the largest text block
// by AREA", which is a different block on a design whose header band spans the
// slide. The two rules disagreed on a real Table walk, and the consequence was
// exactly the reported cross-output bug: the passage's size was solved by
// fitting the whole page BODY into the area box (27.42 px), while the surface
// fitted the reading block's own text into its own box (45.84 px) — so the
// service's table held 45 lines where the drawn column had 75 (ReadingDrawn).
// The step converts a character into THAT table's line, so the output's words
// moved 45/75 of a line per press: "the first output is slower than the second"
// and "it stops scrolling as if finished but some lines are not shown".
static QVariantMap readingBlockOf(const QVariantMap &slide)
{
    const QVariantList blocks = slide.value(QStringLiteral("blocks")).toList();
    QVariantMap best;
    int bestLength = 0;
    for (const QVariant &entry : blocks) {
        const QVariantMap block = entry.toMap();
        if (block.value(QStringLiteral("kind"), QStringLiteral("text")).toString()
            != QLatin1String("text"))
            continue;
        const int length = block.value(QStringLiteral("text")).toString().size();
        if (length == 0 || length <= bestLength)
            continue;      // DesignPreview: only a NON-EMPTY text is a reading block
        bestLength = length;
        best = block;
    }
    return best;
}

// THE TEXT A SLIDE READS (readingLines' point 1, in one place): `body` first —
// that is where a fill-built slide (The Table, the Bible) keeps its reading
// text — else the content block of a show's own slide, else its second line.
static QString readingTextOf(const QVariantMap &slide)
{
    const QString body = slide.value(QStringLiteral("body")).toString();
    if (!body.trimmed().isEmpty())
        return body;
    const int content = contentTextBlock(slide);
    if (content >= 0)
        return slide.value(QStringLiteral("blocks")).toList().at(content).toMap()
            .value(QStringLiteral("text")).toString();
    return slide.value(QStringLiteral("line2")).toString();
}

// The author's OWN breaks as spans — what readingLines falls back to when the
// slide has no measurable box to wrap at.
static QVector<VisualSpan> newlineSpansOf(const QString &text)
{
    QVector<VisualSpan> spans;
    int start = 0;
    while (true) {
        const int br = text.indexOf(QLatin1Char('\n'), start);
        const int end = br < 0 ? static_cast<int>(text.size()) : br;
        spans.append({ start, end });
        if (br < 0)
            break;
        start = br + 1;
    }
    return spans;
}

// A SLIDE'S READING LINES — the unit a line step moves by.
//
// THREE THINGS HAVE TO AGREE for a step to read correctly, and each of them was
// wrong at some point, so they are stated here in the order they bit:
//
//  1. WHICH TEXT. `body` first: that is where a FILL-built slide keeps its
//     reading text (The Table and the Bible both arrive this way — BookFill
//     hands the body on precisely because the filled block list no longer says
//     which block is the content; a table design's date code is as
//     text-bearing as its paragraph slot). A show's own slide has no body, so
//     its content block carries the words instead.
//
//  2. THE WIDTH IT WRAPS AT. Measured on the COMPOSED picture
//     (previewWithActiveStyle — the same call the tile's own picture comes
//     from), because an output style's template owns the slot's geometry: the
//     raw slide's box is the DESIGN's, and a design box and a template slot
//     wrap the same paragraph at different widths.
//
//  3. THE BREAKS THE READER SEES. Wrapped, not counted by newline: a verse or a
//     sermon paragraph is ONE authored line that draws as three to six, so
//     counting newlines answered "1 line" and every press rolled a whole slide
//     (the reported "it not move by lines, it still moving by slides").
QStringList LiveOutputService::readingLines(const QVariantMap &slide) const
{
    const QString text = readingTextOf(slide);
    if (text.trimmed().isEmpty())
        return {};

    // WHICH PICTURE TO MEASURE THE BOX ON. A body-carried slide (The Table, the
    // Bible) is drawn through the output style's template, so its box is the
    // TEMPLATE's — the composed picture is the only place that box exists. A
    // show's own slide is drawn from its own blocks, so it is measured on those,
    // unchanged: that path is confirmed working and must not drift.
    const bool bodyCarried = !slide.value(QStringLiteral("body")).toString().trimmed().isEmpty();
    const QVariantMap measured = bodyCarried ? previewWithActiveStyle(slide) : slide;
    // No measurable box (a slide with no text block of its own): the honest
    // answer is the author's own breaks rather than a guess at a width.
    const QVariantMap box = readingBoxForWrapping(measured);
    const QVector<VisualSpan> spans =
        box.isEmpty() ? newlineSpansOf(text) : visualLinesOfText(text, box);
    QStringList lines;
    lines.reserve(spans.size());
    for (const VisualSpan &span : spans)
        lines.append(text.mid(span.start, span.end - span.start));
    // ONE LINE PER DISTINCT BOX, not one per measurement. This is the line that
    // says whether the two sides wrapped against the same width (compare it with
    // ReadingFlowRender's el.width/flowText.width in DesignPreview.qml) — a real
    // width mismatch between the two sides is exactly what would turn NoWrap's
    // exact-line-sharing into clipped, seemingly-frozen text. Written UNGATED it
    // was one line per page MEASURED: 665 lines in a single 90-second reading
    // walk (measured 2026-10-27), in the log the operator is asked to read, and
    // on the thread that draws. One line per box answers the question it exists
    // for; the per-page counts are already in Sampler/LineStep's own lines.
    static std::mutex boxLogMutex;
    static std::set<QString> boxLogged;
    const QString boxKey = QStringLiteral("bodyCarried=%1 boxW=%2 boxH=%3")
                               .arg(bodyCarried ? 1 : 0)
                               .arg(box.value(QStringLiteral("width")).toDouble())
                               .arg(box.value(QStringLiteral("height")).toDouble());
    {
        std::lock_guard<std::mutex> lock(boxLogMutex);
        if (boxLogged.insert(boxKey).second)
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadingFlowBox"),
                                QStringLiteral("%1 lines=%2 (first page measured on this box)")
                                    .arg(boxKey)
                                    .arg(lines.size()));
    }
    return lines;
}

QStringList LiveOutputService::readingLinesAt(int index) const
{
    if (index < 0 || index >= lineSetRaw_.size())
        return {};
    const auto it = readingLinesCache_.constFind(index);
    if (it != readingLinesCache_.constEnd())
        return it.value();
    const QStringList lines = readingLines(lineSetRaw_.at(index).toMap());
    readingLinesCache_.insert(index, lines);
    return lines;
}

// How many slides on EITHER side of the current one get their reading lines
// composed ahead of time (see the header's own comment). 4 both ways is the
// user's own ask ("build like 4 lists ahead... both for forward and
// backward movement") — plenty for normal reading pace, and still a small,
// bounded amount of work per prewarm pass.
constexpr int kReadingPrewarmRadius = 4;

void LiveOutputService::prewarmReadingLines()
{
    if (lineSetRaw_.isEmpty())
        return;
    const int lo = qMax(0, lineSlideIndex_ - kReadingPrewarmRadius);
    const int hi = qMin(static_cast<int>(lineSetRaw_.size()) - 1, lineSlideIndex_ + kReadingPrewarmRadius);
    for (int i = lo; i <= hi; ++i)
        readingLinesAt(i);   // the cache is the point; nothing else reads the return here
}

void LiveOutputService::stageSlidesWithPreview(const QString &name, const QVariantList &slides,
                                                const QVariantMap &styledPreview)
{
    pendingReadingSteps_ = 0;
    ++readingStageSerial_;
    emit readingStageChanged();
    // THE PICK IS THE READING SET: whatever a pane stages (a passage's verses,
    // a Table section, a show's slides) can then be walked line by line with
    // Alt+Down without that pane having to say anything more than it already
    // does. Stage a SINGLE slide and the stepper simply has no neighbour to
    // read into — lineStep then answers false and the caller does the plain
    // next/previous slide instead.
    lineSetName_ = name;
    lineSetRaw_ = slides;
    lineSourceTextOnly_ = false;
    readingTextSourcePages_.clear();
    readingTextSourcePageEnds_.clear();
    readingTextSourceText_.clear();
    readingTextSourceStartChar_ = -1;
    readingTextSourceInitialPositionPending_ = false;
    readingBudgetFitActive_ = false;
    pendingTextSourceName_.clear();
    pendingTextSourcePages_.clear();
    lineSlideIndex_ = 0;
    readingPickBase_ = 0;      // a new pick: the origin is ITS first page
    lineOffset_ = 0;
    // A NEW PICK: the old one's cached reading-line counts belong to slides
    // that are not this pick's own (same index, different content) — kept,
    // they would answer for the wrong slide the moment this pick's own
    // readingLinesAt(i) is asked.
    readingLinesCache_.clear();
    resetReadingRun();   // its pages are not this pick's pages
    // A NEW PICK STARTS AT ITS OWN TOP. The passage and the offset describe the
    // pick that was being read; carried over, they would step the new pick
    // somewhere down its first page.
    readingRebaseSlide_ = -1;
    readingScroll_ = 0.0;
    if (readingScrollAnim_)
        readingScrollAnim_->stop();
    resetReadingRun();
    rebuildReadingRun(0);
    readingFlowText_ = readingRunText_;
    emit readingScrollChanged();
    // The first page goes out carrying its passage, so a step has somewhere to
    // scroll from the very first press and the two surfaces are drawing the same
    // thing before anything is pressed at all (at offset 0 the passage looks
    // exactly like the page — that is the whole point of the layout being one).
    QVariantList pick = slides;
    if (!pick.isEmpty()) {
        QVariantMap first = pick.first().toMap();
        first[QStringLiteral("flowText")] = readingFlowText_;
        first[QStringLiteral("flowChar")] = 0.0;
        pick[0] = first;
    }
    if (live_) {
        goLiveWithSlides(name, pick);   // commit the pick to the engine now
        stagedName_.clear();
        stagedSlidesRaw_.clear();
        stagedSlide_ = QVariantMap{};     // tiles must read the NEW onAirSlide,
        emit stagedChanged();             // not a stale staged copy over it
        announceReadingPage();           // ...and only now is the page announced
        return;
    }
    stagedName_ = name;
    stagedSlidesRaw_ = pick;
    // Through the SAME active-style template composition onAirSlide/
    // previewWithActiveStyle already apply — the staged tile used to show
    // the slide's own raw, unstyled blocks, then visibly jump to bold/
    // larger template text the instant GO LIVE actually committed it (live
    // reported: "the main output preview got bolder" after pressing GO
    // LIVE). Staging should already show what it will look like once live.
    stagedSlide_ = pick.isEmpty() ? QVariantMap{}
                 : (!styledPreview.isEmpty() ? styledPreview
                                             : previewWithActiveStyle(pick.first().toMap()));
    emit stagedChanged();
    announceReadingPage();
}

// ---- the line stepper (Alt+Down / Alt+Up) --------------------------------
// See the header: a NEXT SLIDE replaces the picture, a LINE STEP moves the
// words — and the words it moves are the PASSAGE the page begins in, slid by an
// offset, not a re-cut window of the page (see lineStep).
// THE OPERATOR'S OWN JUMP (see readingJumpSerial in the header): the surfaces may
// re-solve their column for the page this stages, where the walk's own follow
// re-stage (which also goes through stageSlideSet) must not.
void LiveOutputService::setScrollMode(bool on)
{
    if (on == scrollMode_)
        return;
    scrollMode_ = on;
    if (!on)
        pendingReadingSteps_ = 0;
    emit scrollModeChanged();
}

void LiveOutputService::setReadingWordBudget(int words)
{
    // Clamped to the setting's own range (AppSettings declares 3..1000): a budget of
    // 0 or a negative would have the fit solve against an empty probe and collapse
    // the type, and one beyond the setting's ceiling is not a screenful anyone asked
    // for.
    const int clamped = qBound(3, words, 1000);
    if (clamped == readingWordBudget_)
        return;
    readingWordBudget_ = clamped;
    emit readingWordBudgetChanged();
}

void LiveOutputService::noteOperatorJump()
{
    ++readingJumpSerial_;
    emit readingStageChanged();
}

void LiveOutputService::stageSlideSet(const QString &name, const QVariantList &slides, int index)
{
    VGR_CALL_TRACE("LiveOutputService.stageSlideSet");
    pendingReadingSteps_ = 0;
    if (slides.isEmpty()) {
        lineSetRaw_.clear();
        lineSourceTextOnly_ = false;
        readingTextSourcePages_.clear();
        readingTextSourcePageEnds_.clear();
        readingTextSourceText_.clear();
        readingTextSourceStartChar_ = -1;
        readingTextSourceInitialPositionPending_ = false;
        readingBudgetFitActive_ = false;
        pendingTextSourceName_.clear();
        pendingTextSourcePages_.clear();
        lineSlideIndex_ = 0;
        readingPickBase_ = 0;
        announceReadingPage();
        lineOffset_ = 0;
        readingLinesCache_.clear();
        resetReadingRun();
        stageSlides(name, slides);
        return;
    }
    // A direct page pick starts a new fit/memo generation; line steps do not.
    ++readingStageSerial_;
    emit readingStageChanged();
    const int at = qBound(0, index, static_cast<int>(slides.size()) - 1);
    // THIS IS THE ONE CALL THAT KNOWS WHICH SLIDE OF THE SET IS BEING READ, so it
    // stages it itself rather than seeding the stepper at 0 and putting the index
    // back afterwards (that seed committed slide 0 to the engine and this call
    // then committed the real one — two live replacements for one click).
    lineSetName_ = name;
    lineSetRaw_ = slides;
    lineSourceTextOnly_ = false;
    readingTextSourcePages_.clear();
    readingTextSourcePageEnds_.clear();
    readingTextSourceText_.clear();
    readingTextSourceStartChar_ = -1;
    readingTextSourceInitialPositionPending_ = false;
    readingBudgetFitActive_ = false;
    pendingTextSourceName_.clear();
    pendingTextSourcePages_.clear();
    lineSlideIndex_ = at;
    readingPickBase_ = 0;         // a new pick: the origin is the page it starts on
    lineOffset_ = 0;
    readingLinesCache_.clear();   // a new pick: see stageSlidesWithPreview's own comment
    resetReadingRun();            // ...and its run: those pages are not this pick's
    readingRebaseSlide_ = -1;
    readingScroll_ = 0.0;
    if (readingScrollAnim_)
        readingScrollAnim_->stop();
    resetReadingRun();
    rebuildReadingRun(at);
    readingFlowText_ = readingRunText_;
    // NO ANNOUNCEMENT HERE: readingScroll_ is still 0 at this point (the run
    // has just been rebuilt, the page's base is set by applyReadingRestage
    // below), so emitting now told every drawing surface "the position is 0" —
    // and the restage that follows set the real base (14 on the reported jump)
    // WITHOUT telling them (its own announcement was gated on the FLOW text
    // changing, which a direct jump does not). The tile then drew the passage
    // from its first line while the pane's preview showed the picked paragraph.
    // applyReadingRestage announces the settled position, so that is the one word
    // on this call.
    // The page goes out (or is staged) carrying its passage and its offset — see
    // applyReadingRestage, which also announces the page change once that page is
    // the one in stagedSlide_/onAirSlide_ (the order that matters: a surface which
    // recomposes on the announcement has to find the NEW page already there).
    // A direct page pick resets readingScroll_ before restaging. Force the
    // settled-position notification because the restage can otherwise compare
    // zero with zero and leave surfaces holding the previous page's character.
    applyReadingRestage(true, /*forceRebuild=*/ false, /*forceScrollNotify=*/ true);
}

void LiveOutputService::setReadingTextSource(const QString &name, const QVariantList &pages, int index)
{
    if (pages.isEmpty())
        return;
    pendingTextSourceName_ = name;
    pendingTextSourcePages_ = pages;
    pendingTextSourceIndex_ = qBound(0, index, static_cast<int>(pages.size()) - 1);
    if (scrollMode_)
        activatePendingTextSource();
}

void LiveOutputService::activatePendingTextSource()
{
    if (pendingTextSourcePages_.isEmpty())
        return;
    pendingReadingSteps_ = 0;
    QVariantMap visualPage;
    const QVariantList visualSlides = live_ ? onAirSlidesRaw() : stagedSlidesRaw_;
    if (!visualSlides.isEmpty())
        visualPage = visualSlides.first().toMap();
    QVariantList readingPages;
    readingPages.reserve(pendingTextSourcePages_.size());
    for (const QVariant &value : pendingTextSourcePages_) {
        QVariantMap page = value.toMap();
        if (page.value(QStringLiteral("contentType")).toString().isEmpty())
            page[QStringLiteral("contentType")] = visualPage.value(QStringLiteral("contentType"));
        readingPages.append(page);
    }
    const int selectedPage = qBound(0, pendingTextSourceIndex_,
                                    static_cast<int>(readingPages.size()) - 1);
    int selectedChar = 0;
    QString passage;
    readingTextSourcePageEnds_.clear();
    readingTextSourcePageEnds_.reserve(readingPages.size());
    for (int i = 0; i < readingPages.size(); ++i) {
        if (i == selectedPage)
            selectedChar = passage.size();
        passage += readingTextOf(readingPages.at(i).toMap());
        readingTextSourcePageEnds_.append(passage.size());
    }
    readingTextSourceText_ = passage;
    QVariantMap passagePage = readingPages.first().toMap();
    passagePage[QStringLiteral("body")] = passage;

    lineSetName_ = pendingTextSourceName_;
    // Word-budget slices are UI navigation anchors, not scroll pages. Joining
    // them once here gives the walk one immutable JSON text/line grid; otherwise
    // each slice boundary rewrapped the run and changed the word under the cursor.
    lineSetRaw_ = QVariantList{passagePage};
    readingTextSourcePages_ = readingPages;
    readingTextSourceStartChar_ = selectedChar;
    readingTextSourceInitialPositionPending_ = true;
    lineSourceTextOnly_ = true;
    readingBudgetFitActive_ = true;
    lineSlideIndex_ = 0;
    pendingTextSourceName_.clear();
    pendingTextSourcePages_.clear();
    readingPickBase_ = 0;
    lineOffset_ = 0;
    readingLinesCache_.clear();
    readingRunLinesCache_.clear();
    readingRunLinesLru_.clear();
    resetReadingRun();
    readingRebaseSlide_ = -1;
    if (readingScrollAnim_)
        readingScrollAnim_->stop();
    readingScroll_ = 0.0;
    readingTextWindowCharStart_ = 0;
    readingFlowText_.clear();
    refreshReadingTextWindow(selectedChar);
    rebuildReadingRun(lineSlideIndex_, true);
    const auto *model = OutputListModel::instance();
    const QString referenceStyle = model ? model->activeStyleId() : QString();
    const QVector<int> starts = styleRunLinesForText(referenceStyle, readingRunText_).charStarts;
    const int localChar = qMax(0, selectedChar - readingTextWindowCharStart_);
    if (!starts.isEmpty()) {
        const auto at = std::upper_bound(starts.constBegin(), starts.constEnd(), localChar);
        lineOffset_ = qMax(0, static_cast<int>(at - starts.constBegin()) - 1);
        readingLayoutTargetChar_ = -1;
    } else {
        readingLayoutTargetChar_ = selectedChar;
    }
    readingScroll_ = lineOffset_;
    emit readingStageChanged();
    applyReadingRestage(false, false, true);
}

int LiveOutputService::readingPage() const
{
    return lineSourceTextOnly_ && !readingTextSourcePages_.isEmpty()
        ? readingTextSourcePageForChar(readingCharOffset()) : lineSlideIndex_;
}

int LiveOutputService::readingTextSourcePageForChar(int charOffset) const
{
    if (readingTextSourcePages_.isEmpty())
        return lineSlideIndex_;
    const auto it = std::upper_bound(readingTextSourcePageEnds_.cbegin(),
                                     readingTextSourcePageEnds_.cend(), charOffset);
    return it == readingTextSourcePageEnds_.cend()
        ? readingTextSourcePages_.size() - 1
        : static_cast<int>(it - readingTextSourcePageEnds_.cbegin());
}

// ONE PAGE'S PASSAGE: that page followed by the next one, joined by the page
// break. Two pages and no more — a step only ever has to show the lines directly
// below the box, and the next page's first line is the last thing it needs.
//
// REVERTED to the RAW authored text (was briefly built from readingLinesAt()'s
// own wrapped lines, to share Main's exact wrap with Lower Third — see the
// ReadingFlowBox/ReadingFlowRender diagnostics in readingLines() and
// DesignPreview.qml, added to chase this). That broke something more basic
// than the wrap-matching it was meant to fix: DesignPreview's el.drawsFlow
// gate checks readingFlowText.indexOf(el.modelData.text) === 0 — a literal
// PREFIX match against the block's raw, unwrapped text — and a flow text with
// a \n inserted at every WRAPPED visual line (often within the first few
// words) broke that match everywhere, permanently, so the flow never drew at
// all (reported: "alt+up or down is not moving"; confirmed via
// ReadingFlowRender never once appearing in engine.log while ReadingFlowBox
// and LineStepShortcut both fired normally — the step itself was always
// working, only its own drawing gate was not). The join is a newline, and the
// engine's own layout is what turns the rest into lines: the passage is
// measured (and drawn) at the page's fitted size, so the page inside it
// breaks exactly where the page alone broke — which is also why the prefix
// match holds again. Matching Lower Third's wrap to Main's needs a different
// approach (e.g. fixing el.drawsFlow's own match to compare against wrapped
// text consistently, verified live) rather than another patch on this.
void LiveOutputService::resetReadingRun()
{
    readingRunFirst_ = -1;
    readingRunLast_ = -1;
    readingRunText_.clear();
    readingRunBaseLine_ = 0;
    readingRunCharBase_ = 0;
    // ...and the walk's held type size goes with them: a new pick (or a style change,
    // which reseats through here) is a new column and solves its own size. See
    // styleRunLinesForText for why the size is held at all.
    readingRunSizeForStyle_.clear();
    readingRunPageStart_.clear();
    readingRunPageLines_.clear();
    // NOTHING TO DO ABOUT readingFitAcceptedJump_ HERE: this runs inside
    // rebuildReadingRun for every page re-cut, and the marker is keyed to the
    // OPERATOR-JUMP serial (see the header) precisely so an internal re-cut cannot
    // unsettle a column the walk is holding.
}

// HOW MANY CHARACTERS the pages in [from, to) contribute to a run's text: the
// page plus the break the join puts before it, exactly as rebuildReadingRun
// builds it (empty pages contribute nothing). This is what moves
// readingRunCharBase_ when the window slides — and what makes the run-relative
// offset comparable with itself across that move.
int LiveOutputService::readingCharSpan(int from, int to) const
{
    int total = 0;
    const int count = static_cast<int>(lineSetRaw_.size());
    for (int i = qMax(0, from); i < qMin(to, count); ++i) {
        const QString page = readingTextOf(lineSetRaw_.at(i).toMap());
        if (page.trimmed().isEmpty())
            continue;
        // The page, plus the break before the next one — but a TEXT-ONLY run (the
        // scroll's JSON passage) joins its pages with NOTHING (rebuildReadingRun
        // appends them at their exact boundary), so counting a break there moved the
        // passage origin one character per dropped page: ReadContinuity delta=1 at
        // nearly every page commit, and a reading position that drifted ahead of the
        // text the further the walk went.
        total += page.size() + (lineSourceTextOnly_ ? 0 : 1);
    }
    return total;
}

// The passage's visual lines as ONE STYLE lays them out — the tile/output side of
// `readingLines` (which always measures the ACTIVE style). Same two rules it
// follows: the box comes from the COMPOSED picture at that style (a template owns
// the slot's geometry), and the text is the RUN the step is drawing, so the index
// a caller passes in is the very number `readingScroll` carries.
// CACHED BY THE WHOLE ANSWER: the style, the page whose box is used and the run's
// own text. Composing the page at that style is the cost (a full styled picture,
// exactly what previewWithStyle is), so a per-press caller — the walk's own
// diagnostic, a tile asking for its own grid — asks once per (style, page, run)
// instead of once per press. Content in the key means nothing has to invalidate
// it; a bounded size keeps it from growing across a long session.
const LiveOutputService::StyleRunLines &LiveOutputService::styleRunLines(const QString &styleId) const
{
    return styleRunLinesForText(styleId, readingFlowText_);
}

bool LiveOutputService::refreshReadingTextWindow(int passageChar)
{
    if (!lineSourceTextOnly_ || readingTextSourceText_.isEmpty())
        return false;
    constexpr int kWindowChars = 6000;
    constexpr int kWindowMargin = 1200;
    const int size = readingTextSourceText_.size();
    const int currentStart = readingTextWindowCharStart_;
    const int currentEnd = currentStart + readingFlowText_.size();
    if (!readingFlowText_.isEmpty() && passageChar >= currentStart + kWindowMargin
            && passageChar <= currentEnd - kWindowMargin)
        return false;

    const int desiredStart = qBound(0, passageChar, qMax(0, size - kWindowChars));
    int start = desiredStart;
    int end = qMin(size, desiredStart + kWindowChars);
    while (end < size && end > start && !readingTextSourceText_.at(end - 1).isSpace())
        --end;
    if (end <= start)
        end = qMin(size, start + kWindowChars);
    const QString windowText = readingTextSourceText_.mid(start, end - start);
    if (start == currentStart && windowText == readingFlowText_)
        return false;
    readingTextWindowCharStart_ = start;
    readingFlowText_ = windowText;
    readingRunText_ = readingFlowText_;
    return true;
}

qreal LiveOutputService::readingFlowCharForScroll(qreal lines) const
{
    return qBound(0.0, readingCharForScroll(lines), static_cast<qreal>(readingFlowText_.size()));
}

// ...the same table for a text that is not installed yet. rebuildReadingRun needs
// this: it has just built the run's NEW text and has to ask what line the reader's
// character sits on in IT — readingFlowText_ is still the previous run at that
// point, so asking styleRunLines there would map the position into the old column.
// (That is exactly the mistake the first attempt at a character-preserving rebase
// made; it measured as an oscillation of ~200 characters per press and was
// reverted. See rebuildReadingRun's own note.)
const LiveOutputService::StyleRunLines &LiveOutputService::styleRunLinesForText(const QString &styleId,
                                                                              const QString &text) const
{
    static const StyleRunLines none;
    if (text.trimmed().isEmpty() || lineSetRaw_.isEmpty()
            || lineSlideIndex_ < 0 || lineSlideIndex_ >= lineSetRaw_.size())
        return none;
    const QString key = styleId + QLatin1Char('#') + QString::number(lineSlideIndex_)
                        + QLatin1Char('#') + QString::number(qHash(text), 16);
    if (lineSourceTextOnly_ && readingBudgetFitActive_) {
        const auto *scrollModel = OutputListModel::instance();
        const QString columnKey = styleId.isEmpty()
            ? (scrollModel ? scrollModel->activeStyleId() : QString()) : styleId;
        const auto column = scrollColumns_.constFind(columnKey);
        if (column == scrollColumns_.constEnd() || column->size <= 0.0)
            return none;
        const QString columnCacheKey = QStringLiteral("c#") + columnKey + QLatin1Char('#')
                                       + QString::number(qHash(text), 16);
        const auto hit = readingRunLinesCache_.constFind(columnCacheKey);
        if (hit != readingRunLinesCache_.constEnd())
            return hit.value();
        if (!readingRunLinesPending_.contains(columnCacheKey)) {
            readingRunLinesPending_.insert(columnCacheKey);
            const QString columnSig = column->sig;
            QVariantMap workerBlock = column->block;
            QVariantMap meta = workerBlock.value(QStringLiteral("meta")).toMap();
            meta.insert(QStringLiteral("resolvedFontFamily"),
                        FontService::instance().resolve(meta.value(QStringLiteral("fontFamily")).toString()));
            meta.insert(QStringLiteral("fontFamilyResolved"), true);
            workerBlock.insert(QStringLiteral("meta"), meta);
            const double pixelSize = column->size;
            const QString workerText = text;
            QPointer<LiveOutputService> guard(const_cast<LiveOutputService *>(this));
            QThreadPool::globalInstance()->start(
                [guard, columnCacheKey, columnKey, columnSig, workerText, workerBlock, pixelSize]() {
                    const QVector<VisualSpan> spans = visualLinesOfTextAtSize(
                        workerText, workerBlock, pixelSize);
                    if (!guard)
                        return;
                    QMetaObject::invokeMethod(guard.data(),
                        [guard, columnCacheKey, columnKey, columnSig, workerText, spans]() {
                            auto *self = guard.data();
                            if (!self)
                                return;
                            self->readingRunLinesPending_.remove(columnCacheKey);
                            const auto currentColumn = self->scrollColumns_.constFind(columnKey);
                            if (currentColumn == self->scrollColumns_.constEnd()
                                    || currentColumn->sig != columnSig)
                                return;
                            StyleRunLines entry;
                            entry.lines.reserve(spans.size());
                            entry.charStarts.reserve(spans.size());
                            for (const VisualSpan &span : spans) {
                                entry.lines.append(workerText.mid(span.start, span.end - span.start));
                                entry.charStarts.append(span.start);
                            }
                            while (self->readingRunLinesCache_.size() >= 16
                                   && !self->readingRunLinesLru_.isEmpty())
                                self->readingRunLinesCache_.remove(
                                    self->readingRunLinesLru_.takeFirst());
                            self->readingRunLinesLru_.append(columnCacheKey);
                            self->readingRunLinesCache_.insert(columnCacheKey, entry);
                            if (self->lineSourceTextOnly_ && self->readingFlowText_ == workerText) {
                                const auto *model = OutputListModel::instance();
                                if (model && model->activeStyleId() == columnKey) {
                                    self->readingRunPageLines_.insert(0, spans.size());
                                    if (self->readingLayoutTargetChar_ >= 0 && !entry.charStarts.isEmpty()) {
                                        const int localChar = qBound(0,
                                            self->readingLayoutTargetChar_ - self->readingTextWindowCharStart_,
                                            workerText.size());
                                        const auto at = std::upper_bound(entry.charStarts.constBegin(),
                                            entry.charStarts.constEnd(), localChar);
                                        const int line = qMax(0, static_cast<int>(
                                            at - entry.charStarts.constBegin()) - 1);
                                        self->readingScroll_ = line;
                                        self->lineOffset_ = line;
                                        self->readingLayoutTargetChar_ = -1;
                                    }
                                }
                                emit self->readingScrollChanged();
                                QTimer::singleShot(0, self, &LiveOutputService::drainPendingReadingSteps);
                            }
                        }, Qt::QueuedConnection);
                });
        }
        return none;
    }
    // SCROLL MEASURES ITS OWN COLUMN and nothing else (see scrollColumnSize): the
    // block the surface draws the passage in, at the one size the budget solved. No
    // slide is composed, no report is consulted, no solver runs here.
    if (readingBudgetFitActive_) {
        const auto *scrollModel = OutputListModel::instance();
        const QString columnKey = styleId.isEmpty()
            ? (scrollModel ? scrollModel->activeStyleId() : QString()) : styleId;
        const auto column = scrollColumns_.constFind(columnKey);
        if (column != scrollColumns_.constEnd() && column->size > 0.0) {
            const QString columnCacheKey = QStringLiteral("c#") + columnKey + QLatin1Char('#')
                                           + QString::number(qHash(text), 16);
            const auto hit = readingRunLinesCache_.constFind(columnCacheKey);
            if (hit != readingRunLinesCache_.constEnd()) {
                readingRunLinesLru_.removeOne(columnCacheKey);
                readingRunLinesLru_.append(columnCacheKey);
                return hit.value();
            }
            const QVector<VisualSpan> spans = visualLinesOfTextAtSize(text, column->block, column->size);
            StyleRunLines entry;
            entry.lines.reserve(spans.size());
            entry.charStarts.reserve(spans.size());
            for (const VisualSpan &span : spans) {
                entry.lines.append(text.mid(span.start, span.end - span.start));
                entry.charStarts.append(span.start);
            }
            constexpr int kScrollLinesCap = 16;
            while (readingRunLinesCache_.size() >= kScrollLinesCap && !readingRunLinesLru_.isEmpty())
                readingRunLinesCache_.remove(readingRunLinesLru_.takeFirst());
            readingRunLinesLru_.append(columnCacheKey);
            return *readingRunLinesCache_.insert(columnCacheKey, entry);
        }
    }
    const auto cached = readingRunLinesCache_.constFind(key);
    if (cached != readingRunLinesCache_.constEnd()) {
        // Touch: move to most-recently-used so eviction below takes the
        // table that has actually gone cold, not just whichever happened
        // to be inserted first.
        readingRunLinesLru_.removeOne(key);
        readingRunLinesLru_.append(key);
        return cached.value();
    }
    const QVariantMap page = lineSetRaw_.at(lineSlideIndex_).toMap();
    // WHICH SLIDE'S COMPOSITION THE BOX IS MEASURED ON: THE ONE THE SURFACES DRAW.
    //
    // A surface composes the STAGED slide — DesignPreview's host passes
    // previewWithStyle(stagedSlideRaw, style), and the output window does the same —
    // while this table used to compose the pick's RAW page. For the two styles on the
    // reported book those two compositions put the passage in DIFFERENT blocks:
    // measured, one tile drew its column in a 754x56 band while this table was laid
    // out in a 754x372 content area, and 150x42 against 754x124 for the other output.
    // The wrap width IS the box, so the table described a column nobody draws (64
    // lines against 66, 271 against 39, on every press). The staged slide is what is
    // on screen; a pick with nothing staged falls back to the raw page.
    const QVariantMap staged = stagedSlideRaw();
    const QVariantMap source = !staged.isEmpty() ? staged : page;
    const QVariantMap measured = previewWithStyle(source, styleId);
    // THE BLOCK THE COLUMN IS DRAWN IN, not the largest one by area: the surface
    // lays the passage out in this block's box at THIS block's fitted size, and a
    // table measured anywhere else is a different grid from the words on screen
    // (see readingBlockOf for the 45-vs-75-line measurement that proved it).
    const QVariantMap readingBlock = readingBlockOf(measured);
    QVariantMap box = readingBlock.isEmpty() ? readingBoxForWrapping(measured)
                                             : readingBlock;
    // THE BOX THE SURFACE DRAWS THE COLUMN IN, when it has told us (see
    // noteReadingFit). The two sides can pick different blocks out of the same
    // style — measured 150x41 on a surface against 754x124 here — and the wrap
    // width IS the box, so a table measured in the wrong one counts different
    // lines from the picture (271 against 39 on that same output).
    // THE REFERENCE GRID IS THE ACTIVE STYLE'S GRID, so an empty style id has to
    // look its size up under the ACTIVE style — not under "". Every position in
    // this service is counted in the reference grid, and the surface that draws
    // THAT column reports at `activeStyleId()` (an output tile names its own style;
    // the pane's preview draws the active one). Looking up "" found no report at
    // all, so the table fell back to this file's own solver while the picture was
    // drawn by QML's — two answers for one box. Measured on the reported click
    // (engine.log, 16:23:02): ReadingDrawn "drawn=75 table=65 flowPx=45.00
    // svcSize=39.83 box=754x372 sat=45.84" — a five-per-cent-smaller type wrapped
    // into six fewer lines, so the step moved through a grid nobody drew and the
    // column overflowed its box (reported: "the text is overflowing or trying to
    // scroll").
    const auto *styleModel = OutputListModel::instance();
    const QString reportKey = styleId.isEmpty()
        ? (styleModel ? styleModel->activeStyleId() : QString())
        : styleId;
    auto reported = readingFitReported_.constFind(reportKey);
    // ...and the pane's own preview, which draws the active style with no id of its
    // own, reports under "" — a valid answer for the same column, so it is the
    // second choice rather than being ignored.
    if (reported == readingFitReported_.constEnd() && reportKey != styleId)
        reported = readingFitReported_.constFind(styleId);
    if (!box.isEmpty() && reported != readingFitReported_.constEnd()
            && reported->width > 0.0 && reported->height > 0.0) {
        QVariantMap surfaceBox = box;
        surfaceBox.insert(QStringLiteral("width"), reported->width);
        surfaceBox.insert(QStringLiteral("height"), reported->height);
        QVariantMap style = box.value(QStringLiteral("style")).toMap();
        style.insert(QStringLiteral("padding"), reported->pad);
        surfaceBox.insert(QStringLiteral("style"), style);
        box = surfaceBox;
    }
    // AT THE SIZE THE PAGE IS DRAWN AT, not at a size fitted for the PASSAGE.
    //
    // visualLinesOfText sizes a text to fit the box it is given, and a nine-page
    // passage needs a much smaller type than one page does to fit the same height:
    // measured on a real walk, the window wrapped to 17 lines where the same seven
    // pages, each fitted on its own, are 28–36. The page is what the design approved
    // and what every surface draws (DesignPreview holds the page's size across a page
    // commit — see its readingFitSize note), so laying the passage out at the page's
    // own fitted size is what makes this table describe the words that are on screen.
    // With them disagreeing, the step's per-page counts and the table it moves an
    // index into were different grids — the scroll tracked the keys for the first few
    // pages and then drifted, which is the reported "it works, then it stops scrolling
    // correctly".
    // ...AND AT THE WALK'S OWN SIZE, SOLVED ONCE (the same rule the preview holds with
    // its readingFitSize): a passage re-fitted on every page re-flows as pages of
    // different lengths go by — measured 28, 34, 47, 54 and 51 lines for the same
    // nine-page window, i.e. a picture that redraws itself at a new type size every
    // page turn. One walk is one column, so the size the walk's first page solved is
    // held for the rest — per STYLE, because each output draws the passage at its own
    // size and that is exactly what the per-surface conversion exists for.
    // THE SIZE THE SURFACE DRAWS AT WINS (see noteReadingFit): the block is drawn
    // by a QML Text whose solver is the design's own, and a table measured at any
    // other number is a different grid from the picture — the whole defect this
    // area keeps producing. The service's own solver is the fallback for a style no
    // surface has reported (an engine-only read, the very first frame).
    double pageSize = (reported != readingFitReported_.constEnd()) ? reported->size : 0.0;
    if (pageSize <= 0.0)
        pageSize = readingRunSizeForStyle_.value(reportKey, 0.0);
    if (pageSize <= 0.0) {
        // FITTED FROM THE BLOCK'S OWN TEXT, which is what the surface's own fit
        // solver is handed (DesignPreview's fitText) — not the page's whole body.
        // The body of a Table page carries more than the paragraph (BookFill hands
        // it on for that reason), so fitting IT into the same box answers a much
        // smaller size: measured 27.42 px against the surface's 45.84 px, i.e. a
        // third of the drawn column's lines. The passage is drawn at the block's
        // size, so the table that positions it has to be measured at that size.
        const QString fitText = readingBlock.isEmpty() ? readingTextOf(page)
                                                       : readingBlock.value(QStringLiteral("text")).toString();
        // THE DECLARED SIZE, DIRECTLY — not fittedPixelSize's bisection (the
        // engine's twin of the OLD per-page QML solve this function's own
        // comments already say lost to the surface's answer). DesignPreview's
        // solveFit no longer bisects for the reading block either — it now
        // uses el.meta.fontSize outright, because a scrolling column has
        // nothing to fit a height against (see its own note). Measuring this
        // engine-only guess against the OLD algorithm the surface no longer
        // runs guaranteed the two would disagree the moment the block's text
        // didn't happen to fit the box at the declared size: the guess landed
        // here first (stageSlideSet's own initial restage, before any surface
        // has solved anything), the surface's real report corrected it a
        // frame or two later (noteReadingFit's first-solve reset), and the
        // run's base line count — summed from THIS table — shifted under an
        // already-staged, already-visible page: reported as a pane that was
        // "just read" refreshing itself "as if newly selected", present on
        // whichever pick's style hadn't been measured yet this session and
        // absent once it had (readingFitReported_ then answers first, above,
        // and this branch is never reached at all). Reading the declared
        // size here instead makes the guess and the surface's own answer the
        // SAME number by construction, so there is nothing left to correct.
        const QVariantMap fitMeta = box.value(QStringLiteral("meta")).toMap();
        bool fontSizeOk = false;
        const double declaredFontSize = fitMeta.value(QStringLiteral("fontSize")).toDouble(&fontSizeOk);
        pageSize = (fontSizeOk && declaredFontSize > 0.0) ? declaredFontSize : 16.0;
        if (!box.isEmpty())
            readingRunSizeForStyle_.insert(reportKey, pageSize);
        // WHAT THE TABLE'S SIZE WAS SOLVED FROM, once per (style, page, run): the
        // surface's own column reports ITS size and its wrap as ReadingDrawn, and a
        // residual difference between the two lines is a difference in THESE
        // numbers — the box, its padding, the fit text or the face it was fitted in.
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadFit"),
                            QStringLiteral("style=%1 size=%2 box=%3x%4 pad=%5 "
                                           "fitChars=%6 font=%7 (solved from the reading block)")
                                .arg(styleId.isEmpty() ? QStringLiteral("(ref)") : styleId.left(8))
                                .arg(pageSize, 0, 'f', 2)
                                .arg(box.value(QStringLiteral("width")).toDouble())
                                .arg(box.value(QStringLiteral("height")).toDouble())
                                .arg(blockPadding(box))
                                .arg(fitText.size())
                                .arg(blockFontAt(box, pageSize).family()));
    }
    const QVector<VisualSpan> spans = box.isEmpty() ? newlineSpansOf(text)
                                                   : visualLinesOfTextAtSize(text, box, pageSize);
    StyleRunLines entry;
    entry.lines.reserve(spans.size());
    entry.charStarts.reserve(spans.size());
    for (const VisualSpan &span : spans) {
        entry.lines.append(text.mid(span.start, span.end - span.start));
        entry.charStarts.append(span.start);
    }
    // Evict only the single least-recently-used table, not the whole cache:
    // this function is read once per style per animation frame while a page
    // is on screen, so with 2+ outputs/styles live a few page turns fill the
    // table, and a full clear() here used to make the very next per-frame
    // read pay this function's own expensive re-measure synchronously,
    // mid-glide — the reported "scrolls well for the first few pages, then
    // freezes". Keeping the rest of the table warm removes that cliff.
    constexpr int kReadingRunLinesCap = 16;
    while (readingRunLinesCache_.size() >= kReadingRunLinesCap && !readingRunLinesLru_.isEmpty())
        readingRunLinesCache_.remove(readingRunLinesLru_.takeFirst());
    readingRunLinesLru_.append(key);
    return *readingRunLinesCache_.insert(key, entry);
}

QStringList LiveOutputService::readingRunLinesForStyle(const QString &styleId) const
{
    return styleRunLines(styleId).lines;
}

// THE DRAWN COLUMN'S OWN TYPE SIZE, reported by the surface that draws it (see
// DesignPreview's noteReadingFit call). This is the authority the table is measured
// at: the surface's solver is the one the design's page is actually laid out with,
// so [re-]solving it here only ever produced a number the picture does not have
// (measured 44.48 px drawn against the page's own 40 px, and a 69-line table
// against the 75 lines on screen).
//
// A CHANGED NUMBER IS A NEW COLUMN: the cached line table for that style belongs to
// the old one and is dropped, and — when the style is the REFERENCE (active) grid,
// which every base/offset number in this service is counted in — the run is
// re-seated so the walk's arithmetic moves to the new measurement at once.
void LiveOutputService::noteReadingFit(const QString &rawStyleId, qreal size,
                                      qreal boxWidth, qreal boxHeight, qreal pad)
{
    if (size <= 0.0)
        return;
    // SCROLL DOES NOT REPORT. While the passage is the JSON's, the column's size is
    // scrollColumnSize()'s — the max-words budget solved into the text area, once —
    // and the table reads that same answer. A page fit arriving here is a slide's
    // number; letting it in is how the table and the drawn column became two grids.
    if (readingBudgetFitActive_)
        return;
    // A `required property string` bound to a missing/transient model role
    // coerces JS `undefined` into the literal STRING "undefined" before any
    // QML-side `!== undefined` guard ever sees it (OutputMonitorTile's own
    // readingStyleId binding comment already names this exact failure —
    // measured live: style='undefine' in the log). Caught here, the one
    // place every style id this service is handed funnels through, rather
    // than at each binding that can independently fail to catch it.
    const QString styleId = (rawStyleId == QLatin1String("undefined")
                              || rawStyleId == QLatin1String("null"))
        ? QString() : rawStyleId;
    ReadingFit report;
    report.size = size;
    report.width = boxWidth;
    report.height = boxHeight;
    report.pad = pad;
    // WHAT MAKES A NEW COLUMN, and what is just the same one reported again.
    //
    // `qFuzzyCompare` on doubles is effectively exact, so a surface whose solved
    // size lands a hundredth of a pixel away (a font metric rounding, a box that
    // settled by half a pixel during layout) counted as a NEW column: the run was
    // re-measured and the page re-staged, mid-glide. That is the jitter this check
    // is here to absorb — the measured difference has to be one the picture can
    // show. HEIGHT is compared too: it was missing, so a box that got taller at the
    // same width and size (a template slot re-laid out) went unnoticed and its table
    // stayed wrapped for the old height.
    const auto sameColumn = [](const ReadingFit &a, const ReadingFit &b) {
        return qAbs(a.size - b.size) < 0.05
            && qAbs(a.width - b.width) < 0.25
            && qAbs(a.height - b.height) < 0.25
            && qAbs(a.pad - b.pad) < 0.05;
    };
    const auto previous = readingFitReported_.constFind(styleId);
    if (previous != readingFitReported_.constEnd() && sameColumn(*previous, report))
        return;                       // the same column: nothing to re-measure
    // Captured before the insert below, which can rehash the table and
    // invalidate `previous` — used only for the ReadFitNote diagnostic.
    const double previousSize = (previous != readingFitReported_.constEnd()) ? previous->size : 0.0;
    // A PER-PAGE FIT IS NOT A NEW COLUMN.
    //
    // While a walk is live, a surface re-solves the block for the page it is
    // showing, and a fill-built page carries its OWN size — measured on the
    // reported passage: 45.84, 43.90, 42.93 and 40.99 px for four consecutive
    // pages. Every one of those arrived here as a "new column": the tables were
    // purged, the held size dropped and the run re-staged, so the column the reader
    // was watching was re-measured on EVERY press (engine.log, one walk: ReadFitNote
    // 40.99 -> 42.93 -> 45.84 -> 42.93 -> 45.84 -> 40.99 ... with resetsRun=YES each
    // time). The drawn column itself never moved — the surface draws the passage at
    // the size the walk locked when it started — so all of that churn moved nothing
    // but the step's own grid underneath the words. That is the residual jitter.
    //
    // What the walk actually has to tell apart is the BOX from the ANSWER: width,
    // height and padding are the column; the size is what the solver said about the
    // text in front of it. So while a run is live and this report repeats the last
    // box and padding, the page's answer is recorded (the surfaces draw the page
    // block with it) and the WALK keeps the size it started with — nothing is
    // purged, re-measured or re-staged. A jump clears the held size
    // (resetReadingRun does), so the first page after one is accepted as before, and
    // a real column change (a resized box, a template slot re-laid out) changes the
    // box, which is accepted too.
    {
        const bool walkLive = readingRunFirst_ >= 0 && readingRunLast_ >= readingRunFirst_;
        const bool sameBox = previous != readingFitReported_.constEnd()
            && qAbs(previous->width - report.width) < 0.25
            && qAbs(previous->height - report.height) < 0.25
            && qAbs(previous->pad - report.pad) < 0.05;
        // SETTLED FOR THIS JUMP, or not: the OPERATOR-JUMP serial, not the held size
        // and not a run marker. The held size is written when a line table is computed
        // (which can happen after this report arrives); a marker cleared by
        // resetReadingRun is cleared by rebuildReadingRun for every page re-cut; and
        // the plain stage serial moves on the walk's own follow re-stage (measured:
        // stage=10, 11, 12 ... once per press). All three made the branch unreachable
        // in practice (zero `held=YES` while ReadFitNote printed `resetsRun=YES` once
        // per press).
        if (walkLive && sameBox
            && readingFitAcceptedJump_.value(styleId, -1) == readingJumpSerial_) {
            // The box is the walk's; only the page's own answer moved. Recorded, as
            // slide-mode reading always did (scroll never reaches here).
            readingFitReported_.insert(styleId, report);
            const auto *model = OutputListModel::instance();
            const QString reference = model ? model->activeStyleId() : QString();
            EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadFitNote"),
                                QStringLiteral("style='%1' pageFit=%2 columnSize=%3 box=%4x%5 "
                                               "pad=%6 reference='%7' held=YES "
                                               "(the walk keeps its column; page answer recorded)")
                                    .arg(styleId.isEmpty() ? QStringLiteral("(ref)") : styleId.left(8))
                                    .arg(report.size, 0, 'f', 2)
                                    .arg(previousSize, 0, 'f', 2)
                                    .arg(report.width, 0, 'f', 2)
                                    .arg(report.height, 0, 'f', 2)
                                    .arg(report.pad, 0, 'f', 2)
                                    .arg(reference.isEmpty() ? QStringLiteral("(ref)") : reference.left(8)));
            return;
        }
    }
    // THIS REPORT SETTLES THE COLUMN FOR THIS JUMP (see readingFitAcceptedJump_):
    // the first one after an operator stage is the page's own auto-size answer, and
    // that is what the walk is then measured and drawn at.
    readingFitAcceptedJump_.insert(styleId, readingJumpSerial_);
    readingFitReported_.insert(styleId, report);
    readingRunSizeForStyle_.remove(styleId);
    // THE REFERENCE REPORT IS "" BUT ITS TABLES ARE FILED UNDER THE ACTIVE STYLE
    // (see styleRunLinesForText's reportKey), so the purge has to name both — a
    // purge that only matched "#" left every `s1#…` table measured at the old size
    // in the cache, and the forced rebuild below would have re-counted the pages
    // from those stale tables.
    QStringList purgePrefixes{ styleId + QLatin1Char('#') };
    if (styleId.isEmpty()) {
        const auto *model = OutputListModel::instance();
        const QString active = model ? model->activeStyleId() : QString();
        if (!active.isEmpty())
            purgePrefixes.append(active + QLatin1Char('#'));
    }
    for (const QString &prefix : purgePrefixes) {
        readingRunSizeForStyle_.remove(prefix.left(prefix.size() - 1));
        for (auto it = readingRunLinesCache_.begin(); it != readingRunLinesCache_.end(); ) {
            if (it.key().startsWith(prefix))
                it = readingRunLinesCache_.erase(it);
            else
                ++it;
        }
        // Keep the LRU list's keys matching the cache's exactly — a key left
        // behind here would sit in readingRunLinesLru_ pointing at an entry
        // that no longer exists, and get "evicted" for free on the next insert
        // instead of the table that is actually least-recently-used.
        readingRunLinesLru_.removeIf([&prefix](const QString &k) { return k.startsWith(prefix); });
    }
    const auto *model = OutputListModel::instance();
    const QString reference = model ? model->activeStyleId() : QString();
    const bool resets = (styleId == reference);
    // ONE LINE PER CALL THAT ACTUALLY CHANGES SOMETHING (a no-op return above
    // never reaches here) — the earlier calls in this run are silent, so a
    // mid-walk glitch with no other explanation is this function's own
    // resetReadingRun()/applyReadingRestage() cascade, and this is the only
    // way to see it fire without guessing from the side effects.
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadFitNote"),
                        QStringLiteral("style='%1' previousSize=%2 reportedSize=%3 "
                                       "box=%4x%5 pad=%6 reference='%7' resetsRun=%8 "
                                       "run=[%9..%10] jump=%11 acceptedJump=%12")
                            .arg(styleId.isEmpty() ? QStringLiteral("(ref)") : styleId.left(8))
                            .arg(previousSize, 0, 'f', 2)
                            .arg(report.size, 0, 'f', 2)
                            .arg(report.width, 0, 'f', 2)
                            .arg(report.height, 0, 'f', 2)
                            .arg(report.pad, 0, 'f', 2)
                            .arg(reference.isEmpty() ? QStringLiteral("(ref)") : reference.left(8))
                            .arg(resets ? QStringLiteral("YES") : QStringLiteral("no"))
                            .arg(readingRunFirst_)
                            .arg(readingRunLast_)
                            .arg(readingJumpSerial_)
                            .arg(readingFitAcceptedJump_.value(styleId, -1)));
    if (resets) {
        // RE-MEASURE IN PLACE — DO NOT RE-SEAT THE WALK.
        //
        // This used to throw the run away (resetReadingRun), zero the pick's origin
        // (readingPickBase_ = 0) and re-stage from scratch on every report. A
        // measurement does not change the run's TEXT, so all of that rebuilt the
        // same window; the zeroed origin then moved readingAbsoluteLine back to the
        // window's own start, so the reader's position in the pick jumped backwards
        // and the pane's highlight walked back to the passage's first paragraph at
        // each report. That is the "little skip" while scrolling — measured by the
        // per-frame sampler in tools/selftest/run_line_step_test.sh, which reads
        // exactly that number, and it lands mid-glide because a report arrives
        // whenever a surface re-solves its column.
        //
        // The cached line tables for this style were dropped just above, so a
        // FORCED rebuild of the SAME window re-counts the pages and the base in the
        // new grid, while the page, the offset inside it and the pick's origin stay
        // exactly where the reader is.
        applyReadingRestage(true, /*forceRebuild=*/ true);
        return;
    }
    // (CALLED INLINE, same frame — a deferred QTimer::singleShot version of this
    // was tried and reverted: it stopped the binding loop below but left the FIRST
    // frame showing the OLD scroll position for one tick, with the correct one only
    // landing a moment later — a visible pop on EVERY fresh pick. The QML-side loop
    // (OutputMonitorTile's refreshOnAirSlide) is guarded against re-entrant writes
    // instead, which fixes it without delaying the position by a frame.)
}

// SCROLL'S COLUMN — THE ONE PLACE ITS TYPE SIZE IS DECIDED.
//
// Scroll is not a slide. A slide fits ITS page into its box, page by page; a scroll
// takes the passage from the JSON, caps what it measures at the max-words setting
// (default 40), looks at the text area the template gives it, fits that many words
// into it ONCE, and from then on only moves. Nothing here reads a slide, a page fit
// or a surface's report, and nothing re-solves at a page commit or a new paragraph:
// the answer is keyed by the pick, the budget and the block's own geometry/type, so
// it is solved again only when one of those really changes.
//
// The surface draws the column at the size this returns, and the line table
// (styleRunLinesForText) is measured from the very block and size stored here — the
// drawn column and the table are one grid by construction, not by agreement.
qreal LiveOutputService::scrollColumnSize(const QString &rawStyleId, const QVariantMap &block)
{
    if (!readingBudgetFitActive_ || block.isEmpty())
        return 0.0;
    const QString flow = lineSourceTextOnly_ ? readingTextSourceText_ : readingFlowText_;
    if (flow.trimmed().isEmpty())
        return 0.0;
    const int budget = qMax(3, readingWordBudget_);

    // WHAT MAKES IT A DIFFERENT COLUMN: the pick, the budget, the box and the type —
    // never the text on screen or where the reader is in it. (x/y are where the box
    // sits on the stage; they do not change what fits in it.)
    QVariantMap shape;
    shape.insert(QStringLiteral("w"), block.value(QStringLiteral("width")));
    shape.insert(QStringLiteral("h"), block.value(QStringLiteral("height")));
    shape.insert(QStringLiteral("pad"), blockPadding(block));
    shape.insert(QStringLiteral("meta"), block.value(QStringLiteral("meta")));
    const QString sig = lineSetName_ + QLatin1Char('#') + QString::number(budget) + QLatin1Char('#')
        + QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(shape))
                                .toJson(QJsonDocument::Compact));

    qreal size = scrollSizeBySig_.value(sig, 0.0);
    if (size <= 0.0) {
        // THE BUDGET'S WORDS — exactly that many, cycling the passage's own words when
        // it holds fewer yet — each with the break that follows it, so the paragraph
        // breaks take the lines they will take when drawn.
        QStringList words;
        for (int i = 0, n = flow.size(); i < n && words.size() < budget; ) {
            const int start = i;
            while (i < n && !flow.at(i).isSpace())
                ++i;
            while (i < n && flow.at(i).isSpace())
                ++i;
            if (i > start)
                words.append(flow.mid(start, i - start));
        }
        if (words.isEmpty())
            return 0.0;
        QString probe;
        for (int i = 0; i < budget; ++i)
            probe += words.at(i % words.size());
        // A scroll column always takes the solved answer (it fills the box with the
        // budget), so the declared size is no cap: autoSize "grow".
        QVariantMap grown = block;
        QVariantMap meta = grown.value(QStringLiteral("meta")).toMap();
        meta.insert(QStringLiteral("autoSize"), QStringLiteral("grow"));
        grown.insert(QStringLiteral("meta"), meta);
        size = fittedPixelSize(probe.trimmed(), grown);
        if (scrollSizeBySig_.size() >= 64)
            scrollSizeBySig_.clear();
        scrollSizeBySig_.insert(sig, size);
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("ScrollColumn"),
                            QStringLiteral("style=%1 pick='%2' budget=%3 size=%4 box=%5x%6 pad=%7 "
                                           "(solved once: the max-words budget into the text area)")
                                .arg(rawStyleId.isEmpty() ? QStringLiteral("(ref)") : rawStyleId.left(8))
                                .arg(lineSetName_.left(24))
                                .arg(budget)
                                .arg(size, 0, 'f', 2)
                                .arg(block.value(QStringLiteral("width")).toDouble())
                                .arg(block.value(QStringLiteral("height")).toDouble())
                                .arg(blockPadding(block)));
    }

    // THE TABLE'S COLUMN. A surface the style names (an output's tile or window) owns
    // it; the pane's reference preview, which names none, only seats it when no named
    // surface has — so two surfaces of one style in different boxes cannot take turns
    // re-measuring the table. Seating changes nothing on screen: the position is a
    // character, the table only converts it to a line.
    const auto *styleModel = OutputListModel::instance();
    const bool named = !rawStyleId.isEmpty() && rawStyleId != QLatin1String("undefined")
                       && rawStyleId != QLatin1String("null");
    const QString active = styleModel ? styleModel->activeStyleId() : QString();
    const QString key = named ? rawStyleId : active;
    if (key.isEmpty())
        return size;
    const auto seated = scrollColumns_.constFind(key);
    const bool vacant = seated == scrollColumns_.constEnd();
    if ((named || vacant) && (vacant || seated->sig != sig)) {
        const qreal previousChar = readingCharForScroll(readingScroll_);
        const int preserveChar = lineSourceTextOnly_
            ? (readingTextSourceInitialPositionPending_
                   ? readingTextSourceStartChar_
                   : readingTextWindowCharStart_ + static_cast<int>(std::floor(previousChar)))
            : static_cast<int>(std::floor(previousChar));
        ScrollColumn column;
        column.block = block;
        column.size = size;
        column.sig = sig;
        scrollColumns_.insert(key, column);
        const QString prefix = QStringLiteral("c#") + key + QLatin1Char('#');
        for (auto it = readingRunLinesCache_.begin(); it != readingRunLinesCache_.end(); ) {
            if (it.key().startsWith(prefix))
                it = readingRunLinesCache_.erase(it);
            else
                ++it;
        }
        readingRunLinesLru_.removeIf([&prefix](const QString &k) { return k.startsWith(prefix); });
        // The reference grid is the active style's: re-measure the run in place, at the
        // reader's own page and offset, exactly as a changed fit always did.
        if (key == active && lineSourceTextOnly_) {
            const QVector<int> starts = styleRunLinesForText(key, readingRunText_).charStarts;
            if (!starts.isEmpty()) {
                const int localChar = lineSourceTextOnly_
                    ? qMax(0, preserveChar - readingTextWindowCharStart_) : preserveChar;
                const auto at = std::upper_bound(starts.constBegin(), starts.constEnd(), localChar);
                lineOffset_ = qMax(0, static_cast<int>(at - starts.constBegin()) - 1);
                readingScroll_ = lineOffset_;
            }
            readingTextSourceInitialPositionPending_ = false;
            applyReadingRestage(true, /*forceRebuild=*/ true);
        } else if (key == active) {
            applyReadingRestage(true, /*forceRebuild=*/ true);
        }
    } else if (key == active && lineSourceTextOnly_) {
        // This surface already reports the exact column used to seed the grid.
        readingTextSourceInitialPositionPending_ = false;
    }
    return size;
}

qreal LiveOutputService::readingRunSizeForStyle(const QString &styleId) const
{
    // An empty id means the REFERENCE grid, which is the active style's own column
    // (see styleRunLinesForText's reportKey): answer for that style rather than for
    // a key nothing is ever filed under, or a surface that asks for "the reference
    // size" is told there is none.
    QString key = styleId;
    if (key.isEmpty()) {
        const auto *model = OutputListModel::instance();
        key = model ? model->activeStyleId() : QString();
    }
    const auto it = readingRunSizeForStyle_.constFind(key);
    return it == readingRunSizeForStyle_.constEnd() ? -1.0 : it.value();
}

QVariantList LiveOutputService::readingRunLineStarts(const QString &styleId) const
{
    QVariantList out;
    const QVector<int> starts = styleRunLines(styleId).charStarts;
    out.reserve(starts.size());
    for (int start : starts)
        out.append(start);
    return out;
}

// THE SAME POSITION IN ANOTHER STYLE'S LINES — the conversion that keeps two
// differently-styled outputs on the same words. The last line whose start is at or
// before the character, plus the sub-line fraction so a glide stays smooth.
qreal LiveOutputService::readingLineForStyle(const QString &styleId, qreal charAt) const
{
    const QVector<int> starts = styleRunLines(styleId).charStarts;
    if (starts.isEmpty())
        return lineSourceTextOnly_ ? readingScroll_ : charAt;
    if (charAt < 0.0)
        return charAt;
    if (lineSourceTextOnly_)
        charAt = qMax(0.0, charAt - readingTextWindowCharStart_);
    // THE CHARACTER, IN WHOLE CHARACTERS. readingChar is a reference line's first
    // character plus the glide's progress through that line (a fraction of a LINE,
    // 0..1 — see readingCharForScroll). Added to this style's line index as it stood,
    // that fraction moved this output at the wrong rate, and its line number only
    // changed when the sweep crossed one of its own line starts: the reference output
    // glided while the other ticked, sitting still for presses and then jumping a line
    // or two at a paragraph stub (measured: s2 went 7 -> 9 while s1 went 7 -> 8). So
    // the fraction is turned back into the characters it spans, and this style's line
    // is where that character falls in ITS OWN line — a fraction of a line: the same
    // words at the same moment on every output.
    const auto *referenceModel = OutputListModel::instance();
    const QString referenceId = referenceModel ? referenceModel->activeStyleId() : QString();
    const QVector<int> refStarts = readingRunCharStarts(referenceId);
    const qreal whole = std::floor(charAt);
    const qreal fraction = charAt - whole;
    qreal character = whole;
    if (!refStarts.isEmpty() && fraction > 0.0) {
        const auto refIt = std::upper_bound(refStarts.constBegin(), refStarts.constEnd(),
                                            static_cast<int>(whole));
        const int refIndex = (refIt == refStarts.constBegin())
            ? 0 : static_cast<int>(refIt - refStarts.constBegin()) - 1;
        if (refIndex + 1 < refStarts.size() && refStarts.at(refIndex) == static_cast<int>(whole))
            character = whole + fraction * (refStarts.at(refIndex + 1) - refStarts.at(refIndex));
    }
    const auto it = std::upper_bound(starts.constBegin(), starts.constEnd(),
                                     static_cast<int>(std::floor(character)));
    const int index = (it == starts.constBegin()) ? 0 : static_cast<int>(it - starts.constBegin()) - 1;
    // This line's length: to the next line's start; the last line has no next, so the
    // one before it stands in (never zero).
    int span = 1;
    if (index + 1 < starts.size())
        span = qMax(1, starts.at(index + 1) - starts.at(index));
    else if (index > 0)
        span = qMax(1, starts.at(index) - starts.at(index - 1));
    return static_cast<qreal>(index) + (character - starts.at(index)) / span;
}

// THE STYLE-INDEPENDENT PLACE IN THE PASSAGE — the character the line at the top
// of the box begins with, measured in the REFERENCE grid (the active style, whose
// lines every number in this service is counted in). Two grids can place the same
// line number on different words; both can agree on this offset, because both lay
// out the same run text. An empty run has no position (-1).
int LiveOutputService::readingCharOffset() const
{
    if (readingRunText_.isEmpty() || readingRunFirst_ < 0)
        return -1;
    const qreal at = readingCharForScroll(readingScroll_);
    const int local = static_cast<int>(std::floor(at));
    return lineSourceTextOnly_ ? readingTextWindowCharStart_ + local : local;
}

// The run-relative offset plus the run's own origin: the position in the PICK's
// passage, which a sliding window does not move (see the header).
int LiveOutputService::readingPassageChar() const
{
    const int local = readingCharOffset();
    if (local < 0)
        return -1;
    return lineSourceTextOnly_ ? local : readingRunCharBase_ + local;
}

// HOW MANY LINES THIS PAGE OCCUPIES IN THE PASSAGE — the unit the step moves in.
// The passage's own table when the page is in the run (see readingRunPageLines_),
// and the page's own measurement only when there is no run to ask (a single-page
// pick, or a page outside the window) — where the two differ, the passage wins,
// because that is the grid readingScroll_ indexes and every surface draws.
int LiveOutputService::readingPageLineCount(int index) const
{
    const auto it = readingRunPageLines_.constFind(index);
    if (it != readingRunPageLines_.constEnd())
        return it.value();
    if (lineSourceTextOnly_)
        return 0;
    return static_cast<int>(readingLinesAt(index).size());
}

// WHERE THIS PAGE'S FIRST LINE SITS IN THE PASSAGE — the index readingScroll_ is
// built on (readingScroll_ = this + lineOffset_), so it is counted in the passage's
// table for every page the run holds. The per-page accumulation below is the
// no-run fallback: it counts lines at each page's own fitted size, which is a
// different grid, and is kept only for picks with no passage to index.
int LiveOutputService::readingBaseForSlide(int index) const
{
    if (readingRunFirst_ < 0 || index <= readingRunFirst_)
        return 0;
    const auto inRun = readingRunPageStart_.constFind(index);
    if (inRun != readingRunPageStart_.constEnd()) {
        const auto *model = OutputListModel::instance();
        const QString ref = model ? model->activeStyleId() : QString();
        const QVector<int> starts = styleRunLinesForText(ref, readingRunText_).charStarts;
        const int at = inRun.value();
        int count = 0;
        for (int s : starts) {
            if (s < at)
                ++count;
            else
                break;
        }
        return count;
    }
    int base = 0;
    for (int i = readingRunFirst_; i < index && i < static_cast<int>(lineSetRaw_.size()); ++i)
        base += readingPageLineCount(i);
    return base;
}

void LiveOutputService::shiftPickBase(int from, int to)
{
    if (to > from) {
        for (int i = from; i < to; ++i)
            readingPickBase_ += readingPageLineCount(i);
    } else if (to < from) {
        for (int i = to; i < from; ++i)
            readingPickBase_ -= readingPageLineCount(i);
    }
}

// WHERE IN THE PICK the words are — see the header: the page's own start plus
// the offset inside it, which is the same picture regardless of where the run
// window happens to be. With no run yet (a pick that just started) the offset is
// the whole answer, which is what the two expressions below say.
qreal LiveOutputService::readingAbsoluteLine() const
{
    const qreal inside = (readingRunFirst_ >= 0)
        ? (readingScroll_ - readingRunBaseLine_)
        : static_cast<qreal>(lineOffset_);
    return static_cast<qreal>(readingPickBase_) + inside;
}

qreal LiveOutputService::readingWindowOffset(qreal absolute) const
{
    const qreal inside = absolute - static_cast<qreal>(readingPickBase_);
    return (readingRunFirst_ >= 0)
        ? (static_cast<qreal>(readingRunBaseLine_) + inside)
        : inside;
}

void LiveOutputService::announceReadingPage()
{
    const int page = readingPage();
    if (readingPageAnnounced_ == page)
        return;
    readingPageAnnounced_ = page;
    emit readingPageChanged();
}

// THE RUN: the page being read and kReadingRunRadius pages on each side, joined
// into one passage by the same newline join the two-page passage used (the
// engine's own layout is what turns it into lines, so the page inside the run
// breaks exactly where the page alone broke).
//
// WHY A RUN AND NOT page+next: with two pages, every page turn had to COMMIT a
// new page — new blocks, a new fit, an offset back to 0 — and that re-base is
// what the operator kept seeing as a blink between items while following the
// words. With a run, a turn only moves readingRunBaseLine_; `readingScroll_`
// keeps climbing through one column that is already on screen.
//
// REBUILDS ARE FREE TO THE EYE, which is the whole point: when the window does
// have to shift (the reader has walked near its edge), readingScroll_ is
// rebased by exactly the lines dropped from the front. The drawn column loses
// those same lines above the offset, so the visible words — and therefore the
// pixels — do not move.
void LiveOutputService::rebuildReadingRun(int anchor, bool force)
{
    const int count = static_cast<int>(lineSetRaw_.size());
    if (count == 0) {
        resetReadingRun();
        return;
    }
    // A small slide pick is kept whole to avoid a mid-walk refill. Larger slide sets
    // use a bounded window. JSON scroll reaches this with one full-passage reading
    // unit, so it also keeps one stable wrap grid instead of reflowing at each
    // max-words slice.
    constexpr int kReadingRunWholePick = 2 * LiveOutputService::kReadingRunRadius * 2 + 1;
    const bool wholePick = !lineSourceTextOnly_ && count <= kReadingRunWholePick;
    const int first = wholePick ? 0 : qMax(0, anchor - kReadingRunRadius);
    const int last = wholePick ? count - 1 : qMin(count - 1, anchor + kReadingRunRadius);
    if (!force && first == readingRunFirst_ && last == readingRunLast_)
        return;                        // same window: the column is unchanged
    // (FORCED: the window is the same but the MEASUREMENT is not — the per-page
    // counts and the base below are derived from the table, so they have to be
    // recomputed even though the text is identical. Everything that depends on the
    // window having MOVED stays put: `previousFirst` is only used to move the
    // character origin and the offset by what the window dropped or took on, and a
    // forced rebuild keeps both ends, so both adjustments are zero.)
    // What the picture is NOW, in the old run's numbering (before the window
    // moves). -1 when there was no run yet (a pick that is just starting).
    const int previousBase = readingRunFirst_ >= 0
        ? readingBaseForSlide(lineSlideIndex_) : -1;
    const int previousFirst = readingRunFirst_;
    readingRunFirst_ = first;
    readingRunLast_ = last;
    // THE STABLE CHARACTER ORIGIN (see readingPassageChar): the pages this window
    // drops stop being counted, the ones it takes on start. A fresh run counts its
    // own prefix from the pick's beginning; a slide moves the same total by the
    // pages that entered or left, so the number the surfaces report never jumps
    // backwards merely because the window slid.
    if (previousFirst < 0)
        readingRunCharBase_ = readingCharSpan(0, first);
    else if (first > previousFirst)
        readingRunCharBase_ += readingCharSpan(previousFirst, first);
    else if (first < previousFirst)
        readingRunCharBase_ -= readingCharSpan(first, previousFirst);
    // ONE BREAK PER PAGE BOUNDARY — the pages' own blank edges are the pages' own.
    //
    // This is the mismatch that made the words move at a page turn. The step's
    // arithmetic counts lines PER PAGE (`readingLinesAt`), while the surfaces draw
    // and convert through the run's OWN table; the two agree only if the joined run
    // has exactly the lines its pages have. A page that ends with a newline (or
    // begins with one) has that blank edge STRIPPED from its own count ("leading/
    // trailing blank lines are whitespace, not reading") but contributes an extra
    // EMPTY LINE to the middle of the run, where it is an interior blank and is
    // kept: the run's table then counts a line the step's arithmetic never counts,
    // once per page boundary, so every page turn moved the drawn passage by that
    // line. Measured with tools/selftest/run_table_walk_test.sh: one output stepped
    // 70 characters back at press 26, and ~300 characters forward at presses
    // 69/97/112, with the step trace reporting exactly +1 line each time.
    //
    // The FIRST page's text is appended VERBATIM: an on-air slide must still begin
    // with this page's own reading text (SceneBuilder's “the passage does not begin
    // with this slide's reading text” guard), so the fix can only touch the edges
    // BETWEEN pages. A single break is then the only thing between them, and the
    // run's table has the same lines the pages have.
    QString run;
    readingRunPageStart_.clear();
    readingRunPageLines_.clear();
    for (int i = first; i <= last; ++i) {
        QString page = lineSourceTextOnly_
            ? readingFlowText_ : readingTextOf(lineSetRaw_.at(i).toMap());
        if (page.trimmed().isEmpty())
            continue;
        if (!run.isEmpty()) {
            if (lineSourceTextOnly_) {
                // Reading-only chunks are adjacent slices of the original JSON
                // text. Keep their exact boundary; a slide-style newline here
                // would insert a visible gap every max-words screenful.
                readingRunPageStart_.insert(i, static_cast<int>(run.size()));
                run += page;
                continue;
            }
            // an edge is whitespace: the break before this page, and this page's own
            // leading blank lines, are one boundary — not a boundary plus a blank line
            while (page.startsWith(QLatin1Char('\n')) || page.startsWith(QLatin1Char('\r')))
                page.remove(0, 1);
            while (run.endsWith(QLatin1Char('\n')) || run.endsWith(QLatin1Char('\r')))
                run.chop(1);
            run += QLatin1Char('\n');
        } else {
            // ...and the same for the run's own start, whose leading blanks its table
            // strips anyway (visualLinesOfText)
            while (page.endsWith(QLatin1Char('\n')) || page.endsWith(QLatin1Char('\r')))
                page.chop(1);
        }
        // WHERE THIS PAGE BEGINS IN THE PASSAGE — the anchor every per-page answer is
        // derived from below (see readingPageLineCount), recorded as the run is built
        // so it can never disagree with the text it describes.
        readingRunPageStart_.insert(i, static_cast<int>(run.size()));
        run += page;
    }
    readingRunText_ = run;
    // ---- EACH PAGE'S LINES, IN THE PASSAGE'S OWN TABLE ----------------------
    //
    // Not readingLinesAt: that answers "how many lines does this page take at the
    // size IT is fitted to", which is a different grid from the passage (one column,
    // one fitted size for the whole run — measured 28 lines per-page vs 17 in the
    // passage for the same seven pages). readingScroll_ indexes the passage's table,
    // so the step must count in that table or the two drift by the ratio between the
    // two grids, which is exactly "it works for the first few pages and then stops
    // scrolling correctly".
    {
        const auto *model = OutputListModel::instance();
        const QString refStyle = model ? model->activeStyleId() : QString();
        const QVector<int> starts = styleRunLinesForText(refStyle, run).charStarts;
        if (!starts.isEmpty()) {
            QList<int> pages = readingRunPageStart_.keys();
            std::sort(pages.begin(), pages.end());
            for (int k = 0; k < pages.size(); ++k) {
                const int from = readingRunPageStart_.value(pages.at(k));
                const int to = (k + 1 < pages.size()) ? readingRunPageStart_.value(pages.at(k + 1))
                                                      : static_cast<int>(run.size()) + 1;
                int count = 0;
                for (int s : starts) {
                    if (s < from)
                        continue;
                    if (s >= to)
                        break;
                    ++count;
                }
                readingRunPageLines_.insert(pages.at(k), count);
            }
        }
    }
    readingRunBaseLine_ = readingBaseForSlide(lineSlideIndex_);
    // THE WINDOW'S SHAPE, when the window moves: the run's own line table beside the
    // step's per-page line counts. These two ARE the position — one backs the words
    // the surfaces draw, the other the line the step thinks it is on — and a page
    // turn only keeps the picture still while they agree. One line per window move,
    // next to ReadRun, for the same reason it exists: "the words moved when it turned
    // the page" needs the numbers, not a re-run.
    {
        const auto *model = OutputListModel::instance();
        const QString refStyle = model ? model->activeStyleId() : QString();
        int pageSum = 0;      // the step's counts — the passage's table (readingPageLineCount)
        int pageOwnSum = 0;   // the page's own fitted grid, for the comparison that found this
        QString pageLines;
        for (int i = first; i <= last; ++i) {
            const int n = readingPageLineCount(i);
            const int own = lineSourceTextOnly_
                ? 0 : static_cast<int>(readingLinesAt(i).size());
            pageSum += n;
            pageOwnSum += own;
            if (i > first)
                pageLines += QLatin1Char(',');
            pageLines += QString::number(n);
        }
        // The first number is the invariant this file turns on: the step's counts must
        // ADD UP to the passage's table (tableLines) or the step is walking a different
        // grid from the one every surface draws.
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadRunShape"),
                            QStringLiteral("window=[%1..%2] runChars=%3 tableLines=%4 "
                                           "stepLines=%5 pageOwnSum=%6 pageLines=[%7] "
                                           "(stepLines == tableLines is the requirement; "
                                           "pageOwnSum is the per-page fitted grid)")
                                .arg(first)
                                .arg(last)
                                .arg(run.size())
                                .arg(styleRunLinesForText(refStyle, run).lines.size())
                                .arg(pageSum)
                                .arg(pageOwnSum)
                                .arg(pageLines));
    }
    // WINDOW-MOVE DIAGNOSTIC, one line only when the window ACTUALLY moves (a
    // sliding pick: every kReadingRunRadius pages). The scroll number is rebased
    // by exactly the lines dropped at the front here, so a trace that samples
    // readingScroll per frame sees a step at this instant that the PICTURE does
    // not have — and readingAbsoluteLine (which subtracts this base) is the
    // number that stays continuous across it. Logged for the same reason
    // LineStep is: "the window slid and the words skipped" and "the steps are
    // wrong" are different reports with the same symptom.
    if (previousBase >= 0) {
        // Keep the SAME line at the top of the box: the column now starts
        // `shift` lines further along, so the offset must too.
        //
        // KNOWN SHORTCOMING, MEASURED (tools/selftest/run_table_walk_test.sh):
        // `shift` is a LINE COUNT of the pages that left the window, and a page's
        // own line count (readingLinesAt) is not the number of lines those same
        // words take in the RUN's wrap — a Table page carries its reference line
        // beside the paragraph the run is made of. The character the top of the box
        // reports therefore moves by a line's worth at a page commit: on a real
        // walk, both outputs stepped ~200 characters BACKWARDS at presses 41 and 43,
        // and output 0 ~417 FORWARDS at press 15. The words drawn are the run's
        // text at that character, so this is the visible jump —
        // ``the words go back / it blinks when it turns the page''. Rebasing by
        // character instead was tried and is worse: readingScroll_ is also the
        // step's own line index (synchronised from lineOffset_), so writing it from
        // the character channel alone made the two disagree and oscillate ~200
        // characters per press. The fix belongs in the step arithmetic: derive
        // lineOffset_/the page bases from the run's own table, so one measurement
        // backs both the offset and the drawn words.
        const int shift = readingRunBaseLine_ - previousBase;
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadRun"),
                            QStringLiteral("window=[%1..%2] base=%3 rebase=%4 scroll=%5")
                                .arg(first)
                                .arg(last)
                                .arg(readingRunBaseLine_)
                                .arg(shift)
                                .arg(readingScroll_, 0, 'f', 2));
        if (shift != 0)
            readingScroll_ += shift;
    }
}

// A STYLE CHANGE UNDER A WALK — see the header. The cache is keyed by INDEX and
// is cleared for a new pick only, so without this the step would keep running on
// counts measured against the composition that has just been replaced.
void LiveOutputService::reseadReadingWalk()
{
    if (lineSetRaw_.isEmpty() || lineSlideIndex_ < 0
            || lineSlideIndex_ >= lineSetRaw_.size())
        return;
    readingLinesCache_.clear();   // every count belongs to the OLD picture
    resetReadingRun();            // ...and so does the run built from them
    if (readingScrollAnim_)
        readingScrollAnim_->stop();
    readingRebaseSlide_ = -1;     // no page change owed by the old numbering
    // The reader keeps their place in the page wherever the new layout still has
    // one; a page that re-wraps shorter than the offset clamps to its last line.
    const int ownLines = readingPageLineCount(lineSlideIndex_);
    lineOffset_ = qBound(0, lineOffset_, qMax(0, ownLines - 1));
    readingPickBase_ = 0;         // the walk's origin is the page it is reading
    applyReadingRestage();        // rebuild the run, re-stage, announce the page
    // SAID OUT LOUD. A re-seat is the one event in this file that CAN move the
    // words without a key, so it gets a line of its own: "it jumped while I was
    // reading" and "I changed the style and it jumped" are different reports, and
    // only this line tells them apart in a log. Information level, like the
    // LineStep and ReadRun diagnostics beside it.
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadReseat"),
                        QStringLiteral("set=%1 page=%2 lines=%3 offset=%4 scroll=%5 "
                                       "(the measurement basis changed: style/roster)")
                            .arg(lineSetName_)
                            .arg(lineSlideIndex_)
                            .arg(ownLines)
                            .arg(lineOffset_)
                            .arg(readingScroll_, 0, 'f', 2));
}

void LiveOutputService::connectReadingBasis()
{
    if (readingBasisConnected_)
        return;
    if (auto *model = OutputListModel::instance()) {
        readingBasisConnected_ = true;
        // ONE signal is the whole basis: activeStyleChanged fires on every path
        // that can change the box a page is wrapped at or the size it is drawn at
        // — a style edit, a roster change, choosing/restyling the on-air output,
        // and the design-library templates' own change (OutputListModel's emit
        // sites). NOT the output roster: enabling an output does not re-measure
        // anything by itself, and re-seating the walk for it would be an
        // involuntary re-base — the one thing this whole file exists to avoid.
        connect(model, &OutputListModel::activeStyleChanged,
                this, &LiveOutputService::reseadReadingWalk);
    }
}

// THE ONE NUMBER, IN A UNIT EVERY STYLE AGREES ON. `readingScroll_` is in LINES of
// the reference grid (the active style's) — the unit this service's own arithmetic
// works in (bases, offsets, bounds) — and it is converted HERE, once per frame,
// into the CHARACTER OFFSET into the passage that the engine draws with. A line
// number would be a different word in every output whose style wraps differently:
// measured on the operator's two styles, the same number put "between us, that
// knows what your t…" on one output and "How do you do? We are strangers to…" on
// the other. The character offset is the position in the words; each surface turns
// it into its own line heights (the engine through TextObject::FlowLineForChar,
// the tile through readingLineForStyle).
void LiveOutputService::applyReadingScroll(qreal lines)
{
    readingScroll_ = lines;
    bool windowChanged = false;
    if (lineSourceTextOnly_) {
        const int passageChar = readingTextWindowCharStart_
            + static_cast<int>(std::floor(readingCharForScroll(lines)));
        const int windowEnd = readingTextWindowCharStart_ + readingFlowText_.size();
        const bool nearEdge = passageChar < readingTextWindowCharStart_ + 1200
                              || passageChar > windowEnd - 1200;
        if (nearEdge && (readingScrollPreparing_ || (readingScrollAnim_
                && readingScrollAnim_->state() == QAbstractAnimation::Running))) {
            readingWindowRefreshPending_ = true;
        } else if (nearEdge) {
            windowChanged = refreshReadingTextWindow(passageChar);
            readingWindowRefreshPending_ = false;
            if (windowChanged) {
                const auto *model = OutputListModel::instance();
                const QString ref = model ? model->activeStyleId() : QString();
                const QVector<int> starts = styleRunLinesForText(ref, readingRunText_).charStarts;
                readingRunPageLines_.insert(0, starts.size());
                readingLayoutTargetChar_ = passageChar;
                const qreal localLine = starts.isEmpty()
                    ? 0.0 : readingLineForStyle(ref, passageChar);
                lineOffset_ = localLine;
                readingScroll_ = localLine;
                lines = localLine;
                if (!starts.isEmpty())
                    readingLayoutTargetChar_ = -1;
            }
        }
    }
    if (lineSourceTextOnly_) {
        const int page = readingPage();
        if (page != readingPageAnnounced_) {
            readingPageAnnounced_ = page;
            emit readingPageChanged();
        }
    }
    // Atomics only, from the interface thread — no lock, no device (see
    // LiveOutputController::SetFlowChar).
    bps::live::LiveOutputController::Instance().SetFlowChar(
        lineSourceTextOnly_ ? readingFlowCharForScroll(lines) : readingCharForScroll(lines));
    if (windowChanged) {
        if (live_) {
            QVariantList current = onAirSlidesRaw();
            if (!current.isEmpty()) {
                QVariantMap shown = current.first().toMap();
                shown[QStringLiteral("flowText")] = readingFlowText_;
                shown[QStringLiteral("flowChar")] = readingFlowCharForScroll(lines);
                goLiveWithSlides(onAirTitle_, QVariantList{shown});
            }
        } else if (!stagedSlidesRaw_.isEmpty()) {
            QVariantMap shown = stagedSlidesRaw_.first().toMap();
            shown[QStringLiteral("flowText")] = readingFlowText_;
            shown[QStringLiteral("flowChar")] = readingFlowCharForScroll(lines);
            stagedSlidesRaw_ = QVariantList{shown};
            stagedSlide_ = previewWithActiveStyle(shown);
            emit stagedChanged();
        }
    }
    emit readingScrollChanged();
}

// WHERE A REFERENCE-GRID LINE LANDS IN THE PASSAGE: the character its first line
// starts at, plus the fraction of the move still in flight (a glide interpolates
// between two line indices, and every surface needs that sub-line progress to
// stay smooth). O(1) — the reference grid's character starts are already in the
// per-style table (see readingRunLinesForStyle's cache), so the per-frame path
// costs one table lookup.
qreal LiveOutputService::readingCharForScroll(qreal lines) const
{
    const auto *model = OutputListModel::instance();
    const QString ref = model ? model->activeStyleId() : QString();
    const QVector<int> starts = readingRunCharStarts(ref);
    if (starts.isEmpty())
        return static_cast<qreal>(lines);   // no table (no run): the number is lines
    const int count = starts.size();
    const qreal clamped = qBound(0.0, lines, static_cast<qreal>(count));
    const int index = qMin(count - 1, static_cast<int>(std::floor(clamped)));
    return static_cast<qreal>(starts.at(index)) + (clamped - std::floor(clamped));
}

// A LANDED STEP THAT CROSSED A PAGE BOUNDARY. The offset it landed on IS the
// new page at the top of the box, so committing the page here changes
// everything around the reading block (a reference, a heading, the slide
// number) and nothing the reader is looking at — which is what makes the
// boundary one move instead of a cut and a scroll. See the header comment
// for why this also has to be reachable synchronously, not only from
// readingScrollAnim_'s own finished handler.
void LiveOutputService::commitPendingRebase()
{
    if (readingRebaseSlide_ < 0)
        return;   // nothing owed: the words are already where they belong
    const int slide = readingRebaseSlide_;
    readingRebaseSlide_ = -1;
    shiftPickBase(lineSlideIndex_, slide);   // the page moved: its origin moves too
    lineSlideIndex_ = slide;
    lineOffset_ = 0;
    // NO RESET TO 0: the offset is measured from the RUN's start, and landing on
    // the next page's first line is exactly base+0 of that page — the same line,
    // the same pixel. The old reset to 0 here was the visible re-base.
    // (The page change is announced BY this restage, after it has staged — see
    // announceReadingPage: announcing before it left every composing surface one
    // page behind, because it recomposed while the OLD page was still in place.)
    applyReadingRestage();                       // commits the page inside the run
    applyReadingScroll(readingScroll_);          // ...and tells the engine the same
}

void LiveOutputService::animateReadingScroll(qreal from, qreal to)
{
    if (!readingScrollAnim_) {
        readingScrollAnim_ = new QVariantAnimation(this);
        connect(readingScrollAnim_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) { applyReadingScroll(v.toReal()); });
        connect(readingScrollAnim_, &QVariantAnimation::finished, this,
                [this]() {
                    commitPendingRebase();
                    if (readingWindowRefreshPending_) {
                        readingWindowRefreshPending_ = false;
                        applyReadingScroll(readingScroll_);
                    }
                    drainPendingReadingSteps();
                });
    }
    // THE OPERATOR'S PACE, from the same two keys the old glide read
    // (Settings › Slide options › Reading step speed / motion) — one pair for
    // every tab, because the step is the motion of the words, not a property of
    // what they say. Read at every press, so a change is in force on the next one.
    const QVariant speed = SettingsService::instance().value(QStringLiteral("reading.stepMs"));
    const int ms = qBound(60, speed.isValid() ? speed.toInt() : 190, 600);
    const QString curve = SettingsService::instance()
                              .value(QStringLiteral("reading.stepEasing"))
                              .toString();
    readingScrollAnim_->stop();
    readingScrollPreparing_ = true;
    applyReadingScroll(from);   // starts where the picture already is
    readingScrollPreparing_ = false;
    // CONFIGURED SILENTLY. A finished animation sits parked at its END time, so
    // setEndValue(to) below made it announce the NEW TARGET at once (valueChanged
    // before start() rewound it) — every press published from -> to -> from: the
    // position ran a line ahead for one call and back (measured in the ScrollJump
    // log: "27 -> 26", "26 -> 27", "27 -> 26" inside 10 ms). Nothing is announced
    // until start(), whose first value is `from`.
    {
        const QSignalBlocker quiet(readingScrollAnim_);
        readingScrollAnim_->setStartValue(from);
        readingScrollAnim_->setEndValue(to);
        readingScrollAnim_->setDuration(ms);
        // EASE IN AND OUT by default: zero velocity at both ends is what reads as a
        // scroll rather than a jump that happens to be animated.
        readingScrollAnim_->setEasingCurve(curve == QLatin1String("even") ? QEasingCurve::Linear
                                            : curve == QLatin1String("settle") ? QEasingCurve::OutCubic
                                                                               : QEasingCurve::InOutCubic);
    }
    readingScrollAnim_->start();
}

void LiveOutputService::drainPendingReadingSteps()
{
    if (pendingReadingSteps_ == 0 || !lineSourceTextOnly_ || !scrollMode_)
        return;
    if (readingScrollAnim_ && readingScrollAnim_->state() == QAbstractAnimation::Running)
        return;

    const int direction = pendingReadingSteps_ > 0 ? 1 : -1;
    pendingReadingSteps_ -= direction;
    if (!lineStep(direction))
        pendingReadingSteps_ = 0;
}

// The page as it stands, staged or on air, carrying the passage it begins in and
// the offset it rests at. THE PAGE'S OWN TEXT IS NEVER REWRITTEN — that is the
// whole change: a re-cut page was also a re-FITTED page (the fit solver runs on
// every build), so the type size and the wrapping moved while the words were
// supposed to travel, and the design's box could end up holding a column of
// short lines instead of the paragraph the operator approved.
void LiveOutputService::applyReadingRestage(bool commitPage, bool forceRebuild,
                                            bool forceScrollNotify)
{
    if (lineSetRaw_.isEmpty() || lineSlideIndex_ < 0 || lineSlideIndex_ >= lineSetRaw_.size())
        return;
    // WHERE IN THE PASSAGE THE WORDS ARE, before the run is touched, so a commit
    // that moved the text can be told from one that kept it (ReadContinuity below).
    const int passageBefore = readingPassageChar();
    // THE RUN, NOT ONE PAGE — rebuilt only when the window must move, and the
    // offset rebased with it (see rebuildReadingRun), so a page turn inside the
    // window changes no visible pixel at all.
    rebuildReadingRun(lineSlideIndex_, forceRebuild);
    // THE BASE IS ALWAYS THE CURRENT PAGE'S. rebuildReadingRun only recomputes
    // readingRunBaseLine_ when the WINDOW has to move, and a pick that fits the
    // run radius is its own window — it never moves, so the base stayed at the
    // page the run was built for (0) for the whole walk. The offset below then
    // snapped back to the run's first line on every boundary while the page
    // index advanced correctly: the words re-based at each page turn, which is
    // exactly the blink the operator reported ("it works from 1 to 1-3, then it
    // misbehaves"). Measured by tools/selftest/run_line_step_test.sh, which
    // presses the real step 40 times and asserts the offset grows by 1.0 and
    // never restarts. Costing only cached line counts, so it is recomputed here
    // on every restage rather than trusted from whenever the window last moved.
    readingRunBaseLine_ = readingBaseForSlide(lineSlideIndex_);
    const QString previousFlow = readingFlowText_;
    const qreal previousScroll = readingScroll_;
    readingScroll_ = readingRunBaseLine_ + lineOffset_;
    if (lineSourceTextOnly_) {
        const int passageChar = readingTextWindowCharStart_
            + static_cast<int>(std::floor(readingCharForScroll(readingScroll_)));
        if (refreshReadingTextWindow(passageChar)) {
            const auto *model = OutputListModel::instance();
            const QString ref = model ? model->activeStyleId() : QString();
            const QVector<int> starts = styleRunLinesForText(ref, readingRunText_).charStarts;
            readingRunPageLines_.insert(0, starts.size());
            readingScroll_ = readingLineForStyle(ref, passageChar);
            lineOffset_ = readingScroll_;
            if (!starts.isEmpty())
                readingLayoutTargetChar_ = -1;
        }
    }
    const bool flowChanged = readingFlowText_ != previousFlow;
    // ONE LINE WHEN THE RUN'S TEXT CHANGED: the character the reader is on before
    // and after. They must be equal (the window slid under a still picture); a
    // difference is the words themselves moving at the page commit — the text in
    // the output jumping. -1 is "no run yet" and says nothing.
    if (flowChanged && passageBefore >= 0) {
        const int passageAfter = readingPassageChar();
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadContinuity"),
                            QStringLiteral("slide=%1 passageBefore=%2 passageAfter=%3 delta=%4 "
                                           "linesBefore=%5 linesAfter=%6 (delta != 0: the text moved)")
                                .arg(lineSlideIndex_)
                                .arg(passageBefore).arg(passageAfter)
                                .arg(passageAfter - passageBefore)
                                .arg(previousScroll, 0, 'f', 2)
                                .arg(readingScroll_, 0, 'f', 2));
    }
    // THE POSITION CHANGED, not only the text. A DIRECT JUMP (a click on another
    // paragraph, the pane's ‹ ›, a search pick) stages a page whose position is
    // base+0 while the passage's TEXT is the same run it already had — so
    // `flowChanged` is false, and the announcement below used to be skipped
    // entirely. Every surface that draws by this number (DesignPreview's stepLine
    // → readingChar → readingScroll, the monitor tiles, the output window) then
    // kept the offset it was last told — 0, announced by stageSlideSet before the
    // run existed — and drew the passage from its FIRST line while the pane's own
    // preview (which draws the composed page, not the flow) showed the right
    // paragraph: reported as "it gets stuck and doesn't update but the preview
    // pane changes", and as ‹ › "trying to line step" instead of moving to the
    // next passage. Measured in engine.log: ReadRestage slide=2 base=14.00
    // scroll=14.00 with no readingScrollChanged, while both tiles' composition
    // was already the new page (RefreshOnAirSlide page=2, blocks0Text of 1-2).
    const bool scrollChanged = !readingStepGlide_
        && (forceScrollNotify
            || !qFuzzyCompare(previousScroll + 1.0, readingScroll_ + 1.0));
    // THE RESULT, every time this runs — not just when LineStep calls it. A
    // call from noteReadingFit's reset cascade, or from a fresh pick, lands
    // here with no preceding LineStep line, so without this the glide this
    // produced is invisible: readingScroll jumping with no LineStep right
    // before it IS the trace of an unprompted restage, which is otherwise
    // only inferable from a side effect (ReadFit re-firing) three calls away.
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("ReadRestage"),
                        QStringLiteral("slide=%1 base=%2 lineOffset=%3 scroll=%4 "
                                       "flowChanged=%5 commitPage=%6 live=%7 "
                                       "scrollChanged=%8 (true = the new position was announced)")
                            .arg(lineSlideIndex_)
                            .arg(static_cast<double>(readingRunBaseLine_), 0, 'f', 2)
                            .arg(static_cast<double>(lineOffset_), 0, 'f', 2)
                            .arg(readingScroll_, 0, 'f', 2)
                            .arg(flowChanged ? QStringLiteral("yes") : QStringLiteral("no"))
                            .arg(commitPage ? QStringLiteral("yes") : QStringLiteral("no"))
                            .arg(live_ ? QStringLiteral("yes") : QStringLiteral("no"))
                            .arg(scrollChanged ? QStringLiteral("yes") : QStringLiteral("no")));
    // PREWARM THE NEIGHBOURHOOD, queued for the NEXT event-loop turn (after
    // this call's own step has already drawn) — see prewarmReadingLines's
    // own header comment. Every page the stepper can land on next or come
    // back to runs through here, so one call site covers forward and
    // backward both.
    QTimer::singleShot(0, this, &LiveOutputService::prewarmReadingLines);
    const int page = readingPage();
    const bool pageChanged = (readingPageAnnounced_ != page);
    if (!readingStepGlide_)
        readingPageAnnounced_ = page;
    if (lineSourceTextOnly_) {
        if (live_) {
            if (flowChanged) {
                QVariantList current = onAirSlidesRaw();
                if (!current.isEmpty()) {
                    QVariantMap shown = current.first().toMap();
                    shown[QStringLiteral("flowText")] = readingFlowText_;
                    shown[QStringLiteral("flowChar")] = readingFlowCharForScroll(readingScroll_);
                    goLiveWithSlides(onAirTitle_, QVariantList{shown});
                }
            }
        } else if (flowChanged && !stagedSlidesRaw_.isEmpty()) {
            QVariantMap shown = stagedSlidesRaw_.first().toMap();
            shown[QStringLiteral("flowText")] = readingFlowText_;
            shown[QStringLiteral("flowChar")] = readingFlowCharForScroll(readingScroll_);
            stagedSlidesRaw_ = QVariantList{shown};
            stagedSlide_ = previewWithActiveStyle(shown);
            emit stagedChanged();
        }
        if (pageChanged && !readingStepGlide_)
            emit readingPageChanged();
        if (!readingStepGlide_ && (flowChanged || scrollChanged))
            emit readingScrollChanged();
        return;
    }
    // SETTLED HERE, BUT NOT ANNOUNCED YET — see the emit sites below for why
    // the split. OutputMonitorTile's onAirSlide memoizes its composed blocks
    // per page, keyed in part on readingPage precisely so a step inside a
    // page does not recompose it. That key is exactly the trap if BOTH
    // halves of "which page, and what its raw blocks are" are not already
    // final by the time ANY signal touching this memo fires: announcing the
    // page change here but emitting it immediately (the first attempt at
    // this fix) moved readingPage to the NEW page before stagedSlidesRaw_/
    // onAirSlideRaw_ below had been reassigned to match — trading the old
    // race (new raw blocks, stale page key) for its mirror (new page key,
    // stale raw blocks). Settling the VALUE now, with the EMIT held until
    // the raw data is also final (see emit readingPageChanged() below), is
    // what actually makes every consumer's first look see a page and its
    // blocks that agree (reported: "the template expanding to its normal
    // defined size and then contracting back" / "1-2 to 1-3 it flicked",
    // recurring on every later page turn once the memo had a real OLD/NEW
    // page pair to be wrong about — not on the very first commit, which had
    // no prior page to be stale against).
    // EMITTED LAST, after stagedChanged/onAirChanged below — not alongside
    // them. A reader gating on "does the flow text start with THIS block's
    // own text" (DesignPreview's el.drawsFlow — its guard against drawing a
    // flow left over from another slide) reacts to THIS signal, and if it
    // fires before root.blocks has picked up the NEW page (stagedChanged/
    // onAirChanged), it re-checks the prefix with the OLD block still in
    // hand: the match fails for exactly one frame, falls back to the plain
    // page render, and then the flow resumes a frame later once blocks
    // catches up — a one-frame flash right at the page boundary (reported:
    // "the scroller['s] presenting like it[']s at the end[,] then it[']s
    // now blink[ing] a new one"). Landing blocks first means every reader
    // of readingFlowText always sees them already agree.

    if (live_) {
        // ON AIR a step inside one page needs NOTHING committed: the slide, its
        // text and its passage are already the engine's, and the offset goes to
        // the live loop by itself (SetLineScroll) — so the outputs re-compose the
        // frame they already have at the new position instead of the loop being
        // handed a fresh presentation on every key press.
        if (!commitPage) {
            if (!readingStepGlide_ && (flowChanged || scrollChanged))
                emit readingScrollChanged();   // no block change this time: safe as-is
            return;
        }
        // (readingPageAnnounced_ was already settled above; the announcing
        // emit itself waits until the raw data below is also final.)
        QVariantMap shown = lineSetRaw_.at(lineSlideIndex_).toMap();
        // The two fields the engine scrolls by (Slide::flowText / Slide::flowChar).
        shown[QStringLiteral("flowText")] = readingFlowText_;
        shown[QStringLiteral("flowChar")] = readingFlowCharForScroll(readingScroll_);
        goLiveWithSlides(lineSetName_, QVariantList{shown});
        // The pick is ON AIR, so nothing is staged any more — the tiles read the
        // new on-air slide (same contract as stageSlidesWithPreview's live path).
        stagedName_.clear();
        stagedSlidesRaw_.clear();
        stagedSlide_ = QVariantMap{};
        emit stagedChanged();
        // Held until here — readingPageAnnounced_ settled above, but the raw
        // data a reader's own re-evaluation would compose against (onAirSlideRaw_,
        // set inside goLiveWithSlides above) only became final just now, so this
        // is the earliest point both halves of the memo key agree.
        if (pageChanged)
            emit readingPageChanged();
        if (!readingStepGlide_ && (flowChanged || scrollChanged))
            emit readingScrollChanged();
        return;
    }
    // OFF AIR the staged copy has to carry them: it is what GO LIVE will send.
    QVariantMap shown = lineSetRaw_.at(lineSlideIndex_).toMap();
    shown[QStringLiteral("flowText")] = readingFlowText_;
    shown[QStringLiteral("flowChar")] = lineSourceTextOnly_
        ? readingFlowCharForScroll(readingScroll_) : readingCharForScroll(readingScroll_);
    stagedName_ = lineSetName_;
    stagedSlidesRaw_ = QVariantList{shown};
    stagedSlide_ = previewWithActiveStyle(shown);
    emit stagedChanged();
    // Same reasoning as the live branch above: stagedSlidesRaw_ is only final
    // as of the assignment just above, so readingPageChanged waits for here.
    if (pageChanged)
        emit readingPageChanged();
    if (!readingStepGlide_ && (flowChanged || scrollChanged))
        emit readingScrollChanged();
}

bool LiveOutputService::lineStep(int direction)
{
    if (direction != 0)
        emit lineStepPending(direction);
    // ONE-SHOT DIAGNOSTIC, one line per press: which set/slide the window is
    // in, how many lines the slide is seen to have, and what the step decided.
    // "Alt+Down did nothing" and "Alt+Down jumped a whole slide" look the same
    // from the outside and have different causes (an empty set vs a slide whose
    // text is ONE authored line that wraps on screen) — this says which.
    // Logged at INFO through the engine's own log, NOT as a qWarning: a warning
    // shows up as an application notification, and a toast per key press is its
    // own defect. The numbers are the ones a "did nothing" or "jumped a slide"
    // report turns on: how many slides the pick holds, where the window is, and
    // how many lines THAT slide has on screen.
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("LineStep"),
                        QStringLiteral("dir=%1 set=%2 slide=%3 offset=%4 lines=%5")
                            .arg(direction)
                            .arg(lineSetRaw_.size())
                            .arg(lineSlideIndex_)
                            .arg(lineOffset_)
                            .arg(lineSetRaw_.isEmpty()
                                     ? 0
                                     : readingPageLineCount(lineSlideIndex_)));

    if (lineSetRaw_.isEmpty() || direction == 0)
        return false;

    // The JSON-backed passage is only walked while Scroll is on; with it off, a line
    // step walks the slides' own lines as before.
    if (scrollMode_)
        activatePendingTextSource();

    // A REBASE OWED BY AN EARLIER PRESS is settled now, synchronously, before
    // anything below reads lineSlideIndex_/readingScroll_: in the ordinary
    // case (the previous step's animation already finished on its own) this
    // is a no-op, since its own finished handler already committed it. A
    // rapid second press that INTERRUPTED that animation is the case this
    // exists for — see commitPendingRebase's own comment — and this press
    // must continue from the picture as it will actually be, not as the
    // abandoned animation left it.
    commitPendingRebase();

    const int setSize = static_cast<int>(lineSetRaw_.size());
    // THE PASSAGE'S COUNT, not the page's own — see readingPageLineCount: the step
    // moves an index into the passage's table, so a page is as many lines long as the
    // passage makes it.
    const int ownLines = readingPageLineCount(lineSlideIndex_);
    if (lineSourceTextOnly_ && readingBudgetFitActive_ && ownLines <= 0) {
        pendingReadingSteps_ = qBound(-24, pendingReadingSteps_ + direction, 24);
        return true;
    }
    // Where the picture already is: every move starts here, so a press in the
    // middle of a glide continues from the words' real position instead of
    // snapping back to the previous stop first.
    //
    // TWO NUMBERS, because a branch below can move the run's WINDOW out from
    // under the offset. `windowFrom` is what the walk and the engine speak
    // (LineWalk's own `absolute` is documented as "the visible line within the RUN
    // being drawn", and that is exactly readingScroll_); `absoluteFrom` is the
    // same line in PICK terms, which is the one that still means something after
    // the window has slid. Reading the window number as if it were the pick one
    // is not harmless: the walk's own monotonic guard then compares two different
    // scales and the step can land a whole page away (measured — the skip trace in
    // tools/selftest/run_line_step_test.sh reported a 3-line jump in one press).
    const qreal absoluteFrom = readingAbsoluteLine();
    const qreal windowFrom = readingScroll_;
    // THE ARITHMETIC IS LineWalk'S, not this function's. One press moves ONE
    // visible line; a page boundary is +1 rather than a re-base to 0; the end of
    // the pick does not move at all. All of that is unit-tested now
    // (tests_presentation: TestLineWalkMonotonic / Boundaries / RunShift / Edges)
    // — every stepper regression this service has had lived in exactly this
    // branching, and none of them could fail in a test while it did.
    pl::LineWalk walk;
    walk.begin(lineSlideIndex_, lineOffset_, static_cast<int>(windowFrom));
    const int previousLines = (direction < 0 && lineSlideIndex_ > 0)
        ? readingPageLineCount(lineSlideIndex_ - 1) : 0;
    const pl::LineWalk::Step step = walk.step(direction, ownLines, setSize, previousLines);
    if (!step.moved)
        return false;   // the pick's first/last line: nothing above/below it
    if (!readingBudgetFitActive_) {
        readingBudgetFitActive_ = true;
        emit readingStageChanged();
    }

    if (!step.crossedPage) {
        // ANOTHER LINE OF THIS PAGE LEAVES AT THE TOP, and the line under it
        // arrives from the passage — one offset, no picture swap. The committed
        // page carries the offset it will REST at (not the one it is leaving): if
        // the engine rebuilds the scene while the move is still travelling, its
        // own copy of the number is already the one the move is heading for. In
        // RUN numbering, so it keeps climbing across page turns.
        lineOffset_ = step.offset;
        readingScroll_ = step.absolute;
        readingStepGlide_ = true;      // the glide below announces the position
        applyReadingRestage(false);
        readingStepGlide_ = false;
        animateReadingScroll(readingWindowOffset(absoluteFrom), readingScroll_);
    } else if (direction > 0) {
        // THE PAGE BOUNDARY. The move that takes this page's last line out of the
        // box IS the next page arriving at the top, and the run already holds that
        // page — so the move plays on it and the PAGE is committed as the move
        // lands (readingScrollAnim_'s finished handler): the reference, heading and
        // slide number change around words that are already where they will stay.
        lineOffset_ = ownLines;
        readingRebaseSlide_ = step.page;
        animateReadingScroll(readingWindowOffset(absoluteFrom), step.absolute);
    } else {
        // BACK INTO THE PREVIOUS PAGE. Committed FIRST — its last line at the top
        // of the box is exactly what is on screen — and the offset lands on that
        // line in the run's numbering, which is one line above where the box
        // already is: one line up, glided, instead of a page load then a jump.
        if (readingScrollAnim_)
            readingScrollAnim_->stop();
        shiftPickBase(lineSlideIndex_, step.page);
        lineSlideIndex_ = step.page;
        lineOffset_ = step.offset;
        readingStepGlide_ = true;      // the glide below announces the position
        applyReadingRestage();
        readingStepGlide_ = false;
        animateReadingScroll(readingWindowOffset(absoluteFrom), readingScroll_);
    }

    // THE PULSE, after the offset is on its way: a surface that only needs to
    // know "it moved, and which way" reads these two; a surface that DRAWS the
    // move reads `readingScroll` (see DesignPreview) — and the engine's outputs
    // are already being handed the same number frame by frame.
    lineStepDirection_ = direction > 0 ? 1 : -1;
    ++lineStepSerial_;
    emit lineStepChanged();
    return true;
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
    // The media layer travels in the snapshot too, so the undo gives the file back
    // the way it gives a camera back.
    if (mediaOnAir_) {
        snap.mediaPath = mediaPath_;
        snap.mediaName = mediaName_;
    }
    snap.overlays = activeOverlays_;
    snap.valid = !snap.contentSlidesRaw.isEmpty() || !snap.inputLabel.isEmpty()
                 || !snap.mediaPath.isEmpty() || !snap.overlays.isEmpty();
    cleared_ = snap;
    emit clearedChanged();

    // The same clears the button ran itself, in the same order and with the same
    // gates — PLUS the media file, which is a compositor layer exactly like a taken
    // camera/screen and used to survive Clear all (reported: "when press clear all or
    // the media icon it doesn't clear the media"). Note what is NOT here: live_.
    if (!inputLabel_.isEmpty())
        clearInput();
    if (mediaOnAir_)
        clearMedia();
    if (!activeOverlays_.isEmpty())
        clearAllOverlays();
    // The on-air slide only means something while live (off air it already
    // sits in the preview slot, cleared as content above). It is re-sent as the
    // style's own blank carrier, so the branded backdrop survives the clear
    // (see clearOnAirSlide).
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
    // The media layer back where it was. Only one of the two can be non-empty — a
    // take of either one releases the other (see takeInput/applyMediaTake) — so this
    // cannot fight the input above for the single compositor layer.
    else if (!snap.mediaPath.isEmpty())
        takeMedia(snap.mediaPath, snap.mediaName);
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
        // THE MONITOR'S RATE, not 15 Hz — see the note on the first creation
        // site (takeInput) for why 66 ms was wrong.
        inputPump_->setTimerType(Qt::PreciseTimer);
        inputPump_->setInterval(16);
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
// Defined below (the bus windows' frame conversion — see rgbaFrameOf).
static pl::CompositorState::MediaFrame rgbaFrameOf(const QImage &frame);

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
    bool cardMoved = false;
    // Per-label production (card previews' thumbnails light off this —
    // inputLive only tracks the OUTPUT take). Freshly-taken labels enter
    // producing_ on their first frame; released labels are pruned in
    // clearInput/endPreviewInput.
    //
    // A card is read through the JPEG decode-once cache (previewFrameFor), not
    // through the pixel pull: that cache is EXACTLY what the card's own Image
    // displays, so its identity is the honest "did this card's picture change?"
    // answer. The same comparison is what bumps inputRev for a card-only hold —
    // the card Images use that counter as their URL nonce, and the rev bump used
    // to be unconditional, so dropping it here would freeze every thumbnail.
    for (auto it = cardPreviews_.cbegin(); it != cardPreviews_.cend(); ++it) {
        const QString &held = it.key();
        const QImage frame = EngineBridge::instance().previewFrameFor(held);
        if (frame.isNull() || frame.width() <= 1)
            continue;
        if (!producing_.contains(held))
            producing_.insert(held);
        const qint64 key = frame.cacheKey();
        if (cardFrameKeys_.value(held, 0) != key) {
            cardFrameKeys_.insert(held, key);
            cardMoved = true;
        }
    }
    if (!inputLabel_.isEmpty() && !producing_.contains(inputLabel_)
        && !takenFrame.isNull() && takenFrame.width() > 1)
        producing_.insert(inputLabel_);
    // ---- only MOVED frames cost anything ------------------------------------
    // The pump used to run at 15 Hz and push on every tick. A 30/60 fps camera
    // therefore reached the compositor — and so BOTH outputs, HDMI and NDI — at
    // 15 fps: the reported "the video bus is lagging on hdmi and ndi". It now
    // runs at the monitor's own rate (see the timer's interval below), so the
    // rate cap is gone; what makes that cheap is this pair of checks: a frame is
    // pushed only when its cacheKey moved, so a stationary source costs a cache
    // lookup, and the rev bump (every tile/card re-fetching) fires only when
    // something actually changed instead of 60x/s.
    bool moved = false;
    // A card that produced its FIRST frame has an answer to give (its slash/
    // waiting state clears), even when no picture body moved this tick.
    const int producedBefore = producing_.size();
    // NDI taken as the OUTPUT input feeds the engine's compositor layer
    // (the same one the media decoder fills) with the frame pulled above.
    if (!takenFrame.isNull()) {
        const qint64 key = takenFrame.cacheKey();
        if (key != inputFrameKey_) {
            inputFrameKey_ = key;
            pushNdiInputFrame(takenFrame);
            moved = true;
        }
    }
    // BUS WINDOWS: every frame the on-air composition's buses are watching goes
    // to the compositor, which paints each one at its block's rect (SceneBuilder's
    // bus pass) — so NDI and HDMI show what the canvas shows. Pulled through the
    // SAME kind switch the taken input uses: a camera/screen bus reads the PAL
    // tap, an NDI bus the engine receiver, and neither path knows about the other.
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
    if (cardMoved || producing_.size() != producedBefore)
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

// The newest frame of a held tap, kind-aware: camera/screen pulls the tap's
// PIXELS (previewPixelsFor — native size, the device's own rate, no JPEG decode
// on this thread), NDI the engine receiver's convert-on-demand. The pump's
// first-frame detection reads through this one switch, so a kind can never
// watch the wrong tap family.
//
// The pixel pull is what the outputs show. previewFrameFor would have served the
// same tap through its JPEG — a pane contract, published at ~15 fps and decoded
// here — which is the ceiling the bus used to inherit on BOTH outputs.
QImage LiveOutputService::inputFrame(const QString &label, const QString &kind) const
{
    if (kind == QLatin1String("ndi"))
        return EngineBridge::instance().previewNdiFrameFor(label);
    return EngineBridge::instance().previewPixelsFor(label);
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
        // THE MONITOR'S RATE, not 15 Hz — see the note on the first creation
        // site (takeInput) for why 66 ms was wrong.
        inputPump_->setTimerType(Qt::PreciseTimer);
        inputPump_->setInterval(16);
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
    cardFrameKeys_.remove(label);   // the next hold starts from "no picture yet"
    emit inputChanged();
}

// ---------------------------------------------------------------------------
// Bus video windows — a "bus" block on the on-air slide (see the header)
// ---------------------------------------------------------------------------
// A QImage as CompositorState's tightly packed RGBA8. Row-by-row because
// QImage's bytesPerLine() pads past width*4, and CompositorState's readers
// assume no padding (the same copy pushMediaFrameToCompositor makes).
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
    // Resolve ONE bus into the feed the pump pulls: its FIRST routed video
    // source — the very choice BusMonitor and the Media pane's bus card make, so
    // a window (or an output) can never show a feed the operator's own bus card
    // calls something else. false = nothing to take (a removed bus, no route in,
    // an unnamed source): the caller leaves it out and the next tick retries.
    // One ROSTER ROW resolved into a tap: its caption, kind, capture mode and —
    // for the log — its resolved PAL id / NDI name (the frames themselves travel
    // through the tap, not through this). false = nothing to take (a removed
    // row, an unnamed source).
    const auto feedForSourceRow = [](int sourceRow, BusFeed *feed) -> bool {
        if (sourceRow < 0)
            return false;
        const QVariantMap source = VideoSourceListModel::Instance()->getSource(sourceRow);
        const QString label = source.value(QStringLiteral("sublabel")).toString().trimmed();
        if (label.isEmpty())
            return false;
        feed->label = label;
        feed->kind = source.value(QStringLiteral("kind")).toString();
        feed->mode = source.value(QStringLiteral("mode")).toString();
        feed->deviceId = EngineBridge::instance().resolvedPreviewDeviceId(label);
        return true;
    };
    // A BUS resolved into the feed its FIRST routed video source holds — the very
    // choice BusMonitor and the Media pane's bus card make, so a window (or an
    // output) can never show a feed the operator's own bus card calls something
    // else.
    const auto feedForBus = [&feedForSourceRow](int busId, BusFeed *feed) -> bool {
        const int row = BusListModel::rowForBusNumber(busId);
        if (row < 0)
            return false;
        const QVariantMap bus = BusListModel::Instance()->getBus(row);
        // A TEMPLATE BUS HAS NO PICTURE OF ITS OWN: its design is the picture,
        // and its feeds are per WINDOW (see addTemplateWindows). Taking the bus
        // number's own plane here would claim the same device a second time for
        // a picture nothing reads.
        if (bus.value(QStringLiteral("type")).toString() == QLatin1String("template"))
            return false;
        const QVariantList routes = bus.value(QStringLiteral("routedVideoSources")).toList();
        if (routes.isEmpty())
            return false;   // nothing routed in: no device is opened
        return feedForSourceRow(routes.first().toInt(), feed);
    };

    // WHAT THE ON-AIR COMPOSITION ASKS FOR. onAirSlide_ is the STYLED
    // composition every output renders (slideToVariantMap), so its blocks are
    // the ones SceneBuilder's own bus pass walks: a bus window the outputs will
    // paint is exactly a bus block listed here, and one the style's template
    // replaced is not (and must not open a device either).
    QHash<int, BusFeed> wanted;
    // Every bus this composition or its wires NAME (template buses included, and
    // they are the reason this list exists: a template bus holds no plane of its
    // own, so it never appears in `wanted` — its window planes are added below).
    QList<int> namedBuses;
    const QVariantList blocks = onAirSlide_.value(QStringLiteral("blocks")).toList();
    for (const QVariant &v : blocks) {
        const QVariantMap block = v.toMap();
        if (block.value(QStringLiteral("kind")).toString() != QLatin1String("bus"))
            continue;
        const QVariantMap meta = block.value(QStringLiteral("meta")).toMap();
        const int busId = meta.value(QStringLiteral("busId")).toInt();
        if (busId > 0 && !namedBuses.contains(busId))
            namedBuses.append(busId);
        if (busId <= 0 || wanted.contains(busId))
            continue;
        BusFeed feed;
        if (feedForBus(busId, &feed))
            wanted.insert(busId, feed);
    }

    // ...AND THE ROUTING WIRES. A bus wired into an OUTPUT is watched for the
    // same reason a design block's is: the compositor's per-bus frame is what
    // that output's own pass composes (SceneBuilder::BuildBusFeedScene). Without
    // this, a wire into a screen whose slide carries no bus block would draw its
    // wire and then show nothing — nothing ever pulled the device, which is
    // exactly the reported "the wire is drawn but the screen is black".
    if (const auto *outputs = OutputListModel::instance()) {
        for (int i = 0; i < outputs->rowCount(); ++i) {
            const int busId = outputs->routedVideoBus(i);
            if (busId > 0 && !namedBuses.contains(busId))
                namedBuses.append(busId);
            if (busId <= 0 || wanted.contains(busId))
                continue;
            BusFeed feed;
            if (feedForBus(busId, &feed))
                wanted.insert(busId, feed);
        }
    }

    // ...AND A TEMPLATE BUS's WINDOWS (Settings · Routing's bus picker →
    // Template). A window that names a bus shows THAT bus; a window that names
    // none shows the template bus's own routed video sources in route order, so
    // each of those routes gets a plane of its own — one compositor slot per
    // window (CompositorState::TemplateWindowPlaneKey), refreshed by this same
    // pump and read by the engine's window pass exactly like a bus plane.
    const auto addTemplateWindows = [&feedForSourceRow, &wanted](int busId) {
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
            if (feedForSourceRow(routes.at(i).toInt(), &feed))
                wanted.insert(planeKey, feed);
        }
    };
    // (Walk the NAMED buses, not `wanted`'s keys: a template bus contributes no
    // key to `wanted` — only its windows do, and addTemplateWindows is what adds
    // them.)
    for (int busId : std::as_const(namedBuses))
        addTemplateWindows(busId);

    // RELEASE FIRST: a bus whose feed changed (re-route, source swapped, bus
    // renamed to another device) must let its old tap go before the new one is
    // claimed, or the previous device stays open for the rest of the session.
    const QList<int> held = busFeeds_.keys();
    for (int busId : held) {
        const BusFeed &current = busFeeds_[busId];
        const auto want = wanted.constFind(busId);
        if (want == wanted.constEnd() || want->label != current.label
            || want->kind != current.kind)
            releaseBusFeed(busId);
    }

    // ...then claim what is new. A refusal (an unavailable device, a closed
    // window, an NDI runtime that is not there) unwinds cleanly — the same
    // honest result the Media pane's cards follow: no half-held feed, and no
    // placeholder that can never fill. The failed label is retried on a CLOCK
    // (kBusTapRetryMs), not on every poll tick: this runs 10x/s, and a device
    // that is gone must not write a line (or touch a roster probe) 10x/s.
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
        bool accepted = true;
        if (feed.kind == QLatin1String("screen"))
            accepted = EngineBridge::instance().startScreenPreview(feed.label, QStringLiteral("bus"));
        else if (feed.kind == QLatin1String("ndi"))
            accepted = EngineBridge::instance().startNdiPreview(feed.label, QStringLiteral("bus"));
        else
            accepted = EngineBridge::instance().startVideoPreview(feed.label, feed.mode,
                                                                 QStringLiteral("bus"));
        if (!accepted) {
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

    publishBusFeeds();

    if (busFeeds_.isEmpty())
        return;   // the pump stops itself on its next tick (see pumpTick)
    if (!inputPump_) {
        inputPump_ = new QTimer(this);
        // THE MONITOR'S RATE, not 15 Hz — see the note on the first creation
        // site (takeInput) for why 66 ms was wrong.
        inputPump_->setTimerType(Qt::PreciseTimer);
        inputPump_->setInterval(16);
        connect(inputPump_, &QTimer::timeout, this, &LiveOutputService::pumpTick);
    }
    if (!inputPump_->isActive())
        inputPump_->start();
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
    if (feed.kind == QLatin1String("screen"))
        EngineBridge::instance().stopScreenPreview(feed.label, QStringLiteral("bus"));
    else if (feed.kind == QLatin1String("ndi"))
        EngineBridge::instance().stopNdiPreview(feed.label, QStringLiteral("bus"));
    else
        EngineBridge::instance().stopVideoPreview(feed.label, QStringLiteral("bus"));
    // Blank the plane, do not freeze it: the bus is no longer part of this
    // composition, and a last picture left behind would paint into the next
    // slide's hole.
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
// The MEDIA half of the env self-test (see runEnvSelfTest): the take path is
// split across the thread boundary now — takeMedia() reads the file,
// classifies it and decodes a still on the engine worker, then applies the
// result on the interface thread a beat later (see the hop in takeMedia). Four
// probes, each reading only what QML reads (the published properties), so the
// contract is verifiable from a launch log alone:
//
//   1. a real still goes on air, with a NAME and a decoded FRAME (mediaRev
//      moved) — proof the hop landed, the classifier matched and the decode
//      reached the compositor/provider;
//   2. a path that does not exist is REFUSED and leaves what is on air alone
//      (the same refusal the synchronous version gave, now a beat later);
//   3. take-then-clear: a take still in flight must NOT come back on air
//      (the generation token, cleared by clearMedia);
//   4. two takes back to back: the NEWEST wins, whichever decode finishes
//      first — and never a mixture of the two.
//
// A PASS goes through the engine LOGGER (EngineBridge::write), not qInfo:
// main.cpp's Qt message handler forwards warning and above only, so a qInfo
// verdict never reaches build/logs/engine.log — the file a launch-log
// self-test is read from. A FAIL stays a qWarning: loud, and toasted.
static void selftestPass(const char *source, const QString &text)
{
    EngineBridge::write(QStringLiteral("info"), QString::fromLatin1(source),
                        QStringLiteral("PASS: ") + text);
}

// The media probes run ONCE, whichever trigger comes first: the END of the input
// chain (so the two phases never overlap — a media take and an input take are
// mutually exclusive holds on the one compositor layer, last gesture wins, so a
// media probe running underneath a takeInput would be cancelled by it and read
// as a failure of the WRONG branch) or a late fallback, because that chain bails
// out early on a machine with nothing to capture and the media verdicts must not
// go down with it.
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
    // Real stills off this machine (Windows ships them everywhere): largest
    // first, up to two, so probe 4 has a genuinely different second file.
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
                //    first one's decode may land after it.
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
}

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
    // The MEDIA probes ride the same env var: the input chain hands over to them
    // when it finishes, and this is the fallback for the machine where it gives
    // up early ("no window or monitor source to take") — the media verdicts must
    // not go down with it. Both triggers go through the one-shot gate, so the two
    // chains never run at the same time (see runMediaTakeSelfTestOnce).
    QTimer::singleShot(25000, &instance(), [] { runMediaTakeSelfTestOnce(); });
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
            // NO WINDOW TO TAKE is not the same as nothing to test: a DESKTOP
            // capture rides the very same tap (the label resolves to a monitor id
            // through the same table, and the same take, warm-up, re-activation
            // and clear chain drives it) — and a bare desktop is exactly what a
            // fresh machine or CI box has. Falling
            // back to the first monitor keeps this chain COVERED there instead
            // of ending in a FAIL that tests nothing.
            for (const QVariant &v : screen) {
                const QVariantMap d = v.toMap();
                if (d.value("id").toString().startsWith(QStringLiteral("\\\\.\\DISPLAY"))) {
                    label = d.value("label").toString();
                    break;
                }
            }
            if (!label.isEmpty())
                qInfo("LiveOutputService[selftest] no window source — taking monitor '%s' instead",
                      qUtf8Printable(label));
        }
        if (label.isEmpty()) {
            qWarning("LiveOutputService[selftest] FAIL: no window or monitor source to take");
            return;
        }
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("Input selftest"),
                            QStringLiteral("taking '%1'").arg(label));
        instance().takeInput(label, QStringLiteral("screen"), QString());
        if (instance().inputLabel().isEmpty()) {
            qWarning("LiveOutputService[selftest] FAIL: take was refused");
            return;
        }
        QTimer::singleShot(4000, &instance(), [label] {
            // The service's OWN live flag — flipped by the decode-cache poll,
            // i.e. proof the tap produced real pixels end to end.
            if (instance().inputLive())
                selftestPass("Input selftest", QStringLiteral("frames decoded for '%1'").arg(label));
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
                        selftestPass("Input selftest",
                                     QStringLiteral("re-activation kept the tap, frames flowing ('%1')").arg(label));
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
                            selftestPass("Meter selftest",
                                         QStringLiteral("'%1' tap live, %2 channels, peak=%3")
                                             .arg(audioLabel)
                                             .arg(cc)
                                             .arg(peaks.isEmpty() ? 0.0 : peaks.first().toDouble()));
                        } else
                            qWarning("LiveOutputService[selftest] METER FAIL: '%s' produced no snapshot (no signal)",
                                     qUtf8Printable(audioLabel));
                        EngineBridge::instance().stopInputMeter(audioLabel);
                        // The input chain is done — hand the clock to the media
                        // probes (one-shot: the fallback timer above stands down).
                        runMediaTakeSelfTestOnce();
                    });
                });
            });
        });
    });
}

// THE PROGRAM'S PLAYOUT, ON AIR AND OFF. Nothing here touches an output's own
// endpoint: those streams belong to the routing (AudioEngine::StartOutputAudio),
// and AudioEngine::Stop was changed to leave them playing — which is the whole
// point of wiring STOP to it. A monitor with its own audio device keeps sounding
// while the program is off the air; the booth speakers, which ARE the program
// stream, go quiet with it.
void LiveOutputService::setProgramPlayout(bool onAir)
{
    vgr::EngineWorker::instance().runOffThread([onAir] {
        if (!bps::platform::PlatformAccessor::Installed())
            return;   // platform torn down (shutdown): nothing left to open or close
        auto &engine = bps::audio::AudioEngine::Instance();
        // Start() is idempotent while the program is already on the air (and
        // reuses the control thread a stop left alive for an output endpoint),
        // so GO LIVE twice cannot open a second stream.
        auto r = onAir ? engine.Start() : engine.Stop();
        if (!r.ok())
            qWarning("LiveOutputService: program playout %s refused: %s",
                     onAir ? "start" : "stop", r.error().message.c_str());
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
    // THE PROGRAM COMES OFF THE AIR, SOUND INCLUDED. STOP is the program's own
    // on/off switch, so the program's playout stops with the picture — and an
    // output that plays on its own endpoint keeps going (see setProgramPlayout).
    // Unconditional rather than `if (wasLive)`: playout starts with the engine at
    // boot, so a STOP pressed before the first GO LIVE has one to stop too.
    setProgramPlayout(false);
    bps::live::LiveOutputController::Instance().StopLiveAsync();
    if (live_) {
        live_ = false;
        emit liveChanged();
    }
    // OFF AIR, NO OUTPUT NEEDS A BUS FEED: an output's bus window is the only
    // reason these taps exist, and leaving them open held a camera (and its LED)
    // for the rest of the session after a single GO LIVE/STOP. Unconditional —
    // harmless when nothing was held.
    clearBusFeeds();
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
    // A BLACKOUT IS NOT THE PICK. If the carrier had been folded into the staged
    // slot like any other on-air slide, the next GO LIVE would put the black
    // frame straight back on the screens. What the operator was SHOWING when
    // they blacked out is what STOP should leave on the monitor, so the hidden
    // set is staged instead (styled through the same template path stageSlides
    // uses, so the tile shows what GO LIVE will commit).
    if (!blackoutRestoreSlides_.isEmpty()) {
        stagedName_ = blackoutRestoreTitle_;
        stagedSlidesRaw_ = blackoutRestoreSlides_;
        stagedSlide_ = previewWithActiveStyle(blackoutRestoreSlides_.first().toMap());
        emit stagedChanged();
        blackoutActive_ = false;
        blackoutRestoreTitle_.clear();
        blackoutRestoreSlides_.clear();
    } else if (!onAirSlideRaw_.isEmpty()) {
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
    // THE POLL KEEPS RUNNING OFF AIR. It used to stop here, which is why the
    // monitor preview only ever updated while live: pollTick is the one place
    // that bumps frameRev (the image provider's cache-buster), so with the
    // timer stopped an off-air frame — the idle preview publishing a taken
    // media file — was written into the buffer and never fetched. Off air the
    // tick now does two atomic reads and returns (see its head), so the cost
    // of leaving it armed is a 10 Hz timer, not a render.
    // The NDI feed has its own clock now — off air it stops with it.
    stopNdiSendClock();
    stopNdiFeed();
    // THE FEED PROBE ONLY RUNS LIVE (sampleFeedHealth is called from pollTick,
    // which is past its `if (!live_) return` at that point), so a stopped
    // session would otherwise leave the last measured frame counts sitting in
    // outputRuntime_ forever. Every row must read "off air", not the numbers of
    // a session that has ended.
    if (!outputRuntime_.isEmpty()) {
        outputRuntime_.clear();
        emit outputRuntimeChanged();
    }
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
    // CLEAR ALL KEEPS THE OUTPUT'S OWN LOOK. It takes the CONTENT down — the
    // slide's blocks, a taken camera, the media file, the overlays — and leaves
    // the active style painting: the branded background (colour AND image) stays
    // on the screens, which is what an operator clearing a media take expects to
    // see underneath it ("when I'm showing media and I clear, it clears
    // everything including the bg image from styles, which is wrong").
    //
    // This used to send a `clearStage` carrier that told the engine to paint
    // NOTHING at all — written for the earlier report "the clear brings the
    // background image back and clears the slide". That report was about the
    // SLIDE's content coming back; dropping the blocks fixes it on its own, and
    // the backdrop going black was the part nobody asked for. The engine still
    // understands `clearStage` (a saved show can carry it — see ShowConverter),
    // Clear all just no longer sends it.
    goLiveBlank();
}

void LiveOutputService::blackout()
{
    if (!live_ || blackedOut())
        return;
    // WHAT THE BLACKOUT IS HIDING, saved BEFORE the carrier replaces it in the
    // engine: the session keeps running (live_, the NDI sender and every output
    // are untouched), only the picture changes. The whole raw set travels, not
    // just the slide showing, so coming out of a blackout leaves a passage that
    // can still be stepped.
    blackoutActive_ = true;
    blackoutRestoreTitle_ = onAirTitle_;
    blackoutRestoreSlides_ = onAirSlidesRaw();

    QVariantMap clear;
    clear.insert(QStringLiteral("blocks"), QVariantList{});
    clear.insert(QStringLiteral("background"), QStringLiteral("transparent"));
    clear.insert(QStringLiteral("title"), QString());
    // THE FLAG THAT MAKES THIS A BLACKOUT AND NOT A BLANK. Without it the same
    // empty slide is the blank GO LIVE carrier, which deliberately wears the
    // output's own look (style colour AND background image); with it the engine
    // composes an explicitly empty frame (SceneBuilder's Cleared) — nothing of
    // the style, nothing of the slide — which is what "nothing at all on
    // screen" means on HDMI and on the NDI feed alike.
    clear.insert(QStringLiteral("clearStage"), true);
    goLiveWithSlides(QString(), { clear });
}

void LiveOutputService::endBlackout()
{

    if (!blackedOut())
        return;
    blackoutActive_ = false;
    // Taken out of the slots first: goLiveWithSlides() clears them for any
    // non-carrier content, and these ARE that content.
    const QString title = blackoutRestoreTitle_;
    const QVariantList slides = blackoutRestoreSlides_;
    blackoutRestoreTitle_.clear();
    blackoutRestoreSlides_.clear();
    if (slides.isEmpty()) {
        // Nothing remembered — the carrier came from a saved show, or the
        // session was already blacked out before this control existed. The
        // output's own look is the one answer that is never a lie.
        goLiveBlank();
        return;
    }
    goLiveWithSlides(title, slides);
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
// The DISK half of a take — it exists so that step can run on the worker
// (see the hop in takeMedia). Everything in it touches a file: resolving the
// path, asking whether it exists, classifying its extension and, for a still,
// the decode. A slow/cold volume or a 4K JPEG is a wait the user can feel, and
// that wait belongs to the disk, not to the thread that draws.
LiveOutputService::MediaPreflight LiveOutputService::preflightMediaTake(const QString &path,
                                                                       const QString &name)
{
    MediaPreflight pre;
    pre.clean = path.startsWith(QStringLiteral("file:///"))
        ? QUrl(path).toLocalFile() : path;
    if (pre.clean.isEmpty() || !QFileInfo::exists(pre.clean))
        return pre;   // found = false: the apply half logs the refusal

    static const char *videoExts[] = { "mp4", "mov", "mkv", "avi", "webm", "m4v", "wmv" };
    // THE SAME IMAGE LIST THE LIBRARY CLASSIFIES (MediaLibrary::MediaKindOf), so
    // a file the grid shows as a picture can always be taken. It used to be a
    // short png/jpg/bmp/gif/webp list, which REFUSED an SVG with "unrecognized
    // extension" — a format this build's Qt does decode (qsvg plugin), and the
    // one every logo arrives in ("please add more image types supported svg
    // etc."). A format nothing here can decode still lands as a frameless take
    // with a named reason (see the decode below), not as a refusal.
    static const char *imageExts[] = { "png", "jpg", "jpeg", "jpe", "jfif", "bmp", "dib", "gif",
                                       "webp", "tif", "tiff", "ico", "heic", "heif", "avif",
                                       "svg", "svgz", "jp2", "tga" };
    // Same set MediaLibrary.cpp's own classifier recognizes as audio (see
    // its ExtToKind) — a file indexed there as "audio" must not turn out
    // to be refused here.
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
    // A still is decoded HERE, once, on the worker — it paints through the same
    // provider as a video frame. A failed decode leaves the take up but
    // frameless: image stays null and the apply half keeps the tile's
    // placeholder honest instead of faking a frame.
    if (pre.isImage) {
        // QImageReader, not QImage(path): the reader reports WHY a file failed
        // and lets an SVG be rasterized at the size it will be shown at. An SVG
        // carries no pixels of its own — read at its intrinsic size (often 512)
        // it reaches a 1080 stage as a blown-up bitmap.
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
        // REAL TRANSPARENCY, scanned once, here, on the worker — the one place
        // the decoded pixels are already in hand (see imageHasTransparentPixel
        // and CompositorState::MediaFrame::hasAlpha).
        pre.hasAlpha = imageHasTransparentPixel(pre.image);
        // ONE-SHOT DECODE PROBE. "It does not render all images correctly" has
        // exactly two halves — the file arrived wrong, or it arrived right and
        // the composite lost it — and only the decoded pixels can tell them
        // apart. Dimensions + four corner colours + the centre say whether this
        // is the file the card was showing.
        if (!pre.image.isNull()) {
            const QImage probe = pre.image.convertToFormat(QImage::Format_RGB32);
            const int cx = probe.width() / 2;
            const int cy = probe.height() / 2;
            qInfo("media take image: '%s' %dx%d corners tl=%08x br=%08x center=%08x alpha=%s",
                  qUtf8Printable(pre.clean), probe.width(), probe.height(),
                  probe.pixel(0, 0), probe.pixel(probe.width() - 1, probe.height() - 1),
                  probe.pixel(cx, cy), pre.hasAlpha ? "yes" : "no");
        } else {
            // NAME THE FORMAT, not just the file: "failed to decode" for a .webp
            // is this build's Qt having no WebP image plugin, and the operator
            // can only act on that if the log says so. The supported list is the
            // one that actually answers for THIS machine, plugins included.
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

void LiveOutputService::takeMedia(const QString &path, const QString &name)
{
    // SUPERSESSION TOKEN. A take is now two hops (disk on the worker, apply
    // back here), so a second drop can overtake the first one's decode. The
    // newest take owns the layer: every older generation is dropped on sight,
    // and the apply half re-checks on THIS thread before writing a member, so
    // two results can never interleave into a mixed state.
    const quint64 token = ++mediaTakeToken_;
    // The hop: QFileInfo::exists + the extension classification + (for a still)
    // the QImage decode — all disk work, and takeMedia is a Q_INVOKABLE
    // reachable straight from a drop handler. What stays HERE is only the
    // bookkeeping: the token bump above and the apply below, because every
    // member an apply writes (mediaPlayer_/mediaSink_/mediaAudio_) is a QObject
    // owned by this thread and QMediaPlayer must be created and driven on the
    // thread that runs its event loop.
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

// The interface-thread half: the same body the take used to run inline, minus
// everything that touched a file. It never blocks — the file is already read.
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
    const bool isVideo = pre.isVideo;   // pre.isImage is the else branch below
    const bool isAudio = pre.isAudio;

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
            if (!lastMediaFrame_.isNull()) {
                // THE FIRST FRAME DECIDES THIS TAKE'S OWN TRANSPARENCY — a video
                // that really carries alpha (a keyed WebM/MOV, an alpha-capable
                // capture) is a transparent picture on air, and the checkerboard
                // convention must read it as one. Scanned ONCE: a per-frame walk
                // of 1080p RGBA would cost more than the render it feeds.
                if (!mediaAlphaDecided_) {
                    mediaAlphaDecided_ = true;
                    mediaHasAlpha_ = imageHasTransparentPixel(lastMediaFrame_);
                    g_mediaHasAlphaOnAir = mediaHasAlpha_;
                    emit mediaChanged();
                }
                mediaRev_++;
            }
            pushMediaFrameToCompositor(lastMediaFrame_);
            emit mediaTick();
        });
    }

    mediaPath_ = clean;
    mediaName_ = pre.name;
    // Prefer this file's saved override; otherwise use the shared default from
    // Settings · Outputs. The effective fit is handed to every output so all
    // monitors frame the same media consistently.
    const QString fileFit = MediaLibraryService::instance().itemFit(clean);
    mediaFitUsesDefault_ = fileFit.isEmpty();
    mediaFit_ = mediaFitUsesDefault_ ? defaultMediaFit_ : fileFit;
    g_mediaFitOnAir = mediaFit_.toStdString();
    mediaIsVideo_ = isVideo;
    mediaIsAudio_ = isAudio;
    // A STILL's real transparency, decided by its pixels (see preflightMediaTake).
    // A video or an audio file is never a transparent frame: a decoded video
    // frame is opaque, and the letterbox margins a contain-fit leaves are the
    // OUTPUT's empty pixels, not the picture's alpha — reading them as
    // transparency is what put checkers around a taken 9:16 clip.
    mediaHasAlpha_ = !isVideo && !isAudio && pre.hasAlpha;
    g_mediaHasAlphaOnAir = mediaHasAlpha_;
    // A video's own alpha is decided by its first decoded frame (see the sink
    // handler above); a still's was just scanned on the worker.
    mediaAlphaDecided_ = !isVideo;
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
        // An IMAGE paints through the same provider. The decode already
        // happened on the worker (preflightMediaTake); this is just the hand
        // over. A failed decode leaves the take up but frameless — the tile's
        // placeholder stays honest.
        lastMediaFrame_ = pre.image;
        if (!lastMediaFrame_.isNull())
            mediaRev_++;
        pushMediaFrameToCompositor(lastMediaFrame_);
        mediaState_ = QStringLiteral("playing");   // a still "plays" forever
        mediaPosition_ = 0;
        mediaDuration_ = 0;
    }
    // THE MONITOR METERS FOLLOW A TAKE'S AUDIO. The file's sound is played by
    // Qt's own QAudioOutput, which no PAL tap can see — so the take also holds a
    // STREAM meter on the same file (EngineBridge::startStreamMeter, the tap a
    // video/NDI row already uses for its own audio) under one reserved label the
    // monitor tiles read. Without it, playing an audio file left every output
    // tile's L/R strip flat: the bars only ever read capture-device taps, and a
    // FILE is not a device. Released with the take (clearMedia), so no decoder
    // outlives the picture.
    // RELEASE FIRST: the meter is refcounted per label, and taking a second file
    // must MOVE the decoder, not add a hold that outlives the take (one hold per
    // take, released by the next one and by clearMedia).
    EngineBridge::instance().stopStreamMeter(kMediaMeterLabel);
    if (isVideo || isAudio)
        EngineBridge::instance().startStreamMeter(kMediaMeterLabel, QStringLiteral("media"), clean);
    qInfo("LiveOutputService: media on air: '%s' (%s)", qUtf8Printable(mediaName_),
          isVideo ? "video" : (isAudio ? "audio" : "image"));
    emit mediaChanged();
    emit mediaTick();
}

void LiveOutputService::setMediaFit(const QString &fit)
{
    // NORMALISED at the edge: the three the renderer knows, anything else means
    // "let the output decide" — the picker's fourth choice.
    const QString wanted = (fit == QLatin1String("contain") || fit == QLatin1String("cover")
                            || fit == QLatin1String("fill"))
        ? fit : QString();
    if (mediaPath_.isEmpty())
        return;   // nothing on air to set it on (the card picker writes the file itself)
    // THE FILE KEEPS IT: stored in the media library, so the choice is there for
    // the next take and for every output — the take is only what is up now.
    MediaLibraryService::instance().setItemFit(mediaPath_, wanted);
    mediaFitUsesDefault_ = wanted.isEmpty();
    const QString effective = mediaFitUsesDefault_ ? defaultMediaFit_ : wanted;
    if (mediaFit_ == effective)
        return;
    mediaFit_ = effective;
    g_mediaFitOnAir = effective.toStdString();
    // The picture is already decoded: only the LAYOUT changes, so hand the new
    // fit to the compositor without re-pushing a frame (the pixels are identical)
    // and let the next preview/live tick lay them out again.
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
    // Invalidate any take that is still decoding (see the token in takeMedia):
    // a file that lands after this must not put itself back on air. Bumped
    // BEFORE the on-air guard below — clearing an already-empty layer still has
    // to cancel a take that is in flight.
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
    lastMediaFrame_ = QImage();
    pl::CompositorState::Instance().ClearMedia();
    // The meter's decoder goes with the take (see applyMediaTake).
    EngineBridge::instance().stopStreamMeter(kMediaMeterLabel);
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
    const bool result = !live_ && !stagedSlidesRaw_.isEmpty()
                       && !title.isEmpty() && stagedName_ == title;
    // ONE-SHOT DIAGNOSTIC: every input this answer is built from, named —
    // liveFlowSlide (ReferencePane.qml) stayed stuck at null even after its
    // own binding-dependency fix, so the question is no longer "does this
    // re-evaluate" but "what does it actually compare" (title vs
    // stagedName_ — a mismatched string is invisible from the QML side,
    // which only ever sees the bool).
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("StagedIsOurs"),
                        QStringLiteral("title='%1' stagedName_='%2' live_=%3 stagedEmpty=%4 result=%5")
                            .arg(title)
                            .arg(stagedName_)
                            .arg(live_)
                            .arg(stagedSlidesRaw_.isEmpty())
                            .arg(result));
    return result;
}

// A tab announcing (or withdrawing) itself as a stepper of the tile's content
// — see the header. Only a TRANSITION of the aggregate is signalled, so the
// wall's arrows are not relit on every tab's every announcement.
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
    // THE OWNER (see wallStepperId): the last tab to claim. When it withdraws,
    // another claiming tab takes over rather than leaving the owner blank — only
    // one tab's content is on the tile, so its own claim is the one that matters,
    // but a withdraw/re-claim pair must not read as "nobody".
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
    // The OWNER change is signalled too: a surface keying per-tab state on it has
    // to re-read when the tile's content moves from one tab to another, which can
    // happen with the aggregate staying lit throughout.
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
// The on-air slide as design blocks (the QML preview feed)
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
    onAirSlide_ = slide ? slideToVariantMap(slide, pl::PresentationEngine::Instance().ActiveOutputStyle())
                        : QVariantMap{};
    onAirSlideRaw_ = slide ? slideToVariantMap(slide, pl::OutputStyleSpec{}) : QVariantMap{};
    // THE SLIDE'S GROUP DECIDES ITS TEMPLATE (a group can name one — Manage
    // groups), and the engine renders from the spec that was PUSHED, not from
    // anything computed here. So the push follows the slide: the group the
    // on-air slide carries is re-pushed into the active style's spec, which is
    // what keeps the real output on the same template the monitor tile composes
    // (previewWithActiveStyle reads the same tag). Only a CHANGE pushes: this
    // runs on every stage and step, and an unchanged spec is a no-op anyway.
    const QString tag = (slide && !slide->tags.empty())
                            ? QString::fromStdString(slide->tags.front()) : QString();
    if (tag != onAirGroupTag_) {
        onAirGroupTag_ = tag;
        if (OutputListModel *outputs = OutputListModel::instance())
            outputs->repushActiveStyleForTag(tag);
    }
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
    // A GROUP WITH A TEMPLATE COMPOSES THROUGH IT, the same rule the outputs get
    // (see the group's own row in Manage groups). Read before composing, and only
    // when there IS one: an ungrouped slide keeps the plain active-spec path
    // byte for byte.
    const QString tag = slide.value(QStringLiteral("tag")).toString();
    if (!tag.isEmpty() && !GroupListModel::instance()->templateForTag(tag).isEmpty()) {
        const QString styleId = OutputListModel::instance()
                                    ? OutputListModel::instance()->activeStyleId() : QString();
        return slideToVariantMap(&engineSlide, outputStyleForPreview(styleId, tag));
    }
    return slideToVariantMap(&engineSlide);
}

QVariantMap LiveOutputService::previewWithStyle(const QVariantMap &slide,
                                                 const QString &styleId) const
{
    const pl::Slide engineSlide = ShowConverter::slideFromVariant(slide);
    return slideToVariantMap(&engineSlide,
                             outputStyleForPreview(styleId,
                                                   slide.value(QStringLiteral("tag")).toString()));
}


// ---------------------------------------------------------------------------
// Poll (onAir titles + frame counter while live)
// ---------------------------------------------------------------------------
// See the header: the two publish points of the live loop, read synchronously
// (atomics only), so a measurement is not quantized to the 10Hz poll.
qulonglong LiveOutputService::previewFramesNow() const
{
    return bps::live::LiveOutputController::Instance().PreviewFrames();
}

qulonglong LiveOutputService::framesSentNow() const
{
    return bps::live::LiveOutputController::Instance().FramesSent();
}

// See the header: armed once, at construction, and never stopped — pollTick is
// what publishes the monitor preview OFF AIR, not only while live.
// ---------------------------------------------------------------------------
// FEED HEALTH — is the program feed still reaching each output?
// ---------------------------------------------------------------------------
// THE HEARTBEAT IS THE CLOCK, NOT THE PICTURE. The live loop deliberately
// skips re-rasterising a scene that has not changed, so "no new frame" on its
// own would call a static verse a stall — which is exactly what it is not. What
// a healthy session DOES do is re-present the composed frame to every output
// once a second (LiveOutputController::kHeartbeatMs), so each output's own
// "last presented" stamp advances ~1/s whatever the slides are doing. The age
// of that stamp is therefore a feed heartbeat: a few seconds with no movement
// is a real stall (a dead render loop, a dropped display buffer, a per-output
// pass that stopped landing) and a verse sitting still is not.
//
// WHAT A STALL DOES: that output is handed an opaque BLACK frame, immediately
// and outside the render loop (which is the thing that may have died), so it
// stops showing a frozen picture that reads as a live feed. Nothing has to be
// undone — the loop's next present overwrites the black the moment frames flow
// again, and the header's pill says so.
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
        liveSinceMs_ = 0;
        lastFeedProbeMs_ = 0;
        const bool hadRuntime = !outputRuntime_.isEmpty();
        if (!stalledOutputs_.isEmpty() || !feedNoticeText_.isEmpty()
            || !stalledLabels_.isEmpty() || hadRuntime) {
            stalledOutputs_.clear();
            stalledLabels_.clear();
            feedNoticeText_.clear();
            feedNoticeKind_.clear();
            outputRuntime_.clear();   // no session: every row is idle, not measured
            ++feedHealthRev_;
            emit feedHealthChanged();
        }
        if (hadRuntime)
            emit outputRuntimeChanged();
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

    // ONE ENGINE READ: the per-output "last presented" age AND delivered-frame
    // count, keyed by buffer name. Names and atomic stamps under the manager's
    // lock — no frame copy, no device.
    const auto ages = bps::rendering::RenderEngine::Instance().OutputFeedAges();
    struct FeedSample { int64_t ageMs = -1; uint64_t frames = 0; };
    QHash<QString, FeedSample> sampleByBuffer;
    sampleByBuffer.reserve(static_cast<int>(ages.size()));
    for (const auto &row : ages)
        sampleByBuffer.insert(QString::fromStdString(row.name), FeedSample{row.ageMs, row.frames});

    // WHICH BUFFER A ROW'S PICTURE LANDS IN, exactly as OutputListModel::
    // FrameBufferRole and the live loop key it: a styled output has its own
    // gated buffer, an unstyled one mirrors the shared preview feed. One
    // helper, because the probe now reads it twice (the runtime map below and
    // the stall pass further down) and two copies would be one copy too many.
    // The RULE is the model's (hasOwnPass): a style OR a video wire (Settings ·
    // Routing) gives the row a pass of its own, and that pass is what draws its
    // picture. Asking the model instead of re-reading styleId here is what keeps
    // a wired output from rendering its bus into one buffer while this probe —
    // and the stall watchdog's to-black list — watched the shared program feed.
    const auto bufferForRow = [model](int row) -> QString {
        if (!model || !model->hasOwnPass(row))
            return QString::fromLatin1(bps::live::LiveOutputController::kPreviewName);
        return QStringLiteral("__out_%1__")
            .arg(qHash(model->getOutput(row).value(QStringLiteral("name")).toString()));
    };

    // ---- every row's own runtime state (what Outputs reads) ---------------
    // Built for EVERY row, enabled or not and whatever its kind: the report
    // behind this was that only the row owning a sender ever reported anything
    // ("the notification is only instantiated in New Screen 2"). HDMI rows
    // have exactly the same facts available — frames delivered and how long
    // since the last one — so they say them too.
    QVariantMap runtime;
    for (int i = 0; i < model->rowCount(); ++i) {
        const QVariantMap row = model->getOutput(i);
        const FeedSample sample = sampleByBuffer.value(bufferForRow(i));
        QVariantMap state;
        state.insert(QStringLiteral("presented"), sample.ageMs >= 0);
        state.insert(QStringLiteral("ageMs"), QVariant::fromValue<qint64>(sample.ageMs));
        state.insert(QStringLiteral("frames"), QVariant::fromValue<qulonglong>(sample.frames));
        state.insert(QStringLiteral("enabled"), row.value(QStringLiteral("isEnabled")).toBool());
        runtime.insert(row.value(QStringLiteral("name")).toString(), state);
    }
    if (runtime != outputRuntime_) {
        outputRuntime_ = runtime;
        emit outputRuntimeChanged();
    }

    // A fresh session's buffers are empty until the first passes land; calling
    // that a stall would black every output for the first second of every
    // service. One grace window, from this session's own go-live. (The runtime
    // above is published BEFORE it: "warming up" is a real state to show.)
    if (now - liveSinceMs_ < kFeedGraceMs)
        return;

    QSet<QString> stalled;
    QStringList labels;
    std::vector<std::string> toBlack;
    for (int i = 0; i < model->rowCount(); ++i) {
        const QVariantMap row = model->getOutput(i);
        // A DISABLED output is not on air, so it cannot be missing a feed: skip
        // it — but keep the NUMBER. M<n> is the n-th row in the roster (the wall's
        // own tile order), not the n-th enabled one, so a roster of three with the
        // middle one switched off still names the last output M3, exactly as the
        // operator counts it on the wall.
        if (!row.value(QStringLiteral("isEnabled")).toBool())
            continue;
        const int monitor = i + 1;
        const QString name = row.value(QStringLiteral("name")).toString();
        // The buffer this output's picture actually lands in (one helper, see
        // bufferForRow above — the runtime map reads the same key).
        const QString buffer = bufferForRow(i);
        const auto age = sampleByBuffer.constFind(buffer);
        if (age == sampleByBuffer.constEnd() || age.value().ageMs < 0)
            continue;   // never presented to yet: warming, not stalled
        if (age.value().ageMs > kFeedStallMs) {
            stalled.insert(name);
            labels << QStringLiteral("M%1").arg(monitor);
            if (!stalledOutputs_.contains(name))
                toBlack.push_back(buffer.toStdString());
        }
    }

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
        if (!toBlack.empty()) {
            // BLACK NOW, OFF THE INTERFACE THREAD. Presenting a full black frame
            // is a lock + an 8 MB fill per output, and the render loop this is
            // covering for may be the very thing that is stuck — so it runs on
            // the engine worker rather than in the operator's click path.
            qInfo("LiveOutputService: feed stalled — %s: no frames for over %lld ms, blacked out",
                  qUtf8Printable(labelLine), static_cast<long long>(kFeedStallMs));
            vgr::EngineWorker::instance().runOffThread([buffers = toBlack]() {
                (void)bps::rendering::RenderEngine::Instance().BlackoutOutputs(buffers);
            });
        }
        // A TOAST AS WELL AS THE PILL. The header pill is the standing state,
        // but it sits in the corner and the operator's eyes are on the stage —
        // the moment a feed dies is worth one interruption, exactly once per
        // stall (this branch is the transition, not the 1 Hz re-check). The
        // crux makes the output names bold, the same way a device failure names
        // its device.
        EventBus::instance().notify(
            QStringLiteral("%1 · No live feed — blacked out until frames return").arg(labelLine),
            QStringLiteral("error"), QStringLiteral("Live output"),
            QStringLiteral("live.feed.stalled"), labels);
        if (feedNoticeTimer_)
            feedNoticeTimer_->stop();   // a fresh stall owns the pill again
        setFeedNotice(QStringLiteral("%1 · No live feed").arg(labelLine),
                      QStringLiteral("stalled"));
        return;
    }

    if (wasStalled) {
        qInfo("LiveOutputService: feed restored — %s: frames flowing again",
              qUtf8Printable(wasLabelLine));
        setFeedNotice(QStringLiteral("%1 · Feed restored").arg(wasLabelLine),
                      QStringLiteral("restored"));
        EventBus::instance().notify(
            QStringLiteral("%1 · Feed restored").arg(wasLabelLine),
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
}

void LiveOutputService::pollTick()
{
    connectToNdiOutputRoster();
    connectReadingBasis();   // once, on the first tick that finds the model
    auto &ctrl = bps::live::LiveOutputController::Instance();

    // THE CACHE-BUSTER RUNS OFF AIR TOO — GO LIVE IS NOT THE GATE TO THE
    // MONITOR PREVIEW. The tiles read the engine's preview buffer through
    // image://livepreview?v=<frameRev>, and ONLY a bump here makes them refetch
    // it. This used to sit below `if (!live_) return`, so an off-air frame
    // (LiveOutputController's idle preview publishes the compositor's picture
    // — a taken media file, a camera over the style) landed in the buffer and
    // nothing ever asked for it: the take appeared on the wall the moment GO
    // LIVE started the live loop and not one tick earlier. The preview is what
    // GO LIVE COPIES FROM, so it has to be current while nothing is on air.
    if (ctrl.PreviewFrames() != previewFrames_) {
        previewFrames_ = ctrl.PreviewFrames();
        ++frameRev_;
        emit frameRevChanged();
    }

    if (!live_)
        return;

    // Keep the output roster/feed plans current even while a slide/content
    // replacement is pending. Returning before this refresh used to strand
    // NDI outputs added during that window until the next live session.
    pollTickRecheckNdi();

    if (ctrl.IsLiveContentReplacementPending())
        return;

    // Is the program feed still reaching every output? (The probe throttles
    // itself to 1 Hz; the poll above stays 10 Hz for the preview.)
    sampleFeedHealth();

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
    // (The preview counter is handled above, live or not; this is the LATE
    // publish — once every per-output pass has also landed.)
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
    // The selected NDI output's feed also goes to the network while the
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

    // THE OUTPUTS' BUS WINDOWS ride the same 10Hz mirror: onAirSlide_ is now
    // this instant's on-air composition, so which buses need a feed (and which
    // one each is watching) is a model read away. Idempotent — with nothing
    // changed it touches no device — and a re-route or a bus rename settles
    // within one tick, which is why no event for either has to be published.
    refreshBusFeeds();
}

// ---------------------------------------------------------------------------
// The NDI program sender — the display engine's NdiDisplayProvider IS the
// output ("ndi-program" device, real NDI SDK via the BroadcastEngine with an
// automatic software-loopback fallback). This reads the selected NDI
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
//
// THREADS (§6.6 of docs/gui_thread_audit.md): the resolution is SPLIT the way
// the tap opens are. What belongs to THIS thread stays — the roster read (a QML
// model), the bridge's PUBLISHED NDI verdict, the buffer NAME (a hash of the
// row), the configured rate, and every piece of the pill's own state. What does
// not is the ENGINE WALK: the display provider and the output's frame buffer are
// engine singletons behind their own locks, and a 10 Hz poll taking them on the
// thread that draws is exactly the inventory this audit exists to remove. The
// walk runs on the pool and its answer comes back through applyNdiPlanProbe.
void LiveOutputService::recheckNdiPlanOffThread()
{
    const auto *model = OutputListModel::instance();
    struct Request { QString name, nodeId, buffer, sender; float fps; bool audioWired = false; std::shared_ptr<bps::display::NdiDisplayProvider> provider; };
    std::vector<Request> requests;
    // An output carries audio only if a bus is actually wired into its node. An
    // idle NDI sender streaming a 48 kHz stereo silence track is real bandwidth
    // (about 380 kB/s per output) for nothing, so the wire itself decides.
    const auto snap = bps::production::ProductionEngine::Instance().Graph().PublishedSnapshot();
    const auto audioWired = [&snap](const std::string &nodeId) {
        if (!snap || nodeId.empty())
            return false;
        for (const auto &[from, tos] : snap->edges)
            if (from.ends_with("#a") && tos.count(nodeId) != 0)
                return true;
        return false;
    };
    if (model) {
        for (int i = 0; i < model->rowCount(); ++i) {
            const QVariantMap row = model->getOutput(i);
            if (row.value("kind").toString() != QLatin1String("NDI") || !row.value("isEnabled").toBool()) continue;
            const QString name = row.value("name").toString();
            // The row's OWN graph node, named exactly as RoutingService names it
            // ("out:<stable id>"): that is the node whose mix this sender carries.
            const QString nodeId = QStringLiteral("out:%1").arg(row.value("id").toString());
            auto provider = ndiProviders_.value(name);
            if (!provider) { provider = std::make_shared<bps::display::NdiDisplayProvider>(); ndiProviders_.insert(name, provider); }
            // The model's OWN-PASS rule (a style, or a Routing video wire): an
            // NDI sender must send what the live loop paints for that row.
            const std::string buffer = model->hasOwnPass(i)
                ? QStringLiteral("__out_%1__").arg(qHash(name)).toStdString()
                : std::string(bps::live::LiveOutputController::kPreviewName);
            const QString rate = row.value("refresh").toString();
            bool ok = false;
            const float rawFps = rate.left(rate.indexOf(QLatin1Char(' '))).toFloat(&ok);
            const float fps = bps::display::AcceptedOutputFrameRate(ok ? rawFps : 0.0f);
            requests.push_back({name, nodeId, QString::fromStdString(buffer), ndiSenderDisplayName(name), fps,
                                audioWired(nodeId.toStdString()), provider});
        }
    }
    QStringList rosterParts;
    for (const auto &request : requests)
        rosterParts.append(QStringLiteral("%1->%2@%3").arg(request.name, request.buffer).arg(request.fps, 0, 'f', 1));
    const QString rosterSignature = rosterParts.join(QStringLiteral(" | "));
    if (rosterSignature != ndiRosterLogSignature_) {
        ndiRosterLogSignature_ = rosterSignature;
        qInfo("LiveOutputService: NDI recheck live=%d model=%p rows=%d enabledNdi=%d [%s]",
              live_ ? 1 : 0, static_cast<const void *>(model), model ? model->rowCount() : -1,
              static_cast<int>(requests.size()), qUtf8Printable(rosterSignature));
    }
    // A mid-session row that is SWITCHED OFF never reaches the probe at all (the
    // request list holds enabled rows only), so it would sit there silently —
    // which is the same report from the operator's side. Name it as the wait it
    // is instead of leaving the row dark with no explanation.
    for (int i = 0; i < model->rowCount(); ++i) {
        const QVariantMap row = model->getOutput(i);
        if (row.value("kind").toString() != QLatin1String("NDI"))
            continue;
        const QString name = row.value("name").toString();
        if (row.value("isEnabled").toBool() || ndiSessionNames_.contains(name)
            || ndiAnnouncedNames_.contains(name))
            continue;
        warnSilentNewOutput(name, QStringLiteral("it is switched off on the Outputs screen"));
    }

    QSet<QString> enabledNames;
    for (const auto &request : requests) enabledNames.insert(request.name);
    for (auto it = ndiProviders_.begin(); it != ndiProviders_.end();) {
        if (!enabledNames.contains(it.key())) {
            const auto retired = it.value();
            it = ndiProviders_.erase(it);
            ensureNdiWorker();
            if (ndiWorker_)
                QMetaObject::invokeMethod(ndiWorker_, [retired] { retired->ResetSender(); }, Qt::QueuedConnection);
        } else {
            ++it;
        }
    }
    bool stateAdded = false;
    for (const auto &request : requests) {
        if (ndiOutputStates_.contains(request.name)) continue;
        ndiOutputStates_.insert(request.name, QVariantMap{
            { QStringLiteral("frames"), QVariant::fromValue<qulonglong>(request.provider->FramesSent()) },
            { QStringLiteral("sending"), false },
            { QStringLiteral("warming"), true },
            // This output's own configured rate — the per-output rate watch
            // compares its measured sends against THIS, not a feed-wide one.
            { QStringLiteral("fps"), request.fps }
        });
        stateAdded = true;
    }
    if (stateAdded) emit ndiChanged();
    const bool usable = EngineBridge::instance().ndiAvailable();
    const QString state = EngineBridge::instance().ndiState();
    if (!requests.empty() && !usable) {
        if (state != QLatin1String("unknown") && !ndiGateLogged_) {
            ndiGateLogged_ = true;
            qWarning("LiveOutputService: enabled NDI outputs cannot send: %s", qUtf8Printable(EngineBridge::instance().ndiStatus()));
        }
        ndiOutputStates_.clear(); ndiSending_ = false; ndiSendingOutput_.clear();
        setNdiWarming(false, {}); clearNdiPlan(); emit ndiChanged(); return;
    }
    ndiGateLogged_ = false;
    if (requests.empty()) {
        ndiOutputStates_.clear(); ndiSending_ = false; ndiSendingOutput_.clear();
        setNdiWarming(false, {}); clearNdiPlan(); emit ndiChanged(); return;
    }
    if (ndiPlanProbeBusy_.exchange(true)) return;
    QPointer<LiveOutputService> guard(this);
    QThreadPool::globalInstance()->start([guard, requests = std::move(requests)]() mutable {
        std::vector<NdiProbeResult> results;
        results.reserve(requests.size());
        for (const auto &request : requests) {
            NdiProbeResult result;
            result.outputName = request.name; result.senderName = request.sender; result.fps = request.fps; result.provider = request.provider;
            result.nodeId = request.nodeId; result.audioWired = request.audioWired;
            auto output = pr::RenderEngine::Instance().GetOutput(request.buffer.toStdString());
            if (output.ok()) { result.buffer = std::dynamic_pointer_cast<pr::FrameBufferOutput>(output.value()); result.bufferOk = bool(result.buffer); }
            if (result.buffer) { result.enabled = result.buffer->Enabled(); const auto frame = result.buffer->LastFrameSnapshot(); result.hasFrame = frame && !frame->empty(); }
            results.push_back(std::move(result));
        }
        if (!guard) return;
        vgr::EngineWorker::onInterfaceThread(guard.data(), [guard, results = std::move(results)]() mutable {
            if (auto *self = guard.data()) self->applyNdiPlanProbe(results);
        });
    });
}

// One warning per mid-session output that cannot send, naming the wait. The
// message is deliberately specific — "not sending" alone leaves the operator
// guessing whether it is the cable, the style, the toggle or the machine.
void LiveOutputService::warnSilentNewOutput(const QString &name, const QString &reason)
{
    if (name.isEmpty() || ndiSilentWarned_.contains(name))
        return;
    ndiSilentWarned_.insert(name);
    qCWarning(lcNdiProgress, "LiveOutputService: NDI output '%s' was added during this session "
                             "and is not sending — %s",
              qUtf8Printable(name), qUtf8Printable(reason));
    EventBus::instance().notify(
        QStringLiteral("NDI output '%1' was added while live but is not sending — %2.")
            .arg(name, reason),
        QStringLiteral("warning"), QStringLiteral("Outputs"),
        QStringLiteral("outputs.ndi.outputSilent"));
}

void LiveOutputService::applyNdiPlanProbe(const std::vector<NdiProbeResult> &results)
{
    ndiPlanProbeBusy_ = false;
    if (!live_) { clearNdiPlan(); return; }
    // THE ROSTER AS IT IS NOW, not as it was when the walk started. The probe
    // runs on the pool, so a row switched off (or removed) while it walked came
    // back "ready" and was published anyway: a disabled output kept its sender
    // for one more poll, and — the reported case — a row disabled in the same
    // turn it was added still announced itself as live. One roster read here
    // settles both, because the enable flag is the operator's truth.
    QSet<QString> activeNames;
    if (const auto *model = OutputListModel::instance()) {
        for (int i = 0; i < model->rowCount(); ++i) {
            const QVariantMap row = model->getOutput(i);
            if (row.value("kind").toString() == QLatin1String("NDI")
                && row.value("isEnabled").toBool())
                activeNames.insert(row.value("name").toString());
        }
    }
    std::vector<NdiSendPlan> plans;
    QVariantMap states;
    QString firstSending, firstWarming;
    qulonglong totalFrames = 0;   // the feed's TOTAL: every sender's own counter
    for (const auto &result : results) {
        // Switched off (or gone) since the walk started: publish NOTHING for it —
        // no plan (so its sender stops on the next beat), no state (so its pill
        // stops claiming a feed), no announcement.
        if (!activeNames.contains(result.outputName))
            continue;
        QVariantMap status;
        const auto oldStatus = ndiOutputStates_.value(result.outputName).toMap();
        const qulonglong count = result.provider ? result.provider->FramesSent() : 0;
        totalFrames += count;
        const bool ready = result.provider && result.bufferOk && result.enabled;
        const bool sending = ready && result.hasFrame;
        status.insert(QStringLiteral("frames"), QVariant::fromValue<qulonglong>(count));
        status.insert(QStringLiteral("sending"), sending);
        status.insert(QStringLiteral("warming"), !sending);
        status.insert(QStringLiteral("fps"), result.fps);
        states.insert(result.outputName, status);
        if (ready) {
            NdiSendPlan plan; plan.outputName = result.outputName; plan.provider = result.provider;
            plan.buffer = result.buffer; plan.senderName = result.senderName; plan.fps = result.fps;
            plan.nodeId = result.nodeId;
            // A sender that is ABOUT to send gets its listener here, once: the
            // plan is republished on every roster change and an existing listener
            // is reused, so an audio glitch cannot come from a poll re-arming it.
            // No audio wire into the node = no listener at all (see the walk).
            if (result.audioWired && !plan.nodeId.isEmpty()) {
                plan.audio = ndiAudio_.value(plan.nodeId);
                if (!plan.audio) {
                    plan.audio = std::make_shared<NdiAudio>();
                    ndiAudio_.insert(plan.nodeId, plan.audio);
                    listenOnNdiOutput(plan.nodeId, plan.audio);
                }
            }
            plans.push_back(std::move(plan));
        }
        if (sending && firstSending.isEmpty()) firstSending = result.outputName;
        if (!sending && firstWarming.isEmpty()) firstWarming = result.outputName;
        if (sending && !oldStatus.value(QStringLiteral("sending")).toBool())
            qCWarning(lcNdiProgress, "LiveOutputService: NDI output '%s' is sending", qUtf8Printable(result.outputName));
        // THE MID-SESSION OUTPUT, ANNOUNCED. One added after the session began
        // is not part of the plan the operator started with, so it gets a note
        // the moment its sender is actually on the wire (a row that never
        // reaches this point is the case the drift watch reports instead).
        // Outputs that were there at GO LIVE stay silent — seven toasts on air
        // would be noise, not information.
        if (sending && !ndiSessionNames_.contains(result.outputName)
            && !ndiAnnouncedNames_.contains(result.outputName)) {
            ndiAnnouncedNames_.insert(result.outputName);
            ndiSilentSinceMs_.remove(result.outputName);   // it made it
            EventBus::instance().notify(
                QStringLiteral("NDI output '%1' is live — '%2' is on the network now.")
                    .arg(result.outputName, result.senderName),
                QStringLiteral("success"), QStringLiteral("Outputs"),
                QStringLiteral("outputs.ndi.outputLive"));
        } else if (!sending && !ndiSessionNames_.contains(result.outputName)
                   && !ndiSilentWarned_.contains(result.outputName)) {
            // ADDED MID-SESSION AND STILL DARK. Watched from first sight; once
            // the grace elapses, one warning names the actual wait rather than
            // leaving it at "not sending". (A row that was on air at GO LIVE is
            // never watched here — its warm-up is the session's own start.)
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            auto since = ndiSilentSinceMs_.constFind(result.outputName);
            if (since == ndiSilentSinceMs_.cend())
                ndiSilentSinceMs_.insert(result.outputName, now);
            else if (now - since.value() >= kNdiSilentWarnMs) {
                const QString reason = !result.bufferOk
                    ? QStringLiteral("its frame buffer has not been created yet")
                    : (!result.enabled
                        ? QStringLiteral("its frame buffer is held off")
                        : (!result.hasFrame
                            ? QStringLiteral("no frame has been rendered for it yet")
                            : QStringLiteral("its sender has not opened")));
                warnSilentNewOutput(result.outputName, reason);
            }
        }
    }
    QStringList probeParts;
    for (const auto &result : results) {
        const qulonglong frames = result.provider ? result.provider->FramesSent() : 0;
        probeParts.append(QStringLiteral("%1{buffer=%2,enabled=%3,frame=%4,sent=%5}")
                              .arg(result.outputName).arg(result.bufferOk ? 1 : 0)
                              .arg(result.enabled ? 1 : 0).arg(result.hasFrame ? 1 : 0).arg(frames));
    }
    const QString probeSignature = probeParts.join(QStringLiteral(" | "));
    if (probeSignature != ndiProbeLogSignature_) {
        ndiProbeLogSignature_ = probeSignature;
        qInfo("LiveOutputService: NDI probe result: %s", qUtf8Printable(probeSignature));
    }
    ndiOutputStates_ = states;
    // Published from the per-output counters just read, so the pill's number is
    // the FEED's, not whichever sender happened to send last (see ndiFramesSent_).
    ndiFramesSent_.store(totalFrames, std::memory_order_relaxed);
    setNdiPlan(plans);
    ndiSending_ = !firstSending.isEmpty(); ndiSendingOutput_ = firstSending;
    setNdiWarming(!firstWarming.isEmpty(), firstWarming);
    emit ndiChanged();
}

// Stop the feed when the loop stops (the send clock and the poll are both
// dead by then — stop() stops the clock first).
void LiveOutputService::stopNdiFeed()
{
    // The pill's own state is this thread's (see the members) — no lock, and
    // nothing to wait for.
    bool changed = false;
    if (ndiSending_) {
        ndiSending_ = false;
        ndiSendingOutput_.clear();
        changed = true;
    }
    if (changed)
        emit ndiChanged();
    ndiOutputStates_.clear();
    setNdiWarming(false, {});

    // The plan goes with the session: an empty plan is what stops the feed
    // thread's own clock (see ndiWorkerTick).
    clearNdiPlan();
    // The per-sender one-shot notes belong to the session that just ended:
    // the next live session announces every output again, from scratch.
    ndiSendFailedNames_.clear();
    ndiFirstFrameNames_.clear();
    ndiSlowNames_.clear();
    ndiFastStreaks_.clear();
    ndiRateWindows_.clear();
    ndiSessionNames_.clear();
    ndiAnnouncedNames_.clear();
    ndiSilentSinceMs_.clear();
    ndiSilentWarned_.clear();
    if (ndiWorker_) {
        const auto providers = ndiProviders_.values();
        QMetaObject::invokeMethod(ndiWorker_, [providers] {
            for (const auto &provider : providers)
                if (provider) provider->ResetSender();
        }, Qt::QueuedConnection);
    }
    emit ndiChanged();
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
    // THE WORKER IS DESTROYED IN THE THREAD THAT OWNS IT. It is the parent of the
    // feed's QTimer (`ndiSendTimer_`, created on this thread in setNdiPlan), and a
    // QObject owns its children — so deleting the worker from the interface thread
    // (what shutdownNdiWorker used to do) destroyed that timer from the wrong
    // thread. Qt says so on every quit after a live session:
    //     QObject::killTimer: Timers cannot be stopped from another thread
    //     QObject::~QObject: Timers cannot be stopped from another thread
    // finished() is emitted IN this thread as its loop ends, which is where
    // deleteLater() still runs — so the worker takes itself and its timer down on
    // its own side, and the GUI never reaches into it to destroy anything.
    //
    // finished() is the one hook that is guaranteed to run IN this thread as its
    // loop ends, so the destruction hangs off that rather than off a teardown call
    // queued from the GUI side: a queued call is NOT reliably drained before
    // quit() ends the loop (measured — the teardown lambda this replaced never ran,
    // while deleteLater off finished does), and a fix whose only evidence is an
    // absent warning is one nobody can check later. Hence the line.
    connect(ndiThread_, &QThread::finished, ndiWorker_, [this] {
        qInfo("LiveOutputService: NDI worker teardown on the feed thread (its send clock exists=%d)",
              ndiSendTimer_ != nullptr);
        ndiWorker_->deleteLater();
    });
    // AboutTo-quit ordering is handled by shutdownNdiWorker() from
    // main.cpp's aboutToQuit — BEFORE the engine's own teardown.
    ndiThread_->start();
}

void LiveOutputService::clearNdiPlan()
{
    ndiClockArmed_ = false;   // no plan: the feed stops its own clock next beat
    // Through setNdiPlan, not straight to the thread: an empty plan also RETIRES
    // every audio listener (see the prune there). Handing it over directly left
    // each stopped output's tap pulling its node's mix for the life of the process.
    setNdiPlan({});
}

void LiveOutputService::setNdiPlan(const std::vector<NdiSendPlan> &plans)
{
    // WHAT WE LISTEN TO IS WHAT WE SEND. A row that stopped sending (switched
    // off, its buffer gone, the session over) must stop pulling its node's mix,
    // and every path that empties the plan — including clearNdiPlan — lands
    // here, so this is the one place a listener is retired.
    {
        QSet<QString> sendingNodes;
        for (const auto &plan : plans)
            if (!plan.nodeId.isEmpty())
                sendingNodes.insert(plan.nodeId);
        for (auto it = ndiAudio_.begin(); it != ndiAudio_.end();) {
            if (sendingNodes.contains(it.key())) { ++it; continue; }
            silenceNdiOutput(it.value());
            it = ndiAudio_.erase(it);
        }
    }
    // The thread that will run the feed has to exist before the plan is handed
    // to it (arm first, then publish — the order the mutex version used, so the
    // opening frame still does not wait for the next poll).
    if (!plans.empty())
        ensureNdiWorker();
    handOverNdiPlan(plans);   // ...it has the plan before its first tick
    if (plans.empty()) {
        ndiClockArmed_ = false;     // the feed thread stops its own clock
        return;
    }
    if (ndiClockArmed_)
        return;                     // the feed's clock is already running
    if (!ndiWorker_)
        return;                     // no feed thread could be created: nothing to arm
    ndiClockArmed_ = true;
    // The clock is CREATED on the feed thread (a QTimer belongs to the thread
    // that runs it) and armed there; its interval follows the plan from inside
    // ndiWorkerTick, so the GUI never has to reach in twice. The arm is queued
    // once per feed, not once per poll: the plan itself is what republishes.
    QMetaObject::invokeMethod(ndiWorker_, [this] {
        if (!ndiSendTimer_) {
            ndiSendTimer_ = new QTimer(ndiWorker_);
            ndiSendTimer_->setTimerType(Qt::PreciseTimer);
            ndiSendTimer_->setInterval(5);
            connect(ndiSendTimer_, &QTimer::timeout, ndiWorker_, [this] { ndiWorkerTick(); });
        }
        if (!ndiSendTimer_->isActive())
            ndiSendTimer_->start();
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// The NDI send clock — paced at the selected NDI output's configured
// Refresh rate (Settings · Outputs). pushNdiFrame used to ride the 10 Hz
// GUI poll, which made that select decorative: the feed ran at the poll's
// rate whatever the setting said. Now the setting IS the cadence — the
// timer's interval is derived from the roster value, and the provider
// advertises the same number in every frame's metadata, so receivers'
// frame clocks finally agree with both the setting and the real send rate.
// ---------------------------------------------------------------------------
float LiveOutputService::ndiConfiguredFps()
{
    const QVariantMap preferred = preferredNdiOutput(OutputListModel::instance());
    if (!preferred.isEmpty()) {
        const QString preferredRate = preferred.value("refresh").toString();
        bool preferredOk = false;
        const float preferredFps = preferredRate.left(preferredRate.indexOf(QLatin1Char(' ')))
                                       .toFloat(&preferredOk);
        return bps::display::AcceptedOutputFrameRate(preferredOk ? preferredFps : 0.0f);
    }
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
    // Nothing is queued to the feed thread from here any more. It looks like the
    // careful thing to do — hand the timer's teardown to the thread that owns it
    // before the loop is told to quit — but it does not work: a call posted to a
    // loop that is about to exit is not reliably drained (measured: the lambda this
    // replaced never ran, on a quit 2 s after going live), so anything load-bearing
    // would be silently skipped. The worker's destruction rides finished() instead
    // (where it is created), which IS delivered in this thread as the loop ends.
    // `quit()` only ends the loop; the worker's own teardown does the rest.
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
    ndiPlanPublished_ = false;   // the plan went with the thread it was handed to
        qWarning("LiveOutputService: NDI send worker did not stop within 2s — leaving it detached rather than hanging the UI");
        return;
    }
    // Nothing is deleted from here. The timer was deleted on the feed thread by
    // the queued teardown above (or the loop exited first and the worker's own
    // destruction took it — either way that destruction happens in the thread that
    // owns it, which is the point), and the worker deletes ITSELF as its loop ends
    // (the finished/deleteLater connect where it is created). So the GUI thread
    // only drops the raw pointers, with the objects they belonged to.
    ndiSendTimer_ = nullptr;
    ndiWorker_ = nullptr;
    delete ndiThread_;      // `this`-parented, but explicit for clarity
    ndiThread_ = nullptr;
    ndiClockArmed_ = false;
    // The plan lived on the thread that just went away: a later feed has to
    // hand a fresh one over rather than be told it already did.
    ndiPlanPublished_ = false;
}

void LiveOutputService::startNdiSendClock()
{
    // WHO WAS HERE WHEN THE SESSION STARTED. Every output that exists now is
    // part of the session and needs no announcement; one added LATER is the
    // case the operator has to be told about ("I created a new output mid
    // session … I'm not getting notif"). Snapshot the names, then notify per
    // output the first time it actually sends (applyNdiPlanProbe).
    ndiSessionNames_.clear();
    ndiAnnouncedNames_.clear();
    if (const auto *model = OutputListModel::instance()) {
        for (int i = 0; i < model->rowCount(); ++i) {
            const QVariantMap row = model->getOutput(i);
            if (row.value("kind").toString() == QLatin1String("NDI") && row.value("isEnabled").toBool())
                ndiSessionNames_.insert(row.value("name").toString());
        }
    }
    // Bring the feed thread up and publish the current plan immediately: the
    // 10 Hz poll would otherwise leave the opening frame waiting for its first
    // tick. WHAT to send is resolved there (the roster is GUI-owned); WHEN is
    // the feed thread's own clock from that point on.
    ensureNdiWorker();
    recheckNdiPlanOffThread();
}

void LiveOutputService::stopNdiSendClock()
{
    // An empty plan is the signal: the feed thread skips and sleeps its own
    // clock on the next beat (see ndiWorkerTick) — no cross-thread stop call is
    // needed, and none could race the plan it is stopping. The feed's own
    // pacing clock is cleared there, on its own thread (nothing here touches a
    // member the feed thread owns).
    clearNdiPlan();
    // Every output's drift window dies with the clock — a fresh session must
    // not measure across the gap between two feeds (or inherit a stale tier).
    ndiRateWindows_.clear();
    setNdiWarming(false, QString());   // off air: nothing is warming
}

// The poll's NDI watch: publish WHAT to feed. Every beat of the 10 Hz poll
// re-resolves the selected NDI output (the roster is GUI-owned) and hands
// the feed thread a fresh value — so a row enabled mid-live, a rename, a
// disabled output or a changed Refresh rate all reach the wire within a poll,
// with no clock to re-arm and nothing to keep in sync by hand. The empty-plan
// paths publish nothing, which is what stops the feed.
void LiveOutputService::pollTickRecheckNdi()
{
    if (!live_)
        return;
    // Re-aim the roster connection first: if OutputListModel's singleton moved
    // (the engine's own object adopted after this service was constructed), the
    // signal connection would otherwise point at a roster nobody edits.
    connectToNdiOutputRoster();
    recheckNdiPlanOffThread();
}

void LiveOutputService::connectToNdiOutputRoster()
{
    auto *model = OutputListModel::instance();
    if (!model)
        return;
    // Already following THIS object: nothing to re-wire. (A different one means
    // OutputListModel adopted the engine's roster after this service was built
    // — see ndiRosterModel_'s note — so the old connection is stale and this
    // one has to be made, or the feed would never hear about a roster edit.)
    if (ndiRosterConnected_ && ndiRosterModel_ == model)
        return;
    ndiRosterConnected_ = true;
    ndiRosterModel_ = model;
    const QMetaObject::Connection rosterConnection = connect(model, &OutputListModel::outputRosterChanged, this, [this] {
        qInfo("LiveOutputService: outputRosterChanged received live=%d", live_ ? 1 : 0);
        if (live_) {
            qInfo("LiveOutputService: output roster changed during live; refreshing all NDI send plans");
            recheckNdiPlanOffThread();
        }
    }, Qt::DirectConnection);
    qInfo("LiveOutputService: connected output roster signal model=%p ok=%d rows=%d",
          static_cast<void *>(model), bool(rosterConnection), model->rowCount());
}

// ---- Output frame-rate watch -----------------------------------------------
// One measured number drives two things, both off the GUI thread's 10Hz poll
// (the counters are atomics the feed thread publishes, so nothing here touches
// the worker's state directly — and nothing here waits on it):
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
    // A stopped feed has no rate to measure: drop every window and streak so
    // the next session starts clean and cannot inherit a toast.
    if (!live_ || ndiOutputStates_.isEmpty()) {
        ndiRateWindows_.clear();
        return;
    }

    // ONE WINDOW PER OUTPUT. Every enabled NDI row is its own sender at its own
    // configured rate, so the old single window measured the feed's TOTAL (or,
    // once the counter came from "whichever sender just sent", one sender's
    // count against another's baseline) and then named the preferred output in
    // the toast — which is why every NDI report in this app said "New Screen 2"
    // no matter which sender was actually struggling, and why a healthy
    // seven-output feed logged "made no progress for two windows".
    //
    // A row disabled mid-session must not keep a window alive (it would warn
    // about a rate nobody is sending).
    for (auto it = ndiRateWindows_.begin(); it != ndiRateWindows_.end();) {
        if (!ndiOutputStates_.contains(it.key()))
            it = ndiRateWindows_.erase(it);
        else
            ++it;
    }

    for (auto it = ndiOutputStates_.cbegin(); it != ndiOutputStates_.cend(); ++it) {
        const QString name = it.key();
        const QVariantMap status = it.value().toMap();
        const bool sending = status.value(QStringLiteral("sending")).toBool();
        const qulonglong frames = status.value(QStringLiteral("frames")).toULongLong();
        const float configured = status.value(QStringLiteral("fps")).toFloat();

        NdiRateWindow &w = ndiRateWindows_[name];
        // A warming sender (no frame in its buffer yet) is not a rate problem:
        // rebaseline it and leave its streaks alone.
        if (!sending) {
            w.clock.invalidate();
            w.frames = frames;
            continue;
        }
        if (!w.clock.isValid()) {
            w.frames = frames;   // first sample establishes this output's baseline
            w.clock.start();
            continue;
        }
        const qint64 elapsed = w.clock.elapsed();
        if (elapsed < kDriftWindowMs)
            continue;

        // Roll the window (defensive on the counter: it never decreases, but a
        // provider reset mid-session would, and must not read as a negative
        // rate).
        const qulonglong delta = frames >= w.frames ? frames - w.frames : 0;
        w.frames = frames;
        w.clock.restart();

        // A sender that still has an active plan but completed NO send across
        // two full windows is stuck: recreate ITS SDK sender on the thread that
        // owns SendFrame (serialized between sends; the next beat lazily opens a
        // fresh one). This used to reset DisplayEngine's registered "Ndi"
        // provider — an object this feed never sends through — so the reset hit
        // nothing and the stuck sender stayed stuck.
        if (delta > 0) {
            w.noProgressWindows = 0;
        } else if (++w.noProgressWindows >= kDriftWindowsToWarn) {
            w.noProgressWindows = 0;   // retry after another two windows if this reset does not recover
            ensureNdiWorker();
            const auto provider = ndiProviders_.value(name);
            if (ndiWorker_ && provider) {
                QMetaObject::invokeMethod(ndiWorker_, [provider, name] {
                    provider->ResetSender();
                    qCWarning(lcNdiProgress,
                              "LiveOutputService: NDI output '%s' sent nothing for two windows; its sender was reset for automatic recovery",
                              qUtf8Printable(name));
                }, Qt::QueuedConnection);
            }
        }

        if (configured <= 0.0f) {
            w.tier = -1;
            w.configured = 0.0f;
            continue;
        }

        const double measured = double(delta) * 1000.0 / double(elapsed);

        // Did THIS output's SETTING move since its last window? (Compared before
        // we store it.) True means the rate change came from Settings, not the
        // wire, and the Settings-side toast already covers it.
        const bool configuredChanged = w.configured > 0.0f && !qFuzzyCompare(configured, w.configured);
        w.configured = configured;

        // ---- 1. runtime rate-change toast (on a real step) ---------------
        const int tier = measured >= configured * 0.75 ? 0
                       : (measured >= configured * 0.35 && measured <= configured * 0.7 ? 1 : 2);
        const int previousTier = w.tier;
        w.tier = tier;
        if (!configuredChanged && previousTier >= 0 && tier != previousTier) {
            if (previousTier == 0 && tier == 1) {
                EventBus::instance().notify(
                    QStringLiteral("NDI feed '%1' dropped to about %2 fps (about half of %3 Hz) — the sender is running behind. It returns to full rate automatically.")
                        .arg(name).arg(qRound(measured)).arg(qRound(configured)),
                    QStringLiteral("warning"), QStringLiteral("Outputs"),
                    QStringLiteral("outputs.ndi.rateReduced"));
            } else if (previousTier == 1 && tier == 0) {
                EventBus::instance().notify(
                    QStringLiteral("NDI feed '%1' is back at the configured rate (about %2 Hz).")
                        .arg(name).arg(qRound(configured)),
                    QStringLiteral("success"), QStringLiteral("Outputs"),
                    QStringLiteral("outputs.ndi.rateRestored"));
            }
        }

        // ---- 2. sustained drift warning ----------------------------------
        // A clean backoff half (tier 1) is already announced by the change
        // toast above, so the warning here is reserved for the genuinely-stuck
        // output (tier 2), sustained across consecutive windows.
        if (tier != 2) {
            w.slowWindows = 0;
            w.warned = false;
            continue;
        }
        if (++w.slowWindows < kDriftWindowsToWarn || w.warned)
            continue;
        w.warned = true;
        EventBus::instance().notify(
            QStringLiteral("NDI output '%1' is running at about %2 fps but is set to %3 Hz — the feed can't keep up. Lower the refresh rate or check the machine's load.")
                .arg(name).arg(qRound(measured)).arg(qRound(configured)),
            QStringLiteral("warning"), QStringLiteral("Outputs"),
            QStringLiteral("outputs.framerate.drift"));
    }
}

// The network-visible NDI source name: "<AppName> - <Row name>"
// ("VGRPresenter - New Screen 2") — how Studio Monitor lists a machine's sources — falling
// back to the bare row name when the app name is unavailable, and to "" (the
// provider's own "VGR Program" default) when the row has no name at all.
QString LiveOutputService::ndiSenderDisplayName(const QString &rowName)
{
    QString resolved = rowName;
    if (resolved.isEmpty()) {
        resolved = preferredNdiOutput(OutputListModel::instance()).value("name").toString();
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
    auto *provider = []() -> bps::display::NdiDisplayProvider * {
        auto p = bps::display::DisplayEngine::Instance().Provider("Ndi");
        return p ? dynamic_cast<bps::display::NdiDisplayProvider *>(p.get()) : nullptr;
    }();
    if (!provider)
        return;
    ndiArmedName_ = name;
    // Name + reset ride the WORKER thread (FreeShow's model: the main thread
    // never touches the SDK): ResetSender destroys the old SDK sender — a
    // synchronous SDK call that on the GUI thread stalled GO LIVE — and the
    // next send on the worker recreates it under the new name. The worker is
    // created here if absent so the reset lands after any queued job on a
    // live session (event-loop order keeps the sequence right).
    // Same thread/worker pair as every other path — through the ONE constructor
    // above rather than a copy of it: the worker's ownership rule (it deletes
    // itself, and its timer with it, on its own thread) is stated once, and a
    // second copy of this block is how one of them ends up without it.
    ensureNdiWorker();
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
