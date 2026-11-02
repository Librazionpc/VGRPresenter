#include "modules/production/AudioMixer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bps::production {
void AudioMixer::Attach(ProductionGraph *graph)
{
    graph_ = graph;
}

void AudioMixer::AttachTaps(bps::platform::IAudio *taps)
{
    taps_ = taps;
}

void AudioMixer::BindSourceDevice(const std::string &sourceNodeId, uint32_t waveinId)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (waveinId == UINT32_MAX)
        deviceTable_.erase(sourceNodeId);
    else
        deviceTable_[sourceNodeId] = waveinId;
}

void AudioMixer::UnbindSource(const std::string &sourceNodeId)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    deviceTable_.erase(sourceNodeId);
}

namespace {
float DbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

bool PlaysLocally(const NodeInfo &node)
{
    const auto it = node.meta.find("playsLocally");
    return it != node.meta.end() && it->second == "1";
}

void PanGains(const VolumeControl &busVol, float &left, float &right)
{
    const float p = std::clamp(static_cast<float>(busVol.pan), -1.0f, 1.0f);
    left = p <= 0.0f ? 1.0f : 1.0f - p;
    right = p >= 0.0f ? 1.0f : 1.0f + p;
}

unsigned RouteMaskFor(const NodeInfo &node, const std::string &busNodeId)
{
    const auto it = node.meta.find("uiRouteMask:" + busNodeId);
    if (it == node.meta.end())
        return 0u;
    try {
        return static_cast<unsigned>(std::stoul(it->second));
    } catch (const std::exception &) {
        return 0u;   // unreadable mask reads as "no mask" (the whole source)
    }
}

float SourceChannelGain(const NodeInfo &node, uint32_t channel)
{
    const std::string key = "uiChannelGain" + std::to_string(channel);
    const auto it = node.meta.find(key);
    if (it == node.meta.end())
        return 1.0f;  // older graphs default to unity
    char *end = nullptr;
    const float value = std::strtof(it->second.c_str(), &end);
    if (end == it->second.c_str() || !std::isfinite(value))
        return 1.0f;
    return std::clamp(value, 0.0f, 1.0f);
}

std::vector<std::string> UpstreamOf(const GraphSnapshot &snap, const std::string &to)
{
    std::vector<std::string> out;
    for (const auto &[from, tos] : snap.edges)
        if (tos.count(to) != 0)
            out.push_back(from);
    return out;
}

std::vector<std::string> FeedsForBus(const GraphSnapshot &snap, const std::string &planeId)
{
    std::vector<std::string> out = UpstreamOf(snap, planeId);
    if (!planeId.ends_with("#a"))
        return out;
    const std::string twin = planeId.substr(0, planeId.size() - 2) + "#v";
    for (const std::string &up : UpstreamOf(snap, twin))
        if (std::find(out.begin(), out.end(), up) == out.end())
            out.push_back(up);
    return out;
}

float SmoothEnvelope(float current, float target, float timeSeconds, uint32_t outRateHz)
{
    if (timeSeconds <= 0.0 || outRateHz == 0)
        return target;
    const float coeff = std::exp(-1.0f / (static_cast<float>(timeSeconds) * static_cast<float>(outRateHz)));
    return target + (current - target) * coeff;
}
} // namespace

