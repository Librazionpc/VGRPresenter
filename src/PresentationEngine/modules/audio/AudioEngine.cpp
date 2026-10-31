#include "modules/audio/AudioEngine.hpp"

#include "core/logging/Logger.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <optional>
#include <string>

namespace bps::audio {

namespace {

constexpr const char *kModule = "AudioEngine";
// How often the control thread looks at the graph for source/device changes.
constexpr auto kReconcileInterval = std::chrono::milliseconds(250);
// How long an unresolved device name waits before the roster is read again.
constexpr auto kRosterRetry = std::chrono::seconds(2);

std::string Normalize(std::string_view text)
{
    size_t first = 0;
    size_t last = text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
        ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
        --last;
    std::string out(text.substr(first, last - first));
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The waveIn number of the capture endpoint whose roster name matches. Windows ids
// are "wavein:<n>"; a name that matches nothing, or matches a non-input, resolves to
// nothing rather than a guess.
std::optional<uint32_t> ResolveWaveIn(const std::vector<platform::AudioDeviceInfo> &roster,
                                      std::string_view name)
{
    const std::string want = Normalize(name);
    if (want.empty())
        return std::nullopt;
    for (const auto &dev : roster) {
        if (!dev.isInput || Normalize(dev.name) != want)
            continue;
        constexpr std::string_view prefix = "wavein:";
        if (dev.id.rfind(prefix, 0) != 0)
            return std::nullopt;
        try {
            return static_cast<uint32_t>(std::stoul(dev.id.substr(prefix.size())));
        } catch (const std::exception &) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// True when an output that is playing on its OWN endpoint is wired to this bus.
// With the program off the air those streams are the only path left that can be
// heard — the speakers and the plays-locally copy are drawn by the program render
// that is not running — so a bus counts only when one of them holds it.
bool BusPlaysOnAnEndpoint(const production::GraphSnapshot &snap, const std::string &busId,
                          const std::set<std::string> &endpoints)
{
    if (endpoints.empty())
        return false;
    const auto downstream = snap.edges.find(busId);
    if (downstream == snap.edges.end())
        return false;
    for (const std::string &to : downstream->second)
        if (endpoints.count(to) != 0)
            return true;
    return false;
}

} // namespace

AudioEngine &AudioEngine::Instance()
{
    static AudioEngine instance;
    return instance;
}

Result<void> AudioEngine::Initialize()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    initialized_ = true;
    return Ok();
}

Result<void> AudioEngine::Start()
{
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!initialized_)
            return Error::Make(Err::InvalidState, kModule, "Initialize() first");
        // PROGRAM, not running_: a Stop() that left an output playing on its own
        // endpoint keeps running_ up, so asking that flag would report "already
        // started" and never put the program back on the air.
        if (programPlaying_.load())
            return Ok();
        if (starting_ || stopping_)
            return Error::Make(Err::InvalidState, kModule, "lifecycle operation already in progress");
        starting_ = true;
    }

    auto &audio = platform::PlatformAccessor::Get().Audio();
    EnsureSpeakersOutput();
    mixer_.Attach(&production::ProductionEngine::Instance().Graph());
    mixer_.AttachTaps(&audio);
    auto started = audio.StartRender(
        [this](float *frames, uint32_t count, uint32_t channels, uint32_t rateHz) {
            return mixer_.Render(frames, count, channels, rateHz);
        });
    if (!started.ok()) {
        // No output device: the mix still runs, on a software clock that keeps real
        // time, so a recording of the program still gets its audio. The reason is logged.
        Logger::Instance().Warning("no output device (" + started.error().message +
                                       "); the mix runs on a software clock", kModule);
        softClockOn_.store(true);
    } else {
        Logger::Instance().Info("playout started on the default output device", kModule);
    }

