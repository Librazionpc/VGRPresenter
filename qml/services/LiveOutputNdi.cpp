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
#include <QRunnable>
#include <QThreadPool>
#include "services/ShowService.h"
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

#include "services/NdiLog.hpp"

namespace pl = bps::presentation;
namespace pr = bps::rendering;

namespace {
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

// ---- NDI program sender ----------------------------------------------------

void LiveOutputService::listenOnNdiOutput(const QString &nodeId,
                                          const std::shared_ptr<NdiAudio> &audio)
{
    if (!audio || audio->mixTap != 0 || nodeId.isEmpty())
        return;
    audio->nodeId = nodeId.toStdString();
    auto state = audio;
    audio->mixTap = bps::audio::AudioEngine::Instance().AddMixTap(
        audio->nodeId,
        [state](const float *samples, uint32_t frames, uint32_t channels,
                uint32_t rateHz, uint64_t startFrame) {
            if (samples == nullptr || frames == 0 || channels == 0 || rateHz == 0)
                return;
            std::lock_guard<std::mutex> lock(state->mutex);
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

void LiveOutputService::ndiWorkerSendFrame(void *providerVoid,
                                           std::shared_ptr<const bps::display::NdiRgbaFrame> frame,
                                           QString senderName, float fps)
{
    auto *provider = static_cast<bps::display::NdiDisplayProvider *>(providerVoid);
    auto &svc = LiveOutputService::instance();
    if (!provider || !frame || frame->bytes.empty())
        return;

    if (!senderName.isEmpty())
        provider->SetSenderName(senderName.toStdString());
    provider->SetFrameRate(fps);

    QElapsedTimer clock;
    clock.start();
    const bool sent = provider->SendRgbaFrame(frame, fps).ok();
    const qint64 sendMs = clock.elapsed();

    const QString senderKey = QString::fromStdString(provider->SenderName());
    // Several senders run at once on the pool now, so these notes are shared state: DECIDE them under the
    // lock, then log outside it — a log line under a lock is a printer under a lock, and one sender's
    // logging should never delay another's frame.
    bool noteFirst = false;
    bool noteSlow = false;
    bool noteFailed = false;
    {
        std::lock_guard<std::mutex> book(svc.ndiSendBookMutex_);
        if (sent) {
            if (!svc.ndiFirstFrameNames_.contains(senderKey)) {
                svc.ndiFirstFrameNames_.insert(senderKey);
                noteFirst = true;
            }
            svc.ndiSendFailedNames_.remove(senderKey);
            if (sendMs > qMax(50, fps > 0.0f ? int(1000.0f / fps) : 100)) {
                if (!svc.ndiSlowNames_.contains(senderKey)) {
                    svc.ndiSlowNames_.insert(senderKey);
                    svc.ndiFastStreaks_.insert(senderKey, 0);
                    noteSlow = true;
                }
            } else if (svc.ndiFastStreaks_.value(senderKey) + 1 >= 30) {
                svc.ndiFastStreaks_.insert(senderKey, 0);
                svc.ndiSlowNames_.remove(senderKey);   // sustained recovery re-arms this sender's note
            } else {
                svc.ndiFastStreaks_.insert(senderKey, svc.ndiFastStreaks_.value(senderKey) + 1);
            }
        } else if (!svc.ndiSendFailedNames_.contains(senderKey)) {
            svc.ndiSendFailedNames_.insert(senderKey);
            noteFailed = true;
        }
    }
    if (sent && provider->FramesSent() == 1)
        qInfo("LiveOutputService: first frame sent for NDI output '%s'", qUtf8Printable(senderKey));
    if (noteFirst)
        qCWarning(lcNdiProgress, "LiveOutputService: NDI frames flowing as '%s' — discovery on a receiving monitor can take 10-30s",
                  qUtf8Printable(senderKey));
    if (noteSlow)
        qCWarning(lcNdiProgress, "LiveOutputService: NDI send is slow (%lld ms, sender '%s') — the feed skips the beats it overruns; check the network and the configured rate",
                  sendMs, qUtf8Printable(senderKey));
    if (noteFailed)
        qWarning("LiveOutputService: NDI SendFrame FAILED (sender '%s') — check the engine log for the provider's own reason",
                 qUtf8Printable(senderKey));
}

void LiveOutputService::ndiWorkerSendAudio(void *providerVoid,
                                          const std::shared_ptr<NdiAudio> &audio)
{
    auto *provider = static_cast<bps::display::NdiDisplayProvider *>(providerVoid);
    if (!provider || !audio)
        return;
    const std::string senderId = provider->SenderId();
    if (senderId.empty())
        return;

    std::vector<NdiAudioBlock> ready;
    {
        std::lock_guard<std::mutex> lock(audio->mutex);
        if (audio->queue.empty())
            return;
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

void LiveOutputService::ndiWorkerTick()
{
    const auto plans = ndiFeedPlan_;
    if (plans.empty()) {
        if (ndiSendTimer_ && ndiSendTimer_->isActive())
            ndiSendTimer_->stop();
        ndiSendClock_.invalidate();
        ndiSendDueMs_.clear();
        return;
    }
    for (const auto &plan : plans)
        if (plan.audio)
            ndiWorkerSendAudio(plan.provider.get(), plan.audio);
    // ---- THE TICK DECIDES WHAT IS DUE; THE POOL DOES THE SENDING --------------------------------
    // Each sender keeps its OWN cadence here — the same per-output clock as before, plus an in-flight mark
    // so one sender is never queued twice — while the sends fan out over a few threads. A single slow route
    // used to hold every sender behind it, on the one thread that also paced them all.
    std::map<const void *, std::vector<NdiSendPlan>> due;   // grouped by the frame buffer
    // ---- DEADLINE PACING, NOT "WAIT AND RE-MEASURE" -----------------------------------------------
    // This used to wait for `elapsed >= 0.9 x period` on a 5 ms tick grid. 60 Hz wants 16.67 ms; the grid
    // only offers 15 or 20, and 15 failed that 0.9 test (15 < 15.3), so every frame landed on the 20 ms
    // beat: ~50 fps, and the drift of measuring from the last SEND rather than from a schedule kept it
    // there. That is the "4-6 short of 60" on the telemetry line. Each sender now owns a deadline that
    // advances by exactly one period, so a late tick delays one frame and never shifts the ones after it.
    if (!ndiSendClock_.isValid())
        ndiSendClock_.start();
    const double nowMs = double(ndiSendClock_.elapsed());
    {
        std::lock_guard<std::mutex> lock(ndiInFlightMutex_);
        for (const auto &plan : plans) {
            if (!plan.provider || !plan.buffer) continue;
            if (ndiInFlight_.contains(plan.outputName)) continue;   // its previous send is still running
            const double periodMs = 1000.0 / double(plan.fps > 0.0f ? plan.fps : 30.0f);
            auto &nextDue = ndiSendDueMs_[plan.outputName];
            // A new sender, or a stall of several frames: start from now instead of firing a catch-up
            // burst at the SDK (the far machine shows that as a stutter, not as keeping up).
            if (nextDue <= 0.0 || nextDue < nowMs - 4.0 * periodMs)
                nextDue = nowMs;
            if (nowMs + 0.5 < nextDue) continue;   // 0.5 ms: the feed clock's own resolution
            nextDue += periodMs;
            if (nextDue < nowMs)                   // a beat was already missed: resync, send this one
                nextDue = nowMs + periodMs;
            ndiInFlight_.insert(plan.outputName);
            due[plan.buffer.get()].push_back(plan);
        }
    }
    for (auto &group : due) {
        // Read the group BEFORE moving it: argument evaluation order is unspecified, and front() on the
        // moved-from vector is the assertion this hit.
        const std::shared_ptr<bps::rendering::FrameBufferOutput> groupBuffer = group.second.front().buffer;
        postNdiSendGroup(groupBuffer, std::move(group.second));
    }
}

void LiveOutputService::postNdiSendGroup(std::shared_ptr<bps::rendering::FrameBufferOutput> buffer,
                                         std::vector<NdiSendPlan> sends)
{
    if (!ndiPool_ || !buffer || sends.empty()) {
        std::lock_guard<std::mutex> lock(ndiInFlightMutex_);
        for (const auto &plan : sends)
            ndiInFlight_.remove(plan.outputName);
        return;
    }
    ndiPool_->start(QRunnable::create([this, buffer = std::move(buffer), sends = std::move(sends)] {
        // ONE COPY FOR EVERY SENDER ON THIS BUFFER, and into a REUSED slot when nothing is still reading
        // it: a fresh frame per tick was malloc churn on top of the
        // conversion. A slot a sender still holds has use_count() > 1, so it is never refilled under it;
        // with both slots held a tick allocates one and the next sessions reuse it again.
        std::shared_ptr<bps::display::NdiRgbaFrame> slot;
        // THE PICTURE HAS NOT CHANGED SINCE THE LAST TICK -> THERE IS NOTHING TO CONVERT. A still slide (or a
        // paused picture) used to be converted (8 MB at 1080p) and re-sent on every beat of every sender;
        // now the previous conversion is sent again as it is (the receiver still gets its frames).
        const auto snapshot = buffer->LastFrameSnapshot();
        std::shared_ptr<const bps::display::NdiRgbaFrame> frame;
        {
            std::lock_guard<std::mutex> lock(ndiRgbaMutex_);
            const auto cached = ndiConverted_.constFind(buffer.get());
            if (snapshot && cached != ndiConverted_.constEnd() && cached->source == std::shared_ptr<const void>(snapshot)
                && cached->converted)
                frame = cached->converted;
        }
        if (!frame) {
        {
            std::lock_guard<std::mutex> lock(ndiRgbaMutex_);
            // NOT named `slots`: that is a Qt keyword macro and expands to nothing.
            std::vector<std::shared_ptr<bps::display::NdiRgbaFrame>> &reusable = ndiRgbaSlots_[buffer.get()];
            for (auto &candidate : reusable) {
                if (candidate.use_count() == 1) {
                    slot = candidate;
                    break;
                }
            }
            if (!slot) {
                slot = std::make_shared<bps::display::NdiRgbaFrame>();
                reusable.push_back(slot);
            }
        }
        if (snapshot && !snapshot->empty()) {
            bps::display::ConvertToNdiRgba(snapshot->pixels.data(), snapshot->width, snapshot->height, *slot);
            frame = slot;
            std::lock_guard<std::mutex> lock(ndiRgbaMutex_);
            ndiConverted_[buffer.get()] = NdiConvertedFrame{std::shared_ptr<const void>(snapshot), slot};
        }
        }   // end: convert only when the picture changed
        if (!frame) {
            std::lock_guard<std::mutex> lock(ndiInFlightMutex_);
            for (const auto &plan : sends)
                ndiInFlight_.remove(plan.outputName);
            return;
        }
        // ONE JOB PER SENDER, over the one shared conversion. Sending every sender of this buffer inside this
        // job made them take turns: a slow SDK handoff on one output held the other behind it, so the pool
        // bought nothing whenever two outputs shared a frame. Now the conversion happens once and each
        // handoff runs on its own worker.
        for (const auto &plan : sends) {
            ndiPool_->start(QRunnable::create([this, frame, plan] {
                if (plan.provider)
                    ndiWorkerSendFrame(plan.provider.get(), frame, plan.senderName,
                                       plan.fps > 0.0f ? plan.fps : 30.0f);
                std::lock_guard<std::mutex> lock(ndiInFlightMutex_);
                ndiInFlight_.remove(plan.outputName);
            }));
        }
        slot.reset();   // the senders' own frame references keep it alive for as long as they need it
    }));
}

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
    if (ndiWarming_ == on && (!on || ndiWarmingOutput_ == name))
        return;
    ndiWarming_ = on;
    ndiWarmingOutput_ = on ? name : QString();
    emit ndiChanged();
}

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

void LiveOutputService::recheckNdiPlanOffThread()
{
    std::vector<NdiSendRequest> requests = CollectNdiSendRequests();
    LogNdiRoster(requests);
    WarnSilentNdiOutputs();

    QSet<QString> enabledNames;
    for (const auto &request : requests)
        enabledNames.insert(request.name);
    ForgetRetiredNdiProviders(enabledNames);
    AnnounceNdiOutputStates(requests);

    const bool usable = EngineBridge::instance().ndiAvailable();
    const QString state = EngineBridge::instance().ndiState();
    if (!requests.empty() && !usable) {
        if (state != QLatin1String("unknown") && !ndiGateLogged_) {
            ndiGateLogged_ = true;
            qWarning("LiveOutputService: enabled NDI outputs cannot send: %s",
                     qUtf8Printable(EngineBridge::instance().ndiStatus()));
        }
        StopNdiWithoutPlan();
        return;
    }
    ndiGateLogged_ = false;
    if (requests.empty()) {
        StopNdiWithoutPlan();
        return;
    }
    StartNdiPlanProbe(std::move(requests));
}

std::vector<LiveOutputService::NdiSendRequest> LiveOutputService::CollectNdiSendRequests()
{
    std::vector<NdiSendRequest> requests;
    const auto *model = OutputListModel::instance();
    if (!model)
        return requests;
    const auto snap = bps::production::ProductionEngine::Instance().Graph().PublishedSnapshot();
    const auto audioWired = [&snap](const std::string &nodeId) {
        if (!snap || nodeId.empty())
            return false;
        for (const auto &[from, tos] : snap->edges)
            if (from.ends_with("#a") && tos.count(nodeId) != 0)
                return true;
        return false;
    };
    for (int i = 0; i < model->rowCount(); ++i) {
        const QVariantMap row = model->getOutput(i);
        if (row.value("kind").toString() != QLatin1String("NDI")
            || !row.value("isEnabled").toBool())
            continue;
        const QString name = row.value("name").toString();
        const QString nodeId = QStringLiteral("out:%1").arg(row.value("id").toString());
        auto provider = ndiProviders_.value(name);
        if (!provider) {
            provider = std::make_shared<bps::display::NdiDisplayProvider>();
            ndiProviders_.insert(name, provider);
        }
        const std::string buffer = model->hasOwnPass(i)
            ? QStringLiteral("__out_%1__").arg(qHash(name)).toStdString()
            : std::string(bps::live::LiveOutputController::kPreviewName);
        const QString rate = row.value("refresh").toString();
        bool ok = false;
        const float rawFps = rate.left(rate.indexOf(QLatin1Char(' '))).toFloat(&ok);
        const float fps = bps::display::AcceptedOutputFrameRate(ok ? rawFps : 0.0f);
        requests.push_back({name, nodeId, QString::fromStdString(buffer),
                            ndiSenderDisplayName(name), fps,
                            audioWired(nodeId.toStdString()), provider});
    }
    return requests;
}

void LiveOutputService::LogNdiRoster(const std::vector<NdiSendRequest> &requests)
{
    QStringList rosterParts;
    for (const auto &request : requests) {
        rosterParts.append(QStringLiteral("%1->%2@%3")
                               .arg(request.name, request.buffer)
                               .arg(request.fps, 0, 'f', 1));
    }
    const QString rosterSignature = rosterParts.join(QStringLiteral(" | "));
    if (rosterSignature == ndiRosterLogSignature_)
        return;
    ndiRosterLogSignature_ = rosterSignature;
    const auto *model = OutputListModel::instance();
    qInfo("LiveOutputService: NDI recheck live=%d model=%p rows=%d enabledNdi=%d [%s]",
          live_ ? 1 : 0, static_cast<const void *>(model), model ? model->rowCount() : -1,
          static_cast<int>(requests.size()), qUtf8Printable(rosterSignature));
}

void LiveOutputService::WarnSilentNdiOutputs()
{
    const auto *model = OutputListModel::instance();
    if (!model)
        return;
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
}

void LiveOutputService::ForgetRetiredNdiProviders(const QSet<QString> &enabledNames)
{
    for (auto it = ndiProviders_.begin(); it != ndiProviders_.end();) {
        if (enabledNames.contains(it.key())) {
            ++it;
            continue;
        }
        const auto retired = it.value();
        it = ndiProviders_.erase(it);
        ensureNdiWorker();
        if (ndiWorker_)
            QMetaObject::invokeMethod(ndiWorker_, [retired] { retired->ResetSender(); },
                                      Qt::QueuedConnection);
    }
}

bool LiveOutputService::AnnounceNdiOutputStates(const std::vector<NdiSendRequest> &requests)
{
    bool stateAdded = false;
    for (const auto &request : requests) {
        if (ndiOutputStates_.contains(request.name))
            continue;
        ndiOutputStates_.insert(request.name, QVariantMap{
            { QStringLiteral("frames"), QVariant::fromValue<qulonglong>(request.provider->FramesSent()) },
            { QStringLiteral("sending"), false },
            { QStringLiteral("warming"), true },
            { QStringLiteral("fps"), request.fps }
        });
        stateAdded = true;
    }
    if (stateAdded)
        emit ndiChanged();
    return stateAdded;
}

void LiveOutputService::StopNdiWithoutPlan()
{
    ndiOutputStates_.clear();
    ndiSending_ = false;
    ndiSendingOutput_.clear();
    setNdiWarming(false, {});
    clearNdiPlan();
    emit ndiChanged();
}

void LiveOutputService::StartNdiPlanProbe(std::vector<NdiSendRequest> requests)
{
    if (ndiPlanProbeBusy_.exchange(true))
        return;
    QPointer<LiveOutputService> guard(this);
    QThreadPool::globalInstance()->start([guard, requests = std::move(requests)]() mutable {
        std::vector<NdiProbeResult> results;
        results.reserve(requests.size());
        for (const auto &request : requests) {
            NdiProbeResult result;
            result.outputName = request.name;
            result.senderName = request.sender;
            result.fps = request.fps;
            result.provider = request.provider;
            result.nodeId = request.nodeId;
            result.audioWired = request.audioWired;
            auto output = pr::RenderEngine::Instance().GetOutput(request.buffer.toStdString());
            if (output.ok()) {
                result.buffer = std::dynamic_pointer_cast<pr::FrameBufferOutput>(output.value());
                result.bufferOk = bool(result.buffer);
            }
            if (result.buffer) {
                result.enabled = result.buffer->Enabled();
                const auto frame = result.buffer->LastFrameSnapshot();
                result.hasFrame = frame && !frame->empty();
            }
            results.push_back(std::move(result));
        }
        if (!guard)
            return;
        vgr::EngineWorker::onInterfaceThread(
            guard.data(), [guard, results = std::move(results)]() mutable {
                if (auto *self = guard.data())
                    self->applyNdiPlanProbe(results);
            });
    });
}

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

QSet<QString> LiveOutputService::ActiveNdiOutputNames() const
{
    QSet<QString> names;
    if (const auto *model = OutputListModel::instance()) {
        for (int i = 0; i < model->rowCount(); ++i) {
            const QVariantMap row = model->getOutput(i);
            if (row.value("kind").toString() == QLatin1String("NDI")
                && row.value("isEnabled").toBool())
                names.insert(row.value("name").toString());
        }
    }
    return names;
}

void LiveOutputService::applyNdiPlanProbe(const std::vector<NdiProbeResult> &results)
{
    ndiPlanProbeBusy_ = false;
    if (!live_) { clearNdiPlan(); return; }
    const NdiPlanBuild build = BuildNdiPlan(results, ActiveNdiOutputNames());
    LogNdiProbeResults(results);
    ndiOutputStates_ = build.states;
    ndiFramesSent_.store(build.totalFrames, std::memory_order_relaxed);
    setNdiPlan(build.plans);
    ndiSending_ = !build.firstSending.isEmpty();
    ndiSendingOutput_ = build.firstSending;
    setNdiWarming(!build.firstWarming.isEmpty(), build.firstWarming);
    emit ndiChanged();
}

LiveOutputService::NdiPlanBuild LiveOutputService::BuildNdiPlan(
    const std::vector<NdiProbeResult> &results, const QSet<QString> &activeNames)
{
    NdiPlanBuild build;
    for (const auto &result : results)
        AddNdiPlanFor(result, activeNames, build);
    return build;
}

void LiveOutputService::AddNdiPlanFor(const NdiProbeResult &result, const QSet<QString> &activeNames,
                                      NdiPlanBuild &build)
{
    if (!activeNames.contains(result.outputName))
        return;
    const QVariantMap oldStatus = ndiOutputStates_.value(result.outputName).toMap();
    const qulonglong count = result.provider ? result.provider->FramesSent() : 0;
    build.totalFrames += count;
    const bool ready = result.provider && result.bufferOk && result.enabled;
    const bool sending = ready && result.hasFrame;
    QVariantMap status;
    status.insert(QStringLiteral("frames"), QVariant::fromValue<qulonglong>(count));
    status.insert(QStringLiteral("sending"), sending);
    status.insert(QStringLiteral("warming"), !sending);
    status.insert(QStringLiteral("fps"), result.fps);
    build.states.insert(result.outputName, status);
    if (ready)
        build.plans.push_back(MakeNdiSendPlan(result));
    if (sending && build.firstSending.isEmpty())
        build.firstSending = result.outputName;
    if (!sending && build.firstWarming.isEmpty())
        build.firstWarming = result.outputName;
    AnnounceNdiSending(result, sending, oldStatus);
}

LiveOutputService::NdiSendPlan LiveOutputService::MakeNdiSendPlan(const NdiProbeResult &result)
{
    NdiSendPlan plan;
    plan.outputName = result.outputName;
    plan.provider = result.provider;
    plan.buffer = result.buffer;
    plan.senderName = result.senderName;
    plan.fps = result.fps;
    plan.nodeId = result.nodeId;
    if (!result.audioWired || plan.nodeId.isEmpty())
        return plan;
    plan.audio = ndiAudio_.value(plan.nodeId);
    if (!plan.audio) {
        plan.audio = std::make_shared<NdiAudio>();
        ndiAudio_.insert(plan.nodeId, plan.audio);
        listenOnNdiOutput(plan.nodeId, plan.audio);
    }
    return plan;
}

void LiveOutputService::AnnounceNdiSending(const NdiProbeResult &result, bool sending,
                                           const QVariantMap &oldStatus)
{
    const QString &name = result.outputName;
    if (sending && !oldStatus.value(QStringLiteral("sending")).toBool())
        qCWarning(lcNdiProgress, "LiveOutputService: NDI output '%s' is sending", qUtf8Printable(name));
    if (sending) {
        if (ndiSessionNames_.contains(name) || ndiAnnouncedNames_.contains(name))
            return;
        ndiAnnouncedNames_.insert(name);
        ndiSilentSinceMs_.remove(name);
        EventBus::instance().notify(
            QStringLiteral("NDI output '%1' is live — '%2' is on the network now.")
                .arg(name, result.senderName),
            QStringLiteral("success"), QStringLiteral("Outputs"),
            QStringLiteral("outputs.ndi.outputLive"));
        return;
    }
    if (ndiSessionNames_.contains(name) || ndiSilentWarned_.contains(name))
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto since = ndiSilentSinceMs_.constFind(name);
    if (since == ndiSilentSinceMs_.cend()) {
        ndiSilentSinceMs_.insert(name, now);
        return;
    }
    if (now - since.value() < kNdiSilentWarnMs)
        return;
    const QString reason = !result.bufferOk
        ? QStringLiteral("its frame buffer has not been created yet")
        : (!result.enabled
            ? QStringLiteral("its frame buffer is held off")
            : (!result.hasFrame
                ? QStringLiteral("no frame has been rendered for it yet")
                : QStringLiteral("its sender has not opened")));
    warnSilentNewOutput(name, reason);
}

void LiveOutputService::LogNdiProbeResults(const std::vector<NdiProbeResult> &results)
{
    // The probe runs on a timer for every feed (~10 Hz), so what counts as news is an
    // output's STATE — buffer/enabled/frame. Frames-sent grows on every tick, and gating
    // the whole line on it made this a per-tick writer for the life of a send. It is still
    // printed; it is just not part of the signature.
    QStringList probeParts;
    QStringList stateParts;
    for (const auto &result : results) {
        const qulonglong frames = result.provider ? result.provider->FramesSent() : 0;
        probeParts.append(QStringLiteral("%1{buffer=%2,enabled=%3,frame=%4,sent=%5}")
                              .arg(result.outputName).arg(result.bufferOk ? 1 : 0)
                              .arg(result.enabled ? 1 : 0).arg(result.hasFrame ? 1 : 0).arg(frames));
        stateParts.append(QStringLiteral("%1{buffer=%2,enabled=%3,frame=%4}")
                              .arg(result.outputName).arg(result.bufferOk ? 1 : 0)
                              .arg(result.enabled ? 1 : 0).arg(result.hasFrame ? 1 : 0));
    }
    const QString probeSignature = stateParts.join(QStringLiteral(" | "));
    if (probeSignature == ndiProbeLogSignature_)
        return;
    ndiProbeLogSignature_ = probeSignature;
    qInfo("LiveOutputService: NDI probe result: %s",
          qUtf8Printable(probeParts.join(QStringLiteral(" | "))));
}

void LiveOutputService::stopNdiFeed()
{
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

    clearNdiPlan();
    {
        std::lock_guard<std::mutex> book(ndiSendBookMutex_);
        ndiSendFailedNames_.clear();
        ndiFirstFrameNames_.clear();
        ndiSlowNames_.clear();
        ndiFastStreaks_.clear();
    }
    {
        // Off air: drop the reusable conversions. Senders keep whatever they are still reading alive.
        std::lock_guard<std::mutex> lock(ndiRgbaMutex_);
        ndiRgbaSlots_.clear();
        ndiConverted_.clear();
    }
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

void LiveOutputService::ensureNdiWorker()
{
    if (ndiThread_)
        return;
    if (!ndiPool_) {
        ndiPool_ = new QThreadPool(this);
        ndiPool_->setObjectName(QStringLiteral("ndi-send-pool"));
        // SMALL ON PURPOSE: the runtime compresses on its own threads, so this side only has to keep a
        // few distinct buffers converting and handing off at once.
        // VGR_NDI_POOL_THREADS lets the selftest measure the fan-out against a single send thread without a
        // rebuild: 1 = everything serialised on one worker, N = the ordinary pool.
        int threads = qBound(2, QThread::idealThreadCount() / 2, 4);
        if (qEnvironmentVariableIsSet("VGR_NDI_POOL_THREADS"))
            threads = qBound(1, qEnvironmentVariable("VGR_NDI_POOL_THREADS").toInt(), 8);
        ndiPool_->setMaxThreadCount(threads);
        qInfo("LiveOutputService: NDI send pool threads=%d%s", threads,
              qEnvironmentVariableIsSet("VGR_NDI_POOL_THREADS") ? " (forced by VGR_NDI_POOL_THREADS)" : "");
        ndiPool_->setExpiryTimeout(2000);
    }
    ndiThread_ = new QThread(this);
    ndiThread_->setObjectName("ndi-send");
    ndiWorker_ = new QObject();          // context only; lives on ndiThread_
    ndiWorker_->moveToThread(ndiThread_);
    connect(ndiThread_, &QThread::finished, ndiWorker_, [this] {
        qInfo("LiveOutputService: NDI worker teardown on the feed thread (its send clock exists=%d)",
              ndiSendTimer_ != nullptr);
        ndiWorker_->deleteLater();
    });
    // The feed thread paces and sends every NDI frame: High, so a loaded machine delays other work
    // before it makes the stream stutter.
    ndiThread_->start(QThread::HighPriority);
}

void LiveOutputService::clearNdiPlan()
{
    ndiClockArmed_ = false;   // no plan: the feed stops its own clock next beat
    setNdiPlan({});
}

void LiveOutputService::setNdiPlan(const std::vector<NdiSendPlan> &plans)
{
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
    QMetaObject::invokeMethod(ndiWorker_, [this] {
        if (!ndiSendTimer_) {
            ndiSendTimer_ = new QTimer(ndiWorker_);
            ndiSendTimer_->setTimerType(Qt::PreciseTimer);
            // 2 ms: the tick only has to land ON the deadline (60 Hz deadlines are 16.67 ms apart), and
            // a finer grid keeps the send within ~2 ms of it without spinning the thread.
            ndiSendTimer_->setInterval(2);
            connect(ndiSendTimer_, &QTimer::timeout, ndiWorker_, [this] { ndiWorkerTick(); });
        }
        if (!ndiSendTimer_->isActive())
            ndiSendTimer_->start();
    }, Qt::QueuedConnection);
}

void LiveOutputService::shutdownNdiWorker()
{
    if (!ndiThread_)
        return;
    // Let the pool finish first: a job holds a frame and a provider pointer the teardown below pulls
    // away from it.
    if (ndiPool_)
        ndiPool_->waitForDone(2000);
    {
        std::lock_guard<std::mutex> lock(ndiInFlightMutex_);
        ndiInFlight_.clear();
    }
    ndiThread_->quit();
    if (!ndiThread_->wait(2000)) {
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
    ndiSendTimer_ = nullptr;
    ndiWorker_ = nullptr;
    delete ndiThread_;      // `this`-parented, but explicit for clarity
    ndiThread_ = nullptr;
    ndiClockArmed_ = false;
    ndiPlanPublished_ = false;
}

// ---- GO LIVE WITH THE LOADED SHOW ----------------------------------------------------------------
// The wall shows a loaded show; pressing GO LIVE with nothing picked used to send a blank carrier, which is
// what cleared the monitors. The show's own slides are what the operator means — and their NEXT TIMER
// (nextTimer in the show file, read back as durationMs) is what the feed then advances on.
bool LiveOutputService::goLiveLoadedShow()
{
    ShowService &shows = ShowService::instance();
    // ALWAYS under either gate: a show left open by an earlier run (crash recovery offers one) would
    // otherwise be used in place of the selftest's own three timed slides, and the run would judge
    // whatever happened to be loaded instead of what it built.
    const bool selftestShow = qEnvironmentVariableIsSet("VGR_TIMED_TEST")
                              || qEnvironmentVariableIsSet("VGR_AUTOADVANCE_TEST");
    if (selftestShow) {
        // The proof needs a show WITH times on its slides. The selftest builds its own small one rather than
        // editing whatever the operator has open.
        // The labels follow the gate, so the log says which test built this show.
        const bool autoAdvanceRun = !qEnvironmentVariableIsSet("VGR_TIMED_TEST");
        const QString showLabel = autoAdvanceRun ? QStringLiteral("Auto-advance selftest")
                                                 : QStringLiteral("Timed advance selftest");
        const QString slideLabel = autoAdvanceRun ? QStringLiteral("Auto slide %1")
                                                  : QStringLiteral("Timed slide %1");
        shows.newShowDocument(showLabel);
        for (int i = 1; i <= 3; ++i) {
            QVariantMap block;
            block.insert(QStringLiteral("kind"), QStringLiteral("text"));
            block.insert(QStringLiteral("text"), slideLabel.arg(i));
            QVariantMap slide;
            slide.insert(QStringLiteral("title"), slideLabel.arg(i));
            slide.insert(QStringLiteral("blocks"), QVariantList{ block });
            slide.insert(QStringLiteral("nextTimer"), 2.0);
            shows.addSlide(slide);
        }
        qInfo("LiveOutputService: [TIMEDTEST] built a 3-slide show, 2 s each, for the advance proof");
    }
    if (!shows.hasShow()) {
        qInfo("LiveOutputService: go live has no show to take (hasShow=0)");
        return false;
    }
    QVariantList slides = shows.currentShow().value(QStringLiteral("slides")).toList();
    if (slides.isEmpty()) {
        qInfo("LiveOutputService: go live — the loaded show '%s' has no slides",
              qUtf8Printable(shows.showName()));
        return false;
    }
    if (qEnvironmentVariableIsSet("VGR_TIMED_TEST")) {
        // The selftest's own pacing: a slide with no NEXT TIMER gets a short one, so the advance can be
        // proven without editing the operator's show.
        for (int i = 0; i < slides.size(); ++i) {
            QVariantMap slide = slides.at(i).toMap();
            if (slide.value(QStringLiteral("nextTimer")).toDouble() <= 0.0) {
                slide.insert(QStringLiteral("nextTimer"), 2.0);
                slides[i] = slide;
            }
        }
    }
    qInfo("LiveOutputService: go live with the loaded show '%s' — %d slide(s)",
          qUtf8Printable(shows.showName()), int(slides.size()));
    goLiveWithSlides(shows.showName(), slides);
    return true;
}

// ---- TEST ROSTER: a second NDI output, only when the pool measurement asks for it -----------------
// The roster is persisted by OutputListModel, so the row is SELF-CLEANING: it is added on the way in and
// removed on the way out, and a run that was killed leaves the next start to clear it (a non-test start
// removes it too). It never depends on reaching a clean exit.
namespace {
QString NdiPoolTestName() { return QStringLiteral("NDI Pool Test"); }
int NdiPoolTestRow(const OutputListModel *model)
{
    if (!model)
        return -1;
    for (int i = 0; i < model->rowCount(); ++i) {
        if (model->getOutput(i).value(QStringLiteral("name")).toString() == NdiPoolTestName())
            return i;
    }
    return -1;
}
}  // namespace

void LiveOutputService::ensureNdiPoolTestRoster()
{
    auto *model = OutputListModel::instance();
    if (!model)
        return;
    const int existing = NdiPoolTestRow(model);
    if (!qEnvironmentVariableIsSet("VGR_NDI_POOL_TEST")) {
        if (existing >= 0) {
            model->removeOutput(existing);
            qInfo("LiveOutputService: removed a leftover NDI pool-test output");
        }
        return;
    }
    if (existing >= 0)
        return;                       // left by a killed run: reuse it rather than adding a second copy
    model->addOutput();
    const int row = model->rowCount() - 1;
    model->setKind(row, QStringLiteral("NDI"));
    model->renameOutput(row, QStringLiteral("NDI Pool Test"));
    model->setResolution(row, QStringLiteral("1920×1080"));
    model->setRefresh(row, QStringLiteral("60 Hz"));
    model->setEnabled(row, true);
    qInfo("LiveOutputService: [POOLTEST] added a second NDI output '%s' for the fan-out measurement",
          qUtf8Printable(NdiPoolTestName()));
}

void LiveOutputService::startNdiSendClock()
{
    ensureNdiPoolTestRoster();   // before the session names are read, so it is part of this feed
    ndiSessionNames_.clear();
    ndiAnnouncedNames_.clear();
    if (const auto *model = OutputListModel::instance()) {
        for (int i = 0; i < model->rowCount(); ++i) {
            const QVariantMap row = model->getOutput(i);
            if (row.value("kind").toString() == QLatin1String("NDI") && row.value("isEnabled").toBool())
                ndiSessionNames_.insert(row.value("name").toString());
        }
    }
    ensureNdiWorker();
    recheckNdiPlanOffThread();
}

void LiveOutputService::stopNdiSendClock()
{
    clearNdiPlan();
    ndiRateWindows_.clear();
    setNdiWarming(false, QString());   // off air: nothing is warming
}

// ---- AUTO-ADVANCE: the wall's timer button ---------------------------------
// One press and the on-air set slides by itself — the SAME step the wall's Next
// button takes (a slide in Slide mode, a line in Scroll mode), so every rule the
// manual step already honours applies: the reading walk, the passage hand-off,
// the end of the set.
QString LiveOutputService::autoAdvanceShowKey() const
{
    // THE SET THE TIMER BELONGS TO, never the slide that happens to be showing: onAirTitle_ is the
    // CURRENT SLIDE's title and changes on the first step, so comparing against it disarmed the timer
    // after one slide — "the timer isn't moving the slide". A show keeps its name for the whole set.
    // A PASSAGE BEING SCROLLED belongs to its reading set: that name is stable across every line and
    // every page of it, where the on-air title follows the page. Only in Scroll mode, because that is
    // where lines are what the timer advances.
    if (scrollMode_ && !lineSetName_.isEmpty() && !readingFlowText_.isEmpty())
        return lineSetName_;
    ShowService &shows = ShowService::instance();
    if (shows.hasShow())
        return shows.showName();
    return live_ ? onAirTitle_ : stagedName_;
}

bool LiveOutputService::autoAdvancing() const
{
    // The recorded set, while it is the one on air — not "a timer happens to be ticking", so the button
    // keeps its lit state between slides instead of flickering.
    return !autoAdvanceShow_.isEmpty() && live_ && autoAdvanceShowKey() == autoAdvanceShow_;
}

void LiveOutputService::toggleAutoAdvance()
{
    if (autoAdvancing()) {
        // Off again, and forgotten: the record goes, so this show does not resume by itself next time.
        qInfo("LiveOutputService: auto-advance forgotten for show '%s'", qUtf8Printable(autoAdvanceShow_));
        autoAdvanceShow_.clear();
        SettingsService::instance().setValue(QStringLiteral("shows.timedShow"), QString());
        emit autoAdvanceChanged();
        applyTimedAdvance();
        return;
    }
    // REMEMBERED WITH THE SHOW IT WAS TURNED ON FOR. That is the whole point of the record: the next show
    // must not slide by itself just because this one did.
    autoAdvanceShow_ = autoAdvanceShowKey();
    lastAutoLine_ = -1.0;   // a fresh session: its first step has no settled predecessor
    autoAdvanceLoaded_ = true;
    if (autoAdvanceShow_.isEmpty()) {
        EventBus::instance().notify(QStringLiteral("Nothing is on air to time yet — start the show first."),
                                    QStringLiteral("info"), QStringLiteral("Wall"));
        return;
    }
    SettingsService::instance().setValue(QStringLiteral("shows.timedShow"), autoAdvanceShow_);
    qInfo("LiveOutputService: auto-advance recorded for show '%s' — each slide uses its own NEXT TIMER",
          qUtf8Printable(autoAdvanceShow_));
    emit autoAdvanceChanged();
    applyTimedAdvance();
}

void LiveOutputService::applyTimedAdvance()
{
    if (!autoAdvanceLoaded_) {
        autoAdvanceLoaded_ = true;
        autoAdvanceShow_ = SettingsService::instance()
                               .value(QStringLiteral("shows.timedShow"))
                               .toString();
    }
    // ONLY THE RECORDED SET, and only while it is the one on air. The pace is each slide's own NEXT
    // TIMER, and the step is the wall's Next (armAutoAdvanceTimer / onAutoAdvanceTimeout).
    const bool on = autoAdvancing();

    // ---- THE ENGINE MUST NOT PACE THIS TOO -----------------------------------------------------
    // SetTimedAdvance(true) made PresentationRuntime::Tick step its OWN index on durationMs and publish
    // nothing back, so the picture stayed on the slide the operator pressed (the reported "the timer
    // isn't moving the slide") and a manual Next then skipped one, because the engine was already ahead.
    // The wall's timer below is the only pace; the engine is told off on every slide change, and only
    // the timed selftest — which exists to prove that engine path — turns it back on.
    pl::PresentationEngine::Instance().SetTimedAdvance(qEnvironmentVariableIsSet("VGR_TIMED_TEST"));

    armAutoAdvanceTimer();   // ALWAYS: the slide changed, so its own NEXT TIMER did too

    if (on == timedAdvanceApplied_)
        return;
    timedAdvanceApplied_ = on;
    qInfo("LiveOutputService: timed advance %s — on air '%s', recorded '%s'",
          on ? "ON" : "off",
          qUtf8Printable(onAirTitle_.isEmpty() ? QStringLiteral("—") : onAirTitle_),
          qUtf8Printable(autoAdvanceShow_));
}

// A slide's next timer as the SHOW has it, by slide id. The live content is a copy taken at GO LIVE, so
// this is the only place a timer set afterwards can be seen from.
double LiveOutputService::showNextTimerMsFor(const QString &slideId) const
{
    if (slideId.isEmpty())
        return 0.0;
    ShowService &shows = ShowService::instance();
    if (!shows.hasShow())
        return 0.0;
    const QVariantList slides = shows.currentShow().value(QStringLiteral("slides")).toList();
    for (const QVariant &v : slides) {
        const QVariantMap s = v.toMap();
        if (s.value(QStringLiteral("id")).toString() != slideId)
            continue;
        return qMax(0.0, s.value(QStringLiteral("nextTimer")).toDouble()) * 1000.0;
    }
    return 0.0;
}

// The Show Center applied its timer control: adopt the show's pace onto the set that is on air RIGHT NOW,
// instead of waiting for the next GO LIVE. A timer IS the instruction to advance, so the on-air show is
// recorded as the timed one; clearing every timer forgets that record again.
void LiveOutputService::adoptShowNextTimers()
{
    ShowService &shows = ShowService::instance();
    if (!live_ || !shows.hasShow())
        return;
    const QVariantList slides = shows.currentShow().value(QStringLiteral("slides")).toList();
    bool anyTimer = false;
    for (const QVariant &v : slides) {
        if (v.toMap().value(QStringLiteral("nextTimer")).toDouble() > 0.0) {
            anyTimer = true;
            break;
        }
    }
    const QString key = autoAdvanceShowKey();
    if (anyTimer) {
        if (autoAdvanceShow_ != key) {
            autoAdvanceShow_ = key;
            autoAdvanceLoaded_ = true;
            lastAutoLine_ = -1.0;   // a fresh timed session: its first step has no settled predecessor
            SettingsService::instance().setValue(QStringLiteral("shows.timedShow"), autoAdvanceShow_);
            emit autoAdvanceChanged();
        }
    } else if (autoAdvanceShow_ == key) {
        qInfo("LiveOutputService: every next timer cleared on '%s' — auto-advance forgotten",
              qUtf8Printable(key));
        autoAdvanceShow_.clear();
        SettingsService::instance().setValue(QStringLiteral("shows.timedShow"), QString());
        emit autoAdvanceChanged();
    }
    // Re-read the on-air slide's pace from the show and arm it: refreshOnAirSlide ends in applyTimedAdvance,
    // which is what starts (or stops) the timer.
    refreshOnAirSlide();
    qInfo("LiveOutputService: next timers adopted for show '%s' while on air: advancing=%d pace=%.0f ms",
          qUtf8Printable(key), autoAdvancing() ? 1 : 0, onAirDurationMs_);
}

void LiveOutputService::armAutoAdvanceTimer()
{
    if (!autoAdvanceTimer_) {
        autoAdvanceTimer_ = new QTimer(this);
        autoAdvanceTimer_->setSingleShot(true);
        connect(autoAdvanceTimer_, &QTimer::timeout, this, &LiveOutputService::onAutoAdvanceTimeout);
    }
    autoAdvanceTimer_->stop();
    // Not the recorded set on air, or a slide with no NEXT TIMER: it waits for a hand, and arming nothing
    // is what says so (a show with no times on its slides must never slide by itself).
    if (!autoAdvancing() || onAirDurationMs_ <= 0.0)
        return;
    autoAdvanceTimer_->start(qMax(100, int(onAirDurationMs_)));
}

bool LiveOutputService::stepForward()
{
    // Scroll: the walk steps one line and hands off at the passage's end. Any other mode: the next slide
    // (or passage). This is what the Next hotkey runs AND what the auto-advance timer runs.
    return scrollMode() ? lineStep(1) : next();
}

bool LiveOutputService::stepBackward()
{
    return scrollMode() ? lineStep(-1) : previous();
}

void LiveOutputService::onAutoAdvanceTimeout()
{
    if (!autoAdvancing())
        return;
    // THE SAME STEP THE NEXT BUTTON AND ITS HOTKEYS TAKE: one line in Scroll (handing off at the passage's
    // end), the next slide or passage in Slide. One stepping path, so Scroll mode comes for free.
    const bool scroll = scrollMode();
    // THE POSITION IS MEASURED SETTLED, NOT MID-STEP. lineStep starts a GLIDE (capped at 600 ms), so the
    // position read in the same tick it was asked to move is still the old one — the walk's own test reads
    // the previous press's settled value for exactly this reason. The timer's interval is the slide's NEXT
    // TIMER (2 s in the selftest, longer than any glide), so the value at the START of this tick is where
    // the PREVIOUS step landed: the delta is measured from it, and the first step has no predecessor.
    const qreal lineNow = readingAbsoluteLine();
    const qreal lineFrom = (lastAutoLine_ >= 0.0) ? lastAutoLine_ : lineNow;
    lastAutoLine_ = lineNow;
    const int serialFrom = lineStepSerial_;
    // THE SAME COMMAND THE NEXT HOTKEY RUNS, not a second copy of "what next means": Scroll steps a
    // line, Slide takes the next slide, and the timer and the key can never disagree about it.
    const bool moved = stepForward();
    if (!moved) {
        // End of the set: stop here. Re-arming on a step that cannot move would just tick at nothing.
        qInfo("LiveOutputService: auto-advance stopped at the end of '%s' (index %d, line %.2f)",
              qUtf8Printable(autoAdvanceShow_), onAirIndex_, lineNow);
        return;
    }
    // These labels are the slide being LEFT: the on-air snapshot lands a beat after the step, so the
    // post-step index and title are what the wall's own poll publishes, not what is readable here.
    qInfo("LiveOutputService: auto-advance step from index=%d title='%s' scroll=%d lines=%.3f serial=%d",
          onAirIndex_, qUtf8Printable(onAirSlideRaw_.value(QStringLiteral("title")).toString()),
          scroll ? 1 : 0, lineNow - lineFrom, lineStepSerial_ - serialFrom);
    armAutoAdvanceTimer();   // the next slide's own NEXT TIMER (next() re-arms too; this covers Scroll)
}

void LiveOutputService::pollTickRecheckNdi()
{
    if (!live_)
        return;
    connectToNdiOutputRoster();
    recheckNdiPlanOffThread();
}

void LiveOutputService::connectToNdiOutputRoster()
{
    auto *model = OutputListModel::instance();
    if (!model)
        return;
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
//  1. a RUNTIME RATE-CHANGE toast — the feed stepping DOWN to (about) half
//  2. a SUSTAINED DRIFT warning for a rate that is neither the configured
namespace {

// What an NDI row costs: its resolution times its rate — the quantity that decides whether this machine
// keeps up, and the one a user can trade away (a lower resolution or rate buys back headroom).
struct NdiRowLoad {
    QString name;
    int width = 1920;
    int height = 1080;
    float fps = 30.0f;
};

QVector<NdiRowLoad> EnabledNdiRowLoads()
{
    QVector<NdiRowLoad> rows;
    auto *model = OutputListModel::instance();
    if (!model)
        return rows;
    for (int i = 0; i < model->rowCount(); ++i) {
        const QVariantMap row = model->getOutput(i);
        if (row.value(QStringLiteral("kind")).toString() != QLatin1String("NDI"))
            continue;
        if (!row.value(QStringLiteral("isEnabled")).toBool())
            continue;
        NdiRowLoad load;
        load.name = row.value(QStringLiteral("name")).toString();
        const QString res = row.value(QStringLiteral("res")).toString();
        int sep = res.indexOf(QChar(0x00D7));   // "1920×1080"
        if (sep < 0)
            sep = res.indexOf(QLatin1Char('x'));
        if (sep > 0) {
            const int w = res.left(sep).trimmed().toInt();
            const int h = res.mid(sep + 1).trimmed().toInt();
            if (w > 0 && h > 0) {
                load.width = w;
                load.height = h;
            }
        }
        load.fps = bps::display::AcceptedOutputFrameRate(
            row.value(QStringLiteral("refresh")).toString().section(QLatin1Char(' '), 0, 0).toFloat());
        rows.push_back(load);
    }
    return rows;
}

QString FormatPixelRate(qint64 pixelsPerSecond)
{
    if (pixelsPerSecond >= 1000000000LL)
        return QStringLiteral("%1 Gpx/s").arg(QString::number(double(pixelsPerSecond) / 1e9, 'f', 2));
    return QStringLiteral("%1 Mpx/s").arg(QString::number(double(pixelsPerSecond) / 1e6, 'f', 0));
}

// "1920x1080@60" -> the pixel rate it stands for; 0 = no usable record.
qint64 PixelRateOf(const QString &record)
{
    const int at = record.indexOf(QLatin1Char('@'));
    const int x = record.indexOf(QLatin1Char('x'));
    if (at <= 0 || x <= 0 || x > at)
        return 0;
    const int w = record.left(x).toInt();
    const int h = record.mid(x + 1, at - x - 1).toInt();
    const int fps = record.mid(at + 1).toInt();
    if (w <= 0 || h <= 0 || fps <= 0)
        return 0;
    return qint64(w) * qint64(h) * qint64(fps);
}

// What EACH output held last session: "name|WxH@fps;name|WxH@fps". Per output, because the machine's total
// cannot say which of them is the one asking for more than it can carry.
QHash<QString, QString> ReadSustainedPerOutput()
{
    QHash<QString, QString> held;
    const QString stored = SettingsService::instance()
                               .value(QStringLiteral("outputs.ndi.sustainedPerOutput"))
                               .toString();
    const QStringList entries = stored.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &entry : entries) {
        const int bar = entry.indexOf(QLatin1Char('|'));
        if (bar <= 0 || bar + 1 >= entry.size())
            continue;
        held.insert(entry.left(bar), entry.mid(bar + 1));
    }
    return held;
}

void WriteSustainedPerOutput(const QHash<QString, QString> &held)
{
    QStringList entries;
    for (auto it = held.cbegin(); it != held.cend() && entries.size() < 24; ++it)
        entries.append(it.key() + QLatin1Char('|') + it.value());
    SettingsService::instance().setValue(QStringLiteral("outputs.ndi.sustainedPerOutput"),
                                         entries.join(QLatin1Char(';')));
}

// A CONCRETE setting this output's own record says it can carry: the best one that FITS, not the first
// rung of a ladder. Every (frame, rate) pair inside the record is scored by the pixels it asks for, and the
// largest that still fits wins — so 31 Mpx/s yields 1280×720 at 30 Hz, not a jump two sizes down to 640×360
// (which fits at 60 Hz but looks worse on a wall than 720p at half the rate).
QString SuggestLoad(const NdiRowLoad &row, qint64 capacity)
{
    static const int kRates[] = {50, 48, 30, 25, 24, 20, 15, 12, 10};
    static const int kSizes[][2] = {{1280, 720}, {960, 540}, {640, 360}};
    const int configured = int(std::llround(row.fps));
    int bestW = 0, bestH = 0, bestFps = 0;
    qint64 bestPps = 0;
    const auto consider = [&](int w, int h, int fps) {
        if (w == row.width && h == row.height && fps == configured)
            return;                                   // that IS the setting: no advice to give
        const qint64 pps = qint64(w) * qint64(h) * qint64(fps);
        if (double(pps) > double(capacity))
            return;                                   // would still be over the record
        if (pps > bestPps) {                          // ties keep the earlier, larger-frame candidate
            bestPps = pps;
            bestW = w;
            bestH = h;
            bestFps = fps;
        }
    };
    for (int rate : kRates)                            // same frame, slower: the least visible change
        consider(row.width, row.height, rate);
    for (const auto &size : kSizes) {
        consider(size[0], size[1], configured);        // smaller frame at the rate asked for
        for (int rate : kRates)
            consider(size[0], size[1], rate);
    }
    if (bestPps <= 0)
        return QString();
    return QStringLiteral("%1×%2 at %3 Hz").arg(bestW).arg(bestH).arg(bestFps);
}

}  // namespace

void LiveOutputService::RecordNdiSustainedLoad(qint64 pixelsPerSecond, const QString &detail)
{
    if (pixelsPerSecond <= 0)
        return;
    // What this machine PROVED it can carry, remembered for the next GO LIVE. Only a better session
    // replaces it: one that was cut short must not lower the bar for the next one.
    const QString stored = SettingsService::instance()
                               .value(QStringLiteral("outputs.ndi.sustainedLoad"))
                               .toString();
    const double bestMpx = stored.section(QLatin1Char(' '), 0, 0).toDouble();
    const double nowMpx = double(pixelsPerSecond) / 1e6;
    if (nowMpx <= bestMpx * 1.02)
        return;
    SettingsService::instance().setValue(
        QStringLiteral("outputs.ndi.sustainedLoad"),
        QStringLiteral("%1 Mpx/s · %2").arg(qint64(std::llround(nowMpx))).arg(detail));
    qInfo("LiveOutputService: NDI load this machine sustained — %s (%s), was %.0f Mpx/s",
          qUtf8Printable(FormatPixelRate(pixelsPerSecond)), qUtf8Printable(detail), bestMpx);
}

void LiveOutputService::warnIfNdiLoadExceedsSustained()
{
    if (qEnvironmentVariableIsSet("VGR_LOADCHECK_TEST")) {
        // The advice is a pure function of a record, so the selftest can exercise it without a roster that
        // is genuinely over budget: two fixed capacities, printed as the recommendation each produces.
        const NdiRowLoad probe{ QStringLiteral("probe 1080p60"), 1920, 1080, 60.0f };
        qInfo("LiveOutputService: [LOADCHECK] 1080p60 held 55 Mpx/s -> %s",
              qUtf8Printable(SuggestLoad(probe, 55400000)));   // 720p60 = 55.3 Mpx/s, so a slightly slower
        qInfo("LiveOutputService: [LOADCHECK] 1080p60 held 62 Mpx/s -> %s",
              qUtf8Printable(SuggestLoad(probe, 62000000)));   // 720p60 fits
        qInfo("LiveOutputService: [LOADCHECK] 1080p60 held 31 Mpx/s -> %s",
              qUtf8Printable(SuggestLoad(probe, 31000000)));   // 540p60 = 31.1 (just over) -> 540p30
    }
    const QVector<NdiRowLoad> rows = EnabledNdiRowLoads();
    if (rows.isEmpty())
        return;                       // no NDI configured: nothing to weigh
    qint64 configured = 0;
    QStringList parts;
    for (const auto &row : rows) {
        configured += qint64(row.width) * qint64(row.height) * qint64(std::llround(row.fps));
        parts.append(QStringLiteral("%1 at %2×%3 %4 Hz")
                         .arg(row.name).arg(row.width).arg(row.height).arg(std::llround(row.fps)));
    }
    // ---- A CONCRETE SETTING, PER OUTPUT -----------------------------------------------------------------
    // The aggregate figure below says the roster is over budget; this says WHICH output is and what to set it
    // to, from what that output ITSELF held last session — not from the machine's total.
    const QHash<QString, QString> held = ReadSustainedPerOutput();
    QStringList advice;
    for (const auto &row : rows) {
        const QString record = held.value(row.name);
        const qint64 capacity = PixelRateOf(record);
        if (capacity <= 0)
            continue;                   // no record for this output yet: nothing to advise from
        const qint64 want = qint64(row.width) * qint64(row.height) * qint64(std::llround(row.fps));
        if (double(want) <= double(capacity) * 1.10)
            continue;                   // within what this output itself held last session
        const QString suggested = SuggestLoad(row, capacity);
        if (suggested.isEmpty())
            continue;
        advice.append(QStringLiteral("%1 asks for %2 but held about %3 last session — set it to %4")
                          .arg(row.name, FormatPixelRate(want), FormatPixelRate(capacity), suggested));
    }
    if (!advice.isEmpty()) {
        const QString text =
            QStringLiteral("NDI load is over what these outputs carried last session: %1. "
                           "Lower them before GO LIVE, or expect dropped frames.")
                .arg(advice.join(QStringLiteral("; ")));
        qCWarning(lcNdiProgress, "LiveOutputService: NDI load advice — %s", qUtf8Printable(text));
        EventBus::instance().notify(text, QStringLiteral("warning"), QStringLiteral("Outputs"),
                                    QStringLiteral("outputs.ndi.loadAdvice"));
        return;
    }
    if (!held.isEmpty()) {
        // Nothing to advise, but say what the record is — so "within budget" is evidence, not silence.
        QStringList known;
        for (const auto &row : rows) {
            const QString record = held.value(row.name);
            if (record.isEmpty())
                continue;
            known.append(QStringLiteral("%1 held %2 (%3)")
                             .arg(row.name, record, FormatPixelRate(PixelRateOf(record))));
        }
        if (!known.isEmpty())
            qInfo("LiveOutputService: NDI load check — %s", qUtf8Printable(known.join(QStringLiteral("; "))));
    }

    const QString stored = SettingsService::instance()
                               .value(QStringLiteral("outputs.ndi.sustainedLoad"))
                               .toString();
    const qint64 sustained = qint64(stored.section(QLatin1Char(' '), 0, 0).toDouble() * 1e6);
    if (sustained <= 0) {
        // Say what was weighed even with nothing to compare: a silent check reads as "not run".
        qInfo("LiveOutputService: NDI load check — configured %s, no figure from a previous session yet",
              qUtf8Printable(FormatPixelRate(configured)));
        return;                       // first session on this machine: no figure to compare against
    }
    if (double(configured) <= double(sustained) * 1.10) {
        qInfo("LiveOutputService: NDI load check — configured %s is within what this machine sustained (%s)",
              qUtf8Printable(FormatPixelRate(configured)), qUtf8Printable(FormatPixelRate(sustained)));
        return;                       // within what it held last time
    }
    qCWarning(lcNdiProgress,
              "LiveOutputService: NDI load check — configured %s exceeds what this machine sustained (%s)",
              qUtf8Printable(FormatPixelRate(configured)), qUtf8Printable(FormatPixelRate(sustained)));
    EventBus::instance().notify(
        QStringLiteral("This machine carried about %1 of NDI last session; these %2 NDI outputs ask for %3 (%4). Expect dropped frames — lower a rate or a resolution, or take one output off air.")
            .arg(FormatPixelRate(sustained))
            .arg(rows.size())
            .arg(FormatPixelRate(configured))
            .arg(parts.join(QStringLiteral("; "))),
        QStringLiteral("warning"), QStringLiteral("Outputs"),
        QStringLiteral("outputs.ndi.loadWarning"));
}

void LiveOutputService::checkNdiFeedRate()
{
    if (!live_ || ndiOutputStates_.isEmpty()) {
        ndiRateWindows_.clear();
        return;
    }
    PruneNdiRateWindows();

    for (auto it = ndiOutputStates_.cbegin(); it != ndiOutputStates_.cend(); ++it) {
        const QString name = it.key();
        NdiRateWindow &w = ndiRateWindows_[name];
        double measured = 0.0;
        float configured = 0.0f;
        if (!MeasureNdiRateWindow(name, it.value().toMap(), w, &measured, &configured))
            continue;
        if (configured <= 0.0f) {
            w.tier = -1;
            w.configured = 0.0f;
            continue;
        }
        w.measured = measured;   // what it actually sent — the figure the per-output record is built from
        ReportNdiRateStep(name, measured, configured, w);
    }

    // WHAT THIS SESSION IS PROVING THE MACHINE CAN CARRY. An output counts only once it has held its
    // configured rate through a window (tier 0), so a roster that is already struggling cannot inflate
    // the figure the next GO LIVE is measured against.
    qint64 achieved = 0;
    int keptUp = 0;
    // Record what EACH output held, not just the total: the GO LIVE check needs to name the output that is
    // over budget and the setting it can carry, and only its own record can say that.
    QHash<QString, QString> held = ReadSustainedPerOutput();
    bool improved = false;
    for (const auto &row : EnabledNdiRowLoads()) {
        const auto window = ndiRateWindows_.constFind(row.name);
        if (window == ndiRateWindows_.cend() || !window->clock.isValid())
            continue;
        // An output counts only when it ACTUALLY sent at (near) its configured rate last window: the record
        // stores the measured rate, never the setting it was asked for.
        if (window->measured < double(row.fps) * 0.9)
            continue;
        const int heldFps = qMax(1, int(std::lround(window->measured)));
        const qint64 pps = qint64(row.width) * qint64(row.height) * qint64(heldFps);
        if (pps > qint64(double(PixelRateOf(held.value(row.name))) * 1.02)) {
            held.insert(row.name, QStringLiteral("%1x%2@%3").arg(row.width).arg(row.height).arg(heldFps));
            improved = true;
        }
        achieved += pps;
        ++keptUp;
    }
    if (keptUp > 0) {
        if (improved)
            WriteSustainedPerOutput(held);
        RecordNdiSustainedLoad(achieved, QStringLiteral("%1 NDI outputs").arg(keptUp));
    }
}

void LiveOutputService::PruneNdiRateWindows()
{
    for (auto it = ndiRateWindows_.begin(); it != ndiRateWindows_.end();) {
        if (!ndiOutputStates_.contains(it.key()))
            it = ndiRateWindows_.erase(it);
        else
            ++it;
    }
}

bool LiveOutputService::MeasureNdiRateWindow(const QString &name, const QVariantMap &status,
                                             NdiRateWindow &w, double *measured, float *configured)
{
    const bool sending = status.value(QStringLiteral("sending")).toBool();
    const qulonglong frames = status.value(QStringLiteral("frames")).toULongLong();
    *configured = status.value(QStringLiteral("fps")).toFloat();

    if (!sending) {
        w.clock.invalidate();
        w.frames = frames;
        return false;
    }
    if (!w.clock.isValid()) {
        w.frames = frames;
        w.clock.start();
        return false;
    }
    const qint64 elapsed = w.clock.elapsed();
    if (elapsed < kDriftWindowMs)
        return false;

    const qulonglong delta = frames >= w.frames ? frames - w.frames : 0;
    w.frames = frames;
    w.clock.restart();

    if (delta > 0) {
        w.noProgressWindows = 0;
    } else if (++w.noProgressWindows >= kDriftWindowsToWarn) {
        w.noProgressWindows = 0;
        ResetNdiSenderForRecovery(name);
    }

    *measured = double(delta) * 1000.0 / double(elapsed);
    return true;
}

void LiveOutputService::ResetNdiSenderForRecovery(const QString &name)
{
    ensureNdiWorker();
    const auto provider = ndiProviders_.value(name);
    if (!ndiWorker_ || !provider)
        return;
    QMetaObject::invokeMethod(ndiWorker_, [provider, name] {
        provider->ResetSender();
        qCWarning(lcNdiProgress,
                  "LiveOutputService: NDI output '%s' sent nothing for two windows; its sender was reset for automatic recovery",
                  qUtf8Printable(name));
    }, Qt::QueuedConnection);
}

void LiveOutputService::ReportNdiRateStep(const QString &name, double measured, float configured,
                                          NdiRateWindow &w)
{
    const bool configuredChanged = w.configured > 0.0f && !qFuzzyCompare(configured, w.configured);
    w.configured = configured;

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

    if (tier != 2) {
        w.slowWindows = 0;
        w.warned = false;
        return;
    }
    if (++w.slowWindows < kDriftWindowsToWarn || w.warned)
        return;
    w.warned = true;
    EventBus::instance().notify(
        QStringLiteral("NDI output '%1' is running at about %2 fps but is set to %3 Hz — the feed can't keep up. Lower the refresh rate or check the machine's load.")
            .arg(name).arg(qRound(measured)).arg(qRound(configured)),
        QStringLiteral("warning"), QStringLiteral("Outputs"),
        QStringLiteral("outputs.framerate.drift"));
}

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