void AudioMixer::ApplyProcessing(const NodeInfo &node, SourceDspState &state,
                                 float &l, float &r, uint32_t outRateHz)
{
    size_t eqBand = 0;      // index of this Eq stage among the chain's Eq stages
    size_t reverbStage = 0; // index of this reverb stage among the chain's reverb stages
    size_t denoiseStage = 0; // index of this denoise stage among the chain's denoise stages
    for (const ProcessingStage &stage : node.processing) {
        switch (stage.kind) {
        case ProcessKind::Gain: {
            // amount = dB directly (this stage's own UI convention).
            const float g = DbToLinear(static_cast<float>(stage.amount));
            l *= g; r *= g;
            break;
        }
        case ProcessKind::Gate: {
            const float thresholdLin = DbToLinear(static_cast<float>(stage.amount) * 80.0f - 80.0f);
            const float level = std::max(std::abs(l), std::abs(r));
            const float target = level >= thresholdLin ? 1.0f : 0.0f;
            state.gateGain = SmoothEnvelope(state.gateGain, target,
                                            target > state.gateGain ? 0.005f : 0.08f, outRateHz);
            l *= state.gateGain; r *= state.gateGain;
            break;
        }
        case ProcessKind::Compressor: {
            const float ratio = 1.0f + static_cast<float>(stage.amount) * 9.0f;
            constexpr float thresholdDb = -18.0f;
            const float level = std::max(std::abs(l), std::abs(r));
            const float levelDb = level > 1e-6f ? 20.0f * std::log10(level) : -120.0f;
            const float targetGainDb = levelDb > thresholdDb
                ? (thresholdDb + (levelDb - thresholdDb) / ratio) - levelDb : 0.0f;
            const float targetGain = DbToLinear(targetGainDb);
            state.compEnvelope = SmoothEnvelope(state.compEnvelope, targetGain,
                                                targetGain < state.compEnvelope ? 0.010f : 0.150f,
                                                outRateHz);
            l *= state.compEnvelope; r *= state.compEnvelope;
            break;
        }
        case ProcessKind::Limiter: {
            const float ceilingLin = DbToLinear(static_cast<float>(stage.amount) * 24.0f - 24.0f);
            const float level = std::max(std::abs(l), std::abs(r));
            const float target = level > ceilingLin && level > 1e-6f ? ceilingLin / level : 1.0f;
            state.limiterEnvelope = SmoothEnvelope(state.limiterEnvelope, target,
                                                   target < state.limiterEnvelope ? 0.001f : 0.050f,
                                                   outRateHz);
            l *= state.limiterEnvelope; r *= state.limiterEnvelope;
            break;
        }
        case ProcessKind::Delay: {
            const double seconds = std::max(0.0, stage.amount);
            const size_t lineFrames = outRateHz > 0
                ? static_cast<size_t>(seconds * outRateHz) + 1 : 1;
            if (state.delaySeconds != seconds || state.delayLine.size() != lineFrames * 2) {
                state.delayLine.assign(lineFrames * 2, 0.0f);
                state.delayPos = 0;
                state.delaySeconds = seconds;
            }
            if (!state.delayLine.empty()) {
                const size_t lineLen = state.delayLine.size() / 2;
                const float wetL = state.delayLine[state.delayPos * 2];
                const float wetR = state.delayLine[state.delayPos * 2 + 1];
                state.delayLine[state.delayPos * 2] = l;
                state.delayLine[state.delayPos * 2 + 1] = r;
                state.delayPos = (state.delayPos + 1) % lineLen;
                l = l * 0.5f + wetL * 0.5f;
                r = r * 0.5f + wetR * 0.5f;
            }
            break;
        }
        case ProcessKind::Eq: {
            // One parametric band per Eq stage (frequency, gain, Q), applied to each
            // channel with its own filter state.
            const size_t band = eqBand++;
            if (state.eq.size() <= band)
                state.eq.resize(band + 1);
            auto &bandState = state.eq[band];
            bandState.left.Configure(outRateHz, stage.freqHz, stage.gainDb, stage.q);
            bandState.right.Configure(outRateHz, stage.freqHz, stage.gainDb, stage.q);
            l = bandState.left.Process(l);
            r = bandState.right.Process(r);
            break;
        }
        case ProcessKind::Custom: {
            // "reverb": amount is the wet mix (0..1) of a stereo reverb. Any other
            // custom stage has no audio meaning here and passes through.
            if (stage.custom != "reverb")
                break;
            const size_t index = reverbStage++;
            if (state.reverb.size() <= index)
                state.reverb.resize(index + 1);
            auto &reverb = state.reverb[index];
            reverb.left.Configure(outRateHz);
            reverb.right.Configure(outRateHz);
            const float wet = std::clamp(static_cast<float>(stage.amount), 0.0f, 1.0f);
            const float wetL = reverb.left.Process(l);
            const float wetR = reverb.right.Process(r);
            l += wet * wetL;
            r += wet * wetR;
            break;
        }
        case ProcessKind::Denoise: {
            // amount = depth 0..1: how far the signal near the learned noise floor is
            // pulled down. Each denoise stage keeps its own floor and envelope.
            const size_t index = denoiseStage++;
            if (state.denoise.size() <= index)
                state.denoise.resize(index + 1);
            state.denoise[index].Process(l, r, stage.amount, outRateHz);
            break;
        }
        case ProcessKind::ColorCorrect:   // video-only stage kind; never reaches an audio chain
        default:
            break;
        }
    }
}