    running_.store(true);
    programPlaying_.store(true);
    if (!started.ok())
        softClock_ = std::thread([this] { SoftClockLoop(); });
    // REUSED, never replaced: a stop that left an output playing on its own
    // endpoint keeps this thread (and its capture) alive, and assigning over a
    // joinable std::thread terminates the process.
    {
        const std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!control_.joinable()) {
            control_ = std::thread([this] { ControlLoop(); });
            controlId_ = control_.get_id();
        }
    }
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        starting_ = false;
    }
    return Ok();
}

Result<void> AudioEngine::Stop()
{
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!running_.load())
            return starting_ ? Result<void>(Error::Make(
                                   Err::InvalidState, kModule,
                                   "cannot stop while start is in progress")) : Ok();
        if (stopping_)
            return Error::Make(Err::InvalidState, kModule, "stop already in progress");
        stopping_ = true;
        programPlaying_.store(false);
    }
    wake_.notify_all();

    // 1. THE PROGRAM COMES OFF THE AIR. Its render thread joins inside
    //    StopRender; a program that never got a device is paced by the software
    //    clock instead, which watches programPlaying_ (NOT running_ — that stays
    //    up while an output is still playing, and a loop watching it would never
    //    end here).
    if (softClock_.joinable())
        softClock_.join();
    (void)platform::PlatformAccessor::Get().Audio().StopRender();

    // 2. IS ANYTHING ELSE STILL PLAYING? An output with an endpoint of its own is
    //    a stream the PAL keeps for the ROUTING, not for playout (see
    //    WindowsAudio::renderStreams_), driven by that device's own thread and
    //    mixing straight from the graph (AudioMixer::RenderOutput). Tearing it
    //    down with the program is what used to silence an output's device the
    //    moment playout stopped, so it keeps playing — and so do the capture
    //    bindings the buses behind it need.
    bool outputsPlaying = false;
    {
        const std::lock_guard<std::mutex> lock(outputAudioMutex_);
        outputsPlaying = !outputStreams_.empty();
    }
    if (outputsPlaying) {
        Logger::Instance().Info(
            "playout stopped; the outputs on their own endpoints keep playing", kModule);
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            stopping_ = false;
        }
        return Ok();
    }

    // 3. NOTHING LEFT TO HEAR: the whole engine comes down. Every render thread is
    //    stopped before the mixer is released, so nothing reads the mixer (or a
    //    tap) while the graph is let go.
    running_.store(false);
    wake_.notify_all();
    if (control_.joinable())
        control_.join();
    StopAllOutputAudio();
    ReleaseAll();
    mixer_.Attach(nullptr);
    Logger::Instance().Info("playout stopped", kModule);
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        controlId_ = std::thread::id{};
        stopping_ = false;
    }
    return Ok();
}

void AudioEngine::EnsureSpeakersOutput()
{
    auto &graph = production::ProductionEngine::Instance().Graph();
    if (graph.HasNode(production::kSpeakersOutputId))
        return;   // an existing speaker output keeps whatever wiring the user made

    production::OutputConfig config;
    config.caps.supportsVideo = false;
    config.caps.supportsAudio = true;
    auto added = graph.AddOutput(production::kSpeakersOutputId, "Speakers",
                                 production::SignalType::Audio, config);
    if (!added.ok()) {
        Logger::Instance().Warning("could not create the speaker output: " + added.error().message,
                                   kModule);
        return;
    }
    // Created once: every audio bus plays through the speakers, as it always did.
    // Buses added later are wired like any other output, so they stay silent until routed.
    for (const auto &id : graph.NodeIds()) {
        const auto node = graph.GetNode(id);
        if (!node.ok() || node.value().kind != production::NodeKind::Bus ||
            node.value().signalType != production::SignalType::Audio)
            continue;
        (void)graph.Connect(id, production::kSpeakersOutputId, production::SignalType::Audio);
    }
}

void AudioEngine::SetSourceDevice(const std::string &sourceId, const std::string &deviceName)
{
    std::lock_guard<std::mutex> lock(namesMutex_);
    if (deviceName.empty())
        deviceNames_.erase(sourceId);
    else
        deviceNames_[sourceId] = deviceName;
}

