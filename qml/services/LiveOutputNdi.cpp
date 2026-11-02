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
                                           std::shared_ptr<const pr::Frame> frame,
                                           QString senderName, float fps)
{
    auto *provider = static_cast<bps::display::NdiDisplayProvider *>(providerVoid);
    auto &svc = LiveOutputService::instance();
    if (!provider || !frame || frame->empty())
        return;

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
        return;
    }
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

void LiveOutputService::ensureNdiWorker()
{
    if (ndiThread_)
        return;
    ndiThread_ = new QThread(this);
    ndiThread_->setObjectName("ndi-send");
    ndiWorker_ = new QObject();          // context only; lives on ndiThread_
    ndiWorker_->moveToThread(ndiThread_);
    connect(ndiThread_, &QThread::finished, ndiWorker_, [this] {
        qInfo("LiveOutputService: NDI worker teardown on the feed thread (its send clock exists=%d)",
              ndiSendTimer_ != nullptr);
        ndiWorker_->deleteLater();
    });
    ndiThread_->start();
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
            ndiSendTimer_->setInterval(5);
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

void LiveOutputService::startNdiSendClock()
{
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
        ReportNdiRateStep(name, measured, configured, w);
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