bool AudioMixer::SumSource(const GraphSnapshot &snap,
                           const std::string &sourceNodeId, const std::string &busNodeId,
                           const VolumeControl &busVol,
                           float busGainL, float busGainR,
                           float *out, uint32_t frames, uint32_t channels, uint32_t outRateHz)
{
    if (!taps_)
        return false;

    auto devIt = deviceTable_.find(sourceNodeId);
    if (devIt == deviceTable_.end())
        return false;
    const uint32_t deviceId = devIt->second;

    const auto nodeIt = snap.nodes.find(sourceNodeId);
    if (nodeIt == snap.nodes.end())
        return false;   // the route's source row vanished; the edge is cut anyway
    const NodeInfo &node = nodeIt->second;

    if (busVol.mute || node.volume.mute)
        return false;
    const float gain = DbToLinear(busVol.gainDb) * DbToLinear(node.volume.gainDb);
    if (gain <= 0.0f)
        return false;

    const unsigned routeMask = RouteMaskFor(node, busNodeId);
    if (routeMask != 0u && (routeMask & 0x3u) == 0u)
        return false;   // nothing routed into this bus at all
    const float routeOpenL = (routeMask == 0u || (routeMask & 0x1u)) ? 1.0f : 0.0f;
    const float routeOpenR = (routeMask == 0u || (routeMask & 0x2u)) ? 1.0f : 0.0f;

    const auto fmt = taps_->TapFormat(deviceId);
    if (fmt.channels == 0 || fmt.sampleRateHz == 0)
        return false;   // no live tap behind this source
    const float sourceGainL = SourceChannelGain(node, 0);
    const float sourceGainR = fmt.channels == 1
        ? sourceGainL : SourceChannelGain(node, 1);
    if (sourceGainL <= 0.0f && sourceGainR <= 0.0f)
        return false;   // both source channels are explicitly closed

    float panL = 1.0f, panR = 1.0f;
    PanGains(busVol, panL, panR);
    const float sampleGainL = gain * panL * busGainL * routeOpenL;
    const float sampleGainR = gain * panR * busGainR * routeOpenR;

    SourceDspState &dsp = dspState_[sourceNodeId];   // creates on first use; fine, small and rare

    bool any = false;
    if (fmt.sampleRateHz == outRateHz || outRateHz == 0) {
        scratch_.resize(static_cast<size_t>(frames) * fmt.channels);
        const size_t got = taps_->ReadTapAudio(deviceId, scratch_.data(), frames);
        for (size_t f = 0; f < got; ++f) {
            const size_t base = f * fmt.channels;
            float l = scratch_[base];
            float r = fmt.channels == 1 ? scratch_[base] : scratch_[base + 1];
            ApplyProcessing(node, dsp, l, r, outRateHz);
            l *= sourceGainL;
            r *= sourceGainR;
            if (channels == 1)
                out[f] += (l * sampleGainL + r * sampleGainR) * 0.5f;
            else {
                out[f * channels] += l * sampleGainL;
                out[f * channels + 1] += r * sampleGainR;
            }
            any = true;
        }
        return any;
    }

    const double ratio = static_cast<double>(fmt.sampleRateHz) / static_cast<double>(outRateHz);
    const size_t neededIn = static_cast<size_t>(frames * ratio) + 2;
    scratch_.resize(neededIn * fmt.channels);
    const size_t got = taps_->ReadTapAudio(deviceId, scratch_.data(), neededIn);
    if (got < 2)
        return false;   // not enough real samples yet to interpolate even one output frame

    for (uint32_t f = 0; f < frames; ++f) {
        const double srcPos = f * ratio;
        const size_t i0 = static_cast<size_t>(srcPos);
        if (i0 + 1 >= got)
            break;   // ran out of real samples this chunk — leave the rest silent, never repeat
        const float t = static_cast<float>(srcPos - i0);
        float l0, r0, l1, r1;
        if (fmt.channels == 1) {
            l0 = r0 = scratch_[i0];
            l1 = r1 = scratch_[i0 + 1];
        } else {
            l0 = scratch_[i0 * fmt.channels];       r0 = scratch_[i0 * fmt.channels + 1];
            l1 = scratch_[(i0 + 1) * fmt.channels]; r1 = scratch_[(i0 + 1) * fmt.channels + 1];
        }
        float l = l0 + (l1 - l0) * t;
        float r = r0 + (r1 - r0) * t;
        ApplyProcessing(node, dsp, l, r, outRateHz);
        l *= sourceGainL;
        r *= sourceGainR;
        if (channels == 1)
            out[f] += (l * sampleGainL + r * sampleGainR) * 0.5f;
        else {
            out[f * channels] += l * sampleGainL;
            out[f * channels + 1] += r * sampleGainR;
        }
        any = true;
    }
    return any;
}