void AudioEngine::SoftClockLoop()
{
    // 10 ms of stereo 48 kHz per chunk. The mix is rendered up to the wall-clock time,
    // so the sample clock keeps real time even though no device paces it.
    constexpr uint32_t kRate = 48000;
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kChunk = 480;
    std::vector<float> chunk(static_cast<size_t>(kChunk) * kChannels);
    const auto origin = std::chrono::steady_clock::now();
    uint64_t rendered = 0;
    // programPlaying_, not running_: the clock paces the PROGRAM, so it ends with
    // it even while an output's own endpoint keeps running_ up (a loop watching
    // running_ there would spin forever and Stop's join would hang).
    while (programPlaying_.load()) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
        const auto due = static_cast<uint64_t>(elapsed * kRate);
        while (rendered + kChunk <= due) {
            mixer_.Render(chunk.data(), kChunk, kChannels, kRate);
            rendered += kChunk;
        }
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(5), [this] { return !programPlaying_.load(); });
    }
    softClockOn_.store(false);
}

Result<void> AudioEngine::Shutdown()
{
    auto stopped = Stop();
    if (!stopped.ok()) return stopped;
    // Stop() leaves an output's own endpoint playing on purpose (its lifetime is
    // the routing's, not playout's). Shutdown is the one place that takes them all
    // down, so no device is left open behind an engine that is gone.
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (starting_ || stopping_)
            return Error::Make(Err::InvalidState, kModule, "lifecycle operation already in progress");
        stopping_ = true;
        programPlaying_.store(false);
    }
    running_.store(false);
    wake_.notify_all();
    if (softClock_.joinable())
        softClock_.join();
    if (control_.joinable())
        control_.join();
    if (platform::PlatformAccessor::Installed())
        (void)platform::PlatformAccessor::Get().Audio().StopRender();
    StopAllOutputAudio();
    ReleaseAll();
    mixer_.Attach(nullptr);
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        controlId_ = std::thread::id{};
        stopping_ = false;
        initialized_ = false;
    }
    return stopped;
}

bool AudioEngine::Playing() const
{
    return platform::PlatformAccessor::Get().Audio().Rendering();
}

Result<void> AudioEngine::StartOutputAudio(const std::string &outputNodeId, uint32_t deviceId)
{
    if (outputNodeId.empty())
        return Error::Make(Err::InvalidArgument, kModule,
                           "an output node id is required to play an output's audio");

    uint64_t streamId = 0;
    {
        const std::lock_guard<std::mutex> lock(outputAudioMutex_);
        const auto it = outputStreams_.find(outputNodeId);
        if (it != outputStreams_.end()) {
            // Already playing there: the routing re-ran, not a new stream.
            if (outputDevices_[outputNodeId] == deviceId)
                return Ok();
            // A different device: the SAME stream id replaces the old client in the
            // PAL, which joins the previous thread before reopening.
            streamId = it->second;
        } else {
            streamId = nextOutputStreamId_++;
            outputStreams_[outputNodeId] = streamId;
        }
        outputDevices_[outputNodeId] = deviceId;
    }

    // The callback runs on that device's own WASAPI thread; the mixer serialises it
    // against the program render, and renders at THIS device's rate.
    const std::string node = outputNodeId;
    auto started = platform::PlatformAccessor::Get().Audio().StartRenderStream(
        streamId,
        [this, node](float *frames, uint32_t count, uint32_t channels, uint32_t rateHz) {
            return mixer_.RenderOutput(node, frames, count, channels, rateHz);
        },
        deviceId);
    if (!started.ok()) {
        const std::lock_guard<std::mutex> lock(outputAudioMutex_);
        outputStreams_.erase(outputNodeId);
        outputDevices_.erase(outputNodeId);
        Logger::Instance().Warning("output '" + outputNodeId +
                                       "' cannot play on its audio endpoint: " +
                                       started.error().message,
                                   kModule);
        return started;
    }
    Logger::Instance().Info("output '" + outputNodeId +
                                "' plays on its own audio endpoint (stream " +
                                std::to_string(streamId) + ")",
                            kModule);
    return Ok();
}

