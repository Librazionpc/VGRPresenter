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

// Linear pan/balance taper: centre = full gain BOTH sides (a bus fader's
// predictable contract — the level you set is the level each channel gets),
// one side fading linearly to zero at the extreme.
void PanGains(const VolumeControl &busVol, float &left, float &right)
{
    const float p = std::clamp(static_cast<float>(busVol.pan), -1.0f, 1.0f);
    left = p <= 0.0f ? 1.0f : 1.0f - p;
    right = p >= 0.0f ? 1.0f : 1.0f + p;
}

// The per-channel ROUTING mask a source row's matrix writes for ONE bus: bit
// <c> set = that row's channel c is routed into this bus
// ("uiRouteMask:<busNodeId>", written by AudioInputListModel::pushRoutingToGraph).
// 0 = NO MASK = the whole source feeds the bus, which is what an older graph, a
// bus-dialog route or a video row carries — the honest default, never a silence
// nobody asked for.
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

// One node's per-side trim, index 0 = L, 1 = R. Written on a node's meta by
// the L/R controls — a SOURCE row's channel buttons
// (AudioInputListModel::setChannelGain) and a BUS's (BusListModel::
// setChannelGain) both use this same key, so the mixer needs no knowledge of
// which kind of card wrote it. Absent = unity (an older graph).
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

// What feeds one BUS plane, in the order the mix should consider it: the
// plane's own upstream sources (the routing board's edges on that plane) plus —
// for an AUDIO plane — the sources of its paired VIDEO plane ("bus:N#a" ↔
// "bus:N#v", the pairing BusListModel derives both ids from). A video row
// routed into a "both"/video bus carries its own audio (a media file's track,
// NDI's embedded audio); that audio belongs to this bus's mix, and the app's
// bridge binds those rows to stream taps exactly as it binds an audio row's
// device. SumSource contributes nothing for a node it has no tap bound for, so
// a camera or a silent NDI feed costs one map lookup and stays silent.
std::vector<std::string> FeedsForBus(const ProductionGraph &graph, const std::string &planeId)
{
    std::vector<std::string> out = graph.Upstream(planeId);
    if (!planeId.ends_with("#a"))
        return out;
    const std::string twin = planeId.substr(0, planeId.size() - 2) + "#v";
    for (const std::string &up : graph.Upstream(twin))
        if (std::find(out.begin(), out.end(), up) == out.end())
            out.push_back(up);
    return out;
}

// One-pole envelope follower toward `target`, attack/release in seconds —
// the standard compressor/limiter/gate smoothing shape (fast attack, slower
// release keeps gain reduction from "pumping" on every transient).
float SmoothEnvelope(float current, float target, float timeSeconds, uint32_t outRateHz)
{
    if (timeSeconds <= 0.0 || outRateHz == 0)
        return target;
    const float coeff = std::exp(-1.0f / (static_cast<float>(timeSeconds) * static_cast<float>(outRateHz)));
    return target + (current - target) * coeff;
}
} // namespace