void AudioMixer::PublishLevels(const float *out, uint32_t frames, uint32_t channels)
{
    const uint32_t n = std::min<uint32_t>(channels, 2);
    float peaks[2] = {0.0f, 0.0f};
    float sums[2] = {0.0f, 0.0f};
    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < n; ++c) {
            const float v = out[static_cast<size_t>(f) * channels + c];
            const float a = std::fabs(v);
            if (a > peaks[c]) peaks[c] = a;
            sums[c] += v * v;
        }
    }
    const std::lock_guard<std::mutex> lock(levelMutex_);
    for (uint32_t c = 0; c < 2; ++c) {
        const float held = programLevel_.peaks[c] * kPeakHold;
        if (c < n && peaks[c] > 0.0005f) {
            programLevel_.peaks[c] = std::max(peaks[c], held);
            programLevel_.rms[c] = frames > 0 ? std::sqrt(sums[c] / static_cast<float>(frames))
                                              : programLevel_.rms[c] * kPeakHold;
        } else {
            programLevel_.peaks[c] = 0.0f;
            programLevel_.rms[c] = 0.0f;
        }
    }
    programLevel_.channelCount = n;
    programLevel_.framesRendered += frames;
}

void AudioMixer::PublishSilence()
{
    const std::lock_guard<std::mutex> lock(levelMutex_);
    for (uint32_t c = 0; c < 2; ++c) {
        programLevel_.peaks[c] = 0.0f;
        programLevel_.rms[c] = 0.0f;
    }
}

AudioMixer::ProgramLevels AudioMixer::ProgramLevel() const
{
    const std::lock_guard<std::mutex> lock(levelMutex_);
    return programLevel_;
}

bool AudioMixer::Render(float *out, uint32_t frameCount, uint32_t channelCount, uint32_t sampleRateHz)
{
    if (out == nullptr || frameCount == 0 || channelCount == 0)
        return false;
    // One snapshot for the whole chunk: the speakers and every tap see the same graph.
    const std::shared_ptr<const GraphSnapshot> snap =
        graph_ ? graph_->PublishedSnapshot() : nullptr;
    const uint64_t startFrame = framesRendered_.fetch_add(frameCount, std::memory_order_relaxed);
    bool mixed = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (snap) {
            mixed = MixInto(*snap, out, frameCount, channelCount, sampleRateHz);
        } else {
            PublishSilence();   // no graph attached: the meter follows the real thing to zero
        }
        if (!mixed)
            std::memset(out, 0, sizeof(float) * static_cast<size_t>(frameCount) * channelCount);
        formatRate_.store(sampleRateHz, std::memory_order_relaxed);
        formatChannels_.store(channelCount, std::memory_order_relaxed);
        NotifyTaps(snap.get(), frameCount, channelCount, sampleRateHz, startFrame);
    }
    return mixed;
}