void AudioEngine::StopOutputAudio(const std::string &outputNodeId)
{
    uint64_t streamId = 0;
    {
        const std::lock_guard<std::mutex> lock(outputAudioMutex_);
        const auto it = outputStreams_.find(outputNodeId);
        if (it == outputStreams_.end())
            return;
        streamId = it->second;
        outputStreams_.erase(it);
        outputDevices_.erase(outputNodeId);
    }
    (void)platform::PlatformAccessor::Get().Audio().StopRenderStream(streamId);
    Logger::Instance().Info("output '" + outputNodeId + "' no longer plays on its own endpoint",
                            kModule);
    // It may have been the last thing keeping the engine up with the program off
    // the air (see Stop): the capture behind it has nobody left to play for.
    MaybeFinishStop();
}

void AudioEngine::StopAllOutputAudio()
{
    std::vector<uint64_t> ids;
    {
        const std::lock_guard<std::mutex> lock(outputAudioMutex_);
        for (const auto &entry : outputStreams_)
            ids.push_back(entry.second);
        outputStreams_.clear();
        outputDevices_.clear();
    }
    auto &audio = platform::PlatformAccessor::Get().Audio();
    for (const uint64_t id : ids)
        (void)audio.StopRenderStream(id);
}

// THE OTHER END OF THE STOP IN Stop(). That function keeps an output's own
// endpoint — and with it the control thread, the capture bindings and the graph —
// alive while the program is off the air, because that stream can still be heard.
// When the LAST of those streams closes there is nothing left that could hear a
// source, so the capture devices are released and the control thread winds up: the
// same tail Stop() runs when it finds no output playing.
void AudioEngine::MaybeFinishStop()
{
    {
        const std::lock_guard<std::mutex> lock(outputAudioMutex_);
        if (!outputStreams_.empty())
            return;   // another output still plays out of this engine
    }
    if (programPlaying_.load())
        return;       // the program is playing: this is not the stopped state at all
    {
        const std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!running_.load() || starting_ || stopping_)
            return;   // a Stop()/Start() owns the teardown, or it already ran
        if (controlId_ == std::this_thread::get_id())
            return;   // defensive: the control thread never closes a stream here
        stopping_ = true;
    }
    running_.store(false);
    wake_.notify_all();
    if (control_.joinable())
        control_.join();
    const bool hadBindings = !boundDevice_.empty();   // the control thread is joined
    ReleaseAll();
    mixer_.Attach(nullptr);
    if (hadBindings)
        Logger::Instance().Info("no output is playing; capture released", kModule);
    {
        const std::lock_guard<std::mutex> lock(lifecycleMutex_);
        controlId_ = std::thread::id{};
        stopping_ = false;
    }
}

bool AudioEngine::OutputAudioRunning(const std::string &outputNodeId) const
{
    const std::lock_guard<std::mutex> lock(outputAudioMutex_);
    return outputStreams_.find(outputNodeId) != outputStreams_.end();
}

production::AudioMixer::ProgramLevels AudioEngine::ProgramLevels() const
{
    return mixer_.ProgramLevel();
}

void AudioEngine::ControlLoop()
{
    while (running_.load()) {
        Reconcile();
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wake_.wait_for(lock, kReconcileInterval, [this] { return !running_.load(); });
    }
}