// Applies node's ProcessingStage chain to one already-resampled stereo
// frame, in place. Called from SumSource per output frame, after resampling
// and before the branch gain/pan (dynamics → fader, the normal channel-
// strip order). Gain/Gate/Compressor/Limiter/Delay are real; Eq/Custom
// ("reverb") are documented passthroughs — see the header's scope note for
// why (the UI's own value-domain mapping doesn't give Eq a usable filter
// parameter yet, and reverb is out of scope for this pass).
void AudioMixer::ApplyProcessing(const NodeInfo &node, SourceDspState &state,
                                 float &l, float &r, uint32_t outRateHz)
{
    for (const ProcessingStage &stage : node.processing) {
        switch (stage.kind) {
        case ProcessKind::Gain: {
            // amount = dB directly (this stage's own UI convention).
            const float g = DbToLinear(static_cast<float>(stage.amount));
            l *= g; r *= g;
            break;
        }
        case ProcessKind::Gate: {
            // amount = normalized 0..1 over a -80..0 dBFS threshold (the
            // UI's "noiseGate" mapping). Smoothed gain, not a hard on/off —
            // an instant gate clicks every time it opens/closes.
            const float thresholdLin = DbToLinear(static_cast<float>(stage.amount) * 80.0f - 80.0f);
            const float level = std::max(std::abs(l), std::abs(r));
            const float target = level >= thresholdLin ? 1.0f : 0.0f;
            state.gateGain = SmoothEnvelope(state.gateGain, target,
                                            target > state.gateGain ? 0.005f : 0.08f, outRateHz);
            l *= state.gateGain; r *= state.gateGain;
            break;
        }
        case ProcessKind::Compressor: {
            // amount = normalized 0..1 over ratio 1..10:1 (the UI's own
            // mapping). Fixed -18dBFS threshold — the UI has one knob
            // (ratio) for this stage, no separate threshold control yet.
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
            // amount = normalized 0..1 over ceiling -24..0 dBFS (the UI's
            // mapping). A limiter is a compressor with a very high ratio
            // and a fast release — brick-wall the peak, recover quickly.
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
            // amount = seconds directly (the UI's mapping). A real delay
            // line, mixed 50/50 with the dry signal — a simple, predictable
            // "echo" contract; the UI has no separate wet/dry control for
            // this stage yet.
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
        case ProcessKind::Eq:
        case ProcessKind::ColorCorrect:   // video-only stage kind; never reaches an audio chain
        case ProcessKind::Denoise:
        case ProcessKind::Custom:
        default:
            break;   // documented passthrough — see the header's scope note
        }
    }
}

// One route's contribution: the source's REAL captured frames, drained from
// its tap FIFO (resampled to outRateHz when the tap's own rate differs —
// found in review: this used to be silently skipped), through its
// ProcessingStage chain, then the branch gain (bus plane volume × the
// source node's own fader, either side's mute) and the bus pan, summed into
// the interleaved buffer. Returns true when it actually contributed.
//
// PRECONDITION: called only from Render(), which already holds mutex_ for
// the whole callback — this reads deviceTable_/dspState_ directly, relying
// on that. Found in review as an actual bug, not just a lock-span concern:
// this used to take mutex_ itself too, which — called from inside Render's
// already-held lock on the same non-recursive std::mutex — is a genuine
// self-deadlock on the very first render with any routed source, not a
// theoretical risk.
bool AudioMixer::SumSource(const std::string &sourceNodeId, const std::string &busNodeId,
                           const VolumeControl &busVol,
                           float busGainL, float busGainR,
                           float *out, uint32_t frames, uint32_t channels, uint32_t outRateHz)
{
    if (!graph_ || !taps_)
        return false;

    // Which capture device feeds this graph source? The routing pump binds
    // it; an unbound source contributes nothing (no device, no sound — never
    // a synthetic carrier).
    auto devIt = deviceTable_.find(sourceNodeId);
    if (devIt == deviceTable_.end())
        return false;
    const uint32_t deviceId = devIt->second;

    const auto node = graph_->GetNode(sourceNodeId);
    if (!node.ok())
        return false;   // the route's source row vanished; the edge is cut anyway

    if (busVol.mute || node.value().volume.mute)
        return false;
    const float gain = DbToLinear(busVol.gainDb) * DbToLinear(node.value().volume.gainDb);
    if (gain <= 0.0f)
        return false;

    // The row's per-channel routing to THIS bus: a mask's bit c gates channel c,
    // so a source the matrix routed on one side only stays on that side here.
    // (No mask — the common case — leaves both sides open.)
    const unsigned routeMask = RouteMaskFor(node.value(), busNodeId);
    if (routeMask != 0u && (routeMask & 0x3u) == 0u)
        return false;   // nothing routed into this bus at all
    const float routeOpenL = (routeMask == 0u || (routeMask & 0x1u)) ? 1.0f : 0.0f;
    const float routeOpenR = (routeMask == 0u || (routeMask & 0x2u)) ? 1.0f : 0.0f;

    // THE CARRIER: real captured samples. A drained FIFO yields fewer
    // frames than asked — only what arrived is mixed (the remainder stays
    // silence; a stalled tap goes quiet, it never repeats its last buffer).
    const auto fmt = taps_->TapFormat(deviceId);
    if (fmt.channels == 0 || fmt.sampleRateHz == 0)
        return false;   // no live tap behind this source
    const float sourceGainL = SourceChannelGain(node.value(), 0);
    const float sourceGainR = fmt.channels == 1
        ? sourceGainL : SourceChannelGain(node.value(), 1);
    if (sourceGainL <= 0.0f && sourceGainR <= 0.0f)
        return false;   // both source channels are explicitly closed

    float panL = 1.0f, panR = 1.0f;
    PanGains(busVol, panL, panR);
    // The BUS's own per-side trim folds in beside the pan — the bus card's
    // L/R buttons write it, so a closed side of the bus is silent here even
    // while the source feeding it is wide open, which is the whole point of
    // the bus having its own L/R at all.
    const float sampleGainL = gain * panL * busGainL * routeOpenL;
    const float sampleGainR = gain * panR * busGainR * routeOpenR;

    SourceDspState &dsp = dspState_[sourceNodeId];   // creates on first use; fine, small and rare

    bool any = false;
    if (fmt.sampleRateHz == outRateHz || outRateHz == 0) {
        // Same rate (the common case — WASAPI shared mode usually
        // normalizes every device to one engine rate): drain 1:1, no
        // resampling cost.
        scratch_.resize(static_cast<size_t>(frames) * fmt.channels);
        const size_t got = taps_->ReadTapAudio(deviceId, scratch_.data(), frames);
        for (size_t f = 0; f < got; ++f) {
            const size_t base = f * fmt.channels;
            float l = scratch_[base];
            float r = fmt.channels == 1 ? scratch_[base] : scratch_[base + 1];
            ApplyProcessing(node.value(), dsp, l, r, outRateHz);
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

    // Different rates: real linear-interpolation resample (found in review
    // — this used to be silently skipped entirely, playing a mismatched-
    // rate tap at the wrong pitch/speed with no defense at all). Need
    // ~frames*ratio+2 INPUT frames to produce `frames` OUTPUT frames.
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
        ApplyProcessing(node.value(), dsp, l, r, outRateHz);
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
            // A channel with no signal must go dark, especially when its
            // source-side L/R gain was closed. Holding its old peak made a
            // muted side keep animating after the signal was cut.
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
    if (!graph_ || frameCount == 0 || channelCount == 0 || out == nullptr) {
        PublishSilence();      // the meter follows the real thing down to zero
        return false;
    }

    // The routing pump mutates the device table on the GUI side; the mix
    // holds the same lock (a sub-millisecond hold against a tiny setter).
    // SumSource/ApplyProcessing run INSIDE this lock (see SumSource's own
    // precondition comment) — they must never lock mutex_ themselves.
    const std::lock_guard<std::mutex> lock(mutex_);

    bool anything = false;
    memset(out, 0, sizeof(float) * static_cast<size_t>(frameCount) * channelCount);

    // Every enabled AUDIO BUS plane with sources feeding it contributes,
    // mixed by its own VolumeControl. (Downstream bus→output edges are
    // topology the production planner walks; playout mixes the planes
    // directly — the same content, one hop fewer.)
    for (const auto &[id, node] : graph_->Nodes()) {
        if (node.kind != NodeKind::Bus || node.signalType != SignalType::Audio)
            continue;

        if (!node.enabled)
            continue;
        // The bus plane's own per-side trims (its node meta — the bus card's
        // L/R buttons). Both closed means this bus contributes nothing, the
        // same early-out a fully-closed source takes in SumSource.
        const float busGainL = SourceChannelGain(node, 0);
        const float busGainR = SourceChannelGain(node, 1);
        if (busGainL <= 0.0f && busGainR <= 0.0f)
            continue;
        for (const auto &upstream : FeedsForBus(*graph_, id))
            anything |= SumSource(upstream, id, node.volume, busGainL, busGainR,
                                  out, frameCount, channelCount, sampleRateHz);
    }
    if (!anything) {
        // A muted/empty graph IS silence — the platform emits it, and the
        // program meter has to say so rather than freeze at the last peak.
        PublishSilence();
        return false;
    }

    // Master scale + safety ceiling: never let a pathological stack clip.
    const float master = masterGain_.load(std::memory_order_relaxed);
    const size_t total = static_cast<size_t>(frameCount) * channelCount;
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

} // namespace bps::production