bool AudioMixer::RenderOutput(std::string_view outputNodeId, float *out, uint32_t frameCount,
                              uint32_t channelCount, uint32_t sampleRateHz)
{
    if (out == nullptr || frameCount == 0 || channelCount == 0 || outputNodeId.empty())
        return false;
    const size_t total = static_cast<size_t>(frameCount) * channelCount;
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::shared_ptr<const GraphSnapshot> snap =
        graph_ ? graph_->PublishedSnapshot() : nullptr;
    if (!snap) {
        std::memset(out, 0, sizeof(float) * total);
        return false;
    }
    BuildPlanes(*snap, frameCount, channelCount, sampleRateHz);
    const bool any = MixSelected(*snap, outputNodeId, out, frameCount, channelCount);
    const float master = masterGain_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < total; ++i) {
        float s = any ? out[i] * master : 0.0f;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        out[i] = s;
    }
    return any;
}

AudioMixer::MixFormat AudioMixer::Format() const
{
    return MixFormat{formatRate_.load(std::memory_order_relaxed),
                     formatChannels_.load(std::memory_order_relaxed)};
}

uint64_t AudioMixer::AddMixTap(std::string outputNodeId, MixTap tap)
{
    std::lock_guard<std::mutex> lock(tapMutex_);
    const uint64_t id = nextTapId_++;
    mixTaps_.push_back(MixTapEntry{id, std::move(outputNodeId), std::move(tap)});
    return id;
}

void AudioMixer::RemoveMixTap(uint64_t id)
{
    std::lock_guard<std::mutex> lock(tapMutex_);
    mixTaps_.erase(std::remove_if(mixTaps_.begin(), mixTaps_.end(),
                                  [id](const MixTapEntry &entry) { return entry.id == id; }),
                   mixTaps_.end());
}

void AudioMixer::NotifyTaps(const GraphSnapshot *snap, uint32_t frames, uint32_t channels,
                            uint32_t rateHz, uint64_t startFrame)
{
    std::lock_guard<std::mutex> lock(tapMutex_);
    const size_t samples = static_cast<size_t>(frames) * channels;
    if (tapScratch_.size() < samples)
        tapScratch_.resize(samples);
    for (const auto &entry : mixTaps_) {
        const std::string_view target = entry.nodeId.empty() ? std::string_view(kSpeakersOutputId)
                                                              : std::string_view(entry.nodeId);
        if (snap == nullptr || !MixSelected(*snap, target, tapScratch_.data(), frames, channels))
            std::memset(tapScratch_.data(), 0, sizeof(float) * samples);
        entry.tap(tapScratch_.data(), frames, channels, rateHz, startFrame);
    }
}

bool AudioMixer::BusIsHeard(const GraphSnapshot &snap, const std::string &busId)
{
    const auto wired = snap.edges.find(busId);
    if (wired != snap.edges.end() && !wired->second.empty())
        return true;
    // The caller holds mutex_; tapMutex_ is taken inside it, the one order the mixer uses.
    const std::lock_guard<std::mutex> lock(tapMutex_);
    for (const auto &entry : mixTaps_)
        if (entry.nodeId == busId)
            return true;
    return false;
}

