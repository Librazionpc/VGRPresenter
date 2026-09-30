#pragma once

// AudioMixer — the ENGINE half of the sound you hear: a callback-driven
// mixer that consumes the ProductionGraph's audio topology (source nodes →
// bus planes carrying VolumeControl { gainDb, mute, pan, balance } and
// ProcessingStage chains) and fills the platform render callback's frames.
//
// This is the piece that was missing entirely: ProductionEngine "never
// renders pixels, plays audio", so the buses were a topology data structure
// with nothing consuming them into real playback. The mixer is the consumer.
// One call site: the app's AudioEngineBridge hands the mixer's
// RenderCallback to platform Audio().StartRender(); the WASAPI render
// thread then pulls frames through here at the endpoint's mix rate.
//
// Scope: VOLUME/GAIN/MUTE + pan/balance, plus real per-source ProcessingStage
// DSP for Gain/Gate/Compressor/Limiter/Delay (envelope-follower dynamics +
// a real delay line — see ApplyProcessing in the .cpp). Eq and Custom
// ("reverb") stages are read but PASSED THROUGH, not applied — the UI's own
// value-domain mapping for "eq" stores a band COUNT (1..5), not a usable
// filter frequency/gain, so there is nothing to apply yet without a UI
// change first; reverb is deliberately out of scope for this pass. Both are
// documented here, not silently dropped.

#include "modules/production/ProductionGraph.hpp"
#include "modules/production/ProductionTypes.hpp"
#include "platform/IAudio.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bps::production {

class AudioMixer final {
public:
    AudioMixer() = default;

    // Binds the mixer to a graph. Retargeting mid-render is fine (the next
    // callback sees the new graph); a null graph renders silence.
    void Attach(ProductionGraph *graph);

    // Binds the tap source: real captured samples come from here. The mixer
    // keeps a deviceId per graph source node ("asrc:<stableId>" → wavein id)
    // and drains real frames from the taps every render chunk. Without a
    // tap source the mixer renders nothing (there is no fake).
    void AttachTaps(bps::platform::IAudio *taps);

    // The platform render callback's body: interleaved float32 out.
    // Returns false when there is nothing to render (no graph) — the
    // platform emits true silence for that chunk.
    bool Render(float *out, uint32_t frameCount, uint32_t channelCount, uint32_t sampleRateHz);

    // The routing pump maps a graph source node to its capture device.
    // `waveinId` UINT32_MAX = unmapped (contributes nothing).
    void BindSourceDevice(const std::string &sourceNodeId, uint32_t waveinId);
    void UnbindSource(const std::string &sourceNodeId);

    // Diagnostics for the settings screen.
    bool Attached() const { return graph_ != nullptr; }

private:
    // One route's contribution: the source's REAL captured frames (drained
    // from its tap FIFO) through the branch gain/pan, summed into the
    // interleaved buffer. True when it actually contributed. outRateHz is
    // the RENDER endpoint's rate — when it differs from the tap's own
    // TapFormat().sampleRateHz, the tap is linearly resampled to match
    // (found in review: this used to be silently skipped, playing a
    // mismatched-rate tap at the wrong pitch/speed).
    bool SumSource(const std::string &sourceNodeId, const VolumeControl &busVol,
                   float *out, uint32_t frames, uint32_t channels, uint32_t outRateHz);

    // Runs `node`'s ProcessingStage chain over one already-resampled STEREO
    // frame (l/r, in place) — called once per output frame from SumSource,
    // after resampling and before the branch gain/pan (so gain/pan still
    // apply post-processing, matching a normal channel-strip order: dynamics
    // → fader). `state` is this source's persistent per-stage DSP state
    // (envelope followers, delay line) — keyed by source node id so it
    // survives across render callbacks; a stage instance with no state of
    // its own (Gain) ignores it.
    struct SourceDspState {
        // Compressor/limiter: a shared LINEAR gain-reduction envelope per
        // stage kind (attack instant, release smoothed) — one each, since a
        // chain could carry both.
        float compEnvelope = 1.0f;
        float limiterEnvelope = 1.0f;
        // Gate: smoothed linear gain (0..1), not a hard on/off — an instant
        // gate clicks every time it opens/closes.
        float gateGain = 1.0f;
        // Delay: a real circular line of past OUTPUT-rate stereo samples,
        // sized to the stage's configured delay time (rebuilt if that
        // changes).
        std::vector<float> delayLine;   // interleaved L/R
        size_t delayPos = 0;
        double delaySeconds = -1.0;     // what delayLine is currently sized for
    };
    void ApplyProcessing(const NodeInfo &node, SourceDspState &state,
                        float &l, float &r, uint32_t outRateHz);

    ProductionGraph *graph_ = nullptr;   // not owned
    bps::platform::IAudio *taps_ = nullptr;   // not owned; the sample source
    std::mutex mutex_;                   // guards deviceTable_ and dspState_
    // sourceNodeId → capture device (UINT32_MAX = unmapped). Set by the
    // routing pump (EngineBridge::feedMixerLevels), read by the render thread.
    std::map<std::string, uint32_t> deviceTable_;
    // sourceNodeId → its ProcessingStage chain's persistent DSP state —
    // survives across render callbacks (an envelope follower or delay line
    // reset every callback would be useless). Entries accumulate per source
    // ever routed; not pruned on unroute (cheap, small, and a re-route
    // shouldn't lose an in-flight envelope for no reason).
    std::map<std::string, SourceDspState> dspState_;
    // Per-chunk scratch: the frames drained from one tap, mono→stereo handled
    // in SumSource. Resize-only, reused.
    std::vector<float> scratch_;
    std::atomic<float> masterGain_{1.0f};
};

} // namespace bps::production