void AudioEngine::Reconcile()
{
    const auto snap = production::ProductionEngine::Instance().Graph().PublishedSnapshot();
    if (!snap)
        return;

    // WHICH BUSES ARE BEING PLAYED RIGHT NOW. On the air, every enabled audio bus
    // is: the program sums the ones wired to the speakers and to a plays-locally
    // display, and a mix tap (NDI, recording, a stream) reads any of them off that
    // same render. With the program STOPPED none of that is true, and the only
    // buses that can still be heard are the ones an output's own endpoint is
    // playing (StartOutputAudio → AudioMixer::RenderOutput). Capturing for
    // anything else would hold a device open for silence.
    const bool programOn = programPlaying_.load();
    std::set<std::string> endpoints;
    if (!programOn) {
        const std::lock_guard<std::mutex> lock(outputAudioMutex_);
        for (const auto &entry : outputStreams_)
            endpoints.insert(entry.first);
    }

    // The sources that reach a bus somebody is playing, each with the device name
    // it asks for.
    std::map<std::string, std::string> wanted;
    for (const auto &[from, tos] : snap->edges) {
        const auto src = snap->nodes.find(from);
        if (src == snap->nodes.end()
            || src->second.kind != production::NodeKind::Source
            || src->second.signalType != production::SignalType::Audio)
            continue;

        bool reachesBus = false;
        for (const auto &to : tos) {
            const auto bus = snap->nodes.find(to);
            if (bus != snap->nodes.end()
                && bus->second.kind == production::NodeKind::Bus
                && bus->second.signalType == production::SignalType::Audio
                && bus->second.enabled
                && (programOn || BusPlaysOnAnEndpoint(*snap, to, endpoints))) {
                reachesBus = true;
                break;
            }
        }
        if (!reachesBus)
            continue;

        std::string label;
        {
            std::lock_guard<std::mutex> lock(namesMutex_);
            const auto named = deviceNames_.find(from);
            if (named != deviceNames_.end())
                label = named->second;
        }
        if (label.empty()) {
            const auto device = src->second.meta.find("device");
            if (device != src->second.meta.end())
                label = device->second;
        }
        if (Normalize(label).empty())
            continue;
        wanted[from] = label;
    }

    // Sources that stopped qualifying, or changed device, let go of their capture.
    for (auto it = boundLabel_.begin(); it != boundLabel_.end();) {
        const auto keep = wanted.find(it->first);
        if (keep != wanted.end() && keep->second == it->second) {
            ++it;
            continue;
        }
        const std::string id = it->first;
        it = boundLabel_.erase(it);
        Unbind(id);
    }

    // New sources: resolve the device name and open its capture.
    const auto now = std::chrono::steady_clock::now();
    std::vector<platform::AudioDeviceInfo> roster;
    bool rosterRead = false;
    for (const auto &[id, label] : wanted) {
        if (boundLabel_.count(id) != 0)
            continue;
        if (!rosterRead) {
            if (now < nextRetry_)
                break;
            roster = platform::PlatformAccessor::Get().Audio().Enumerate();
            rosterRead = true;
            nextRetry_ = now + kRosterRetry;
        }
        const auto dev = ResolveWaveIn(roster, label);
        if (!dev) {
            Logger::Instance().Warning("no capture device named '" + label
                                           + "' for source " + id, kModule);
            continue;
        }
        auto tap = platform::PlatformAccessor::Get().Audio().StartInputMeter(*dev, label);
        if (!tap.ok()) {
            Logger::Instance().Warning("could not open '" + label + "' for source " + id
                                           + ": " + tap.error().message, kModule);
            continue;
        }
        mixer_.BindSourceDevice(id, *dev);
        boundLabel_[id] = label;
        boundDevice_[id] = *dev;
        tapUsers_[*dev].insert(id);
    }
}

void AudioEngine::Unbind(const std::string &sourceId){
    mixer_.UnbindSource(sourceId);
    const auto dev = boundDevice_.find(sourceId);
    if (dev == boundDevice_.end())
        return;
    const uint32_t device = dev->second;
    boundDevice_.erase(dev);

    const auto users = tapUsers_.find(device);
    if (users == tapUsers_.end())
        return;
    users->second.erase(sourceId);
    if (users->second.empty()) {
        tapUsers_.erase(users);
        (void)platform::PlatformAccessor::Get().Audio().StopInputMeter(device);
    }
}

void AudioEngine::ReleaseAll()
{
    std::vector<std::string> ids;
    ids.reserve(boundDevice_.size());
    for (const auto &[id, device] : boundDevice_)
        ids.push_back(id);
    for (const auto &id : ids)
        Unbind(id);
    boundLabel_.clear();
    tapUsers_.clear();
    boundDevice_.clear();
}

} // namespace bps::audio