void AudioMixer::BuildPlanes(const GraphSnapshot &snap, uint32_t frameCount,
                             uint32_t channelCount, uint32_t sampleRateHz)
{
    const size_t total = static_cast<size_t>(frameCount) * channelCount;
    planeCount_ = 0;
    for (const auto &[id, node] : snap.nodes) {
        if (node.kind != NodeKind::Bus || node.signalType != SignalType::Audio)
            continue;
        if (!node.enabled)
            continue;
        if (!BusIsHeard(snap, id))
            continue;
        const float busGainL = SourceChannelGain(node, 0);
        const float busGainR = SourceChannelGain(node, 1);
        if (busGainL <= 0.0f && busGainR <= 0.0f)
            continue;
        if (planeCount_ == planes_.size())
            planes_.emplace_back();
        PlaneBuffer &plane = planes_[planeCount_];
        plane.id = id;
        plane.samples.assign(total, 0.0f);
        plane.any = false;
        for (const auto &upstream : FeedsForBus(snap, id))
            plane.any |= SumSource(snap, upstream, id, node.volume, busGainL, busGainR,
                                   plane.samples.data(), frameCount, channelCount, sampleRateHz);
        if (plane.any)
            ++planeCount_;
    }
}

bool AudioMixer::MixInto(const GraphSnapshot &snap, float *out, uint32_t frameCount,
                         uint32_t channelCount, uint32_t sampleRateHz)
{
    if (frameCount == 0 || channelCount == 0 || out == nullptr) {
        PublishSilence();      // the meter follows the real thing down to zero
        return false;
    }

    const size_t total = static_cast<size_t>(frameCount) * channelCount;
    BuildPlanes(snap, frameCount, channelCount, sampleRateHz);

    // The speaker endpoint is the sum of the planes wired to it.
    bool any = MixSelected(snap, kSpeakersOutputId, out, frameCount, channelCount);

    for (size_t p = 0; p < planeCount_; ++p) {
        const PlaneBuffer &plane = planes_[p];
        if (!plane.any)
            continue;
        const auto wired = snap.edges.find(plane.id);
        if (wired == snap.edges.end())
            continue;
        bool onSpeakers = false;
        bool onLocalOutput = false;
        for (const std::string &to : wired->second) {
            if (to == kSpeakersOutputId) {
                onSpeakers = true;
                break;
            }
            const auto target = snap.nodes.find(to);
            if (target != snap.nodes.end() && target->second.kind == NodeKind::Output &&
                PlaysLocally(target->second))
                onLocalOutput = true;
        }
        if (onSpeakers || !onLocalOutput)
            continue;
        any = true;
        for (size_t s = 0; s < total; ++s)
            out[s] += plane.samples[s];
    }

    if (!any) {
        // A muted/empty graph IS silence — the platform emits it, and the
        // program meter has to say so rather than freeze at the last peak.
        PublishSilence();
        return false;
    }

    // Master scale + safety ceiling: never let a pathological stack clip.
    const float master = masterGain_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < total; ++i) {
        float s = out[i] * master;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        out[i] = s;
    }
    // Publish AFTER the master scale and the ceiling: the meter shows what is
    // actually going out, clipped, not the pre-limiter sum.
    PublishLevels(out, frameCount, channelCount);
    return true;
}

bool AudioMixer::MixSelected(const GraphSnapshot &snap, std::string_view nodeId, float *dst,
                             uint32_t frameCount, uint32_t channelCount)
{
    const size_t total = static_cast<size_t>(frameCount) * channelCount;
    std::memset(dst, 0, sizeof(float) * total);
    const auto target = snap.nodes.find(std::string(nodeId));
    if (target != snap.nodes.end() && !target->second.enabled)
        return false;
    bool any = false;
    // Render thread only: planes_ and planeCount_ belong to the current chunk's MixInto.
    // Compared in place: this runs once per tap per chunk, so it builds no string.
    for (size_t i = 0; i < planeCount_; ++i) {
        const PlaneBuffer &plane = planes_[i];
        // A bus target means that bus alone; any other target means the buses wired to it.
        bool heard = std::string_view(plane.id) == nodeId;
        if (!heard) {
            const auto wired = snap.edges.find(plane.id);
            if (wired != snap.edges.end())
                for (const std::string &to : wired->second)
                    if (std::string_view(to) == nodeId) {
                        heard = true;
                        break;
                    }
        }
        if (!heard)
            continue;
        for (size_t s = 0; s < total; ++s)
            dst[s] += plane.samples[s];
        any = true;
    }
    return any;
}
} // namespace bps::production