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
// What the render fills is the PROGRAM: every bus wired to the speaker endpoint,
// PLUS every bus wired to an output that plays locally (an HDMI/projector display,
// flagged `playsLocally` on its node by RoutingService). Those display outputs are
// not a second render stream — the platform renders one, and on a booth machine the
// display IS that endpoint — but a bus wired to both is summed once, never twice.
// Outputs that leave through their own sink (NDI, recording, streaming) contribute
// nothing here; they are read with AddMixTap instead.
//
// Scope: VOLUME/GAIN/MUTE + pan/balance, plus real per-source ProcessingStage
// DSP for Gain/Gate/Compressor/Limiter/Delay (envelope-follower dynamics + a
// real delay line), Eq (one peaking band per stage, frequency/gain/Q) and
// Custom "reverb" (Freeverb-style stereo reverb, amount = wet mix) — see
// ApplyProcessing in the .cpp and the sample blocks in AudioDsp.hpp. Denoise is an
// adaptive noise reducer (it learns each source's floor). ColorCorrect is video-only
// and passes audio through.

#include "modules/production/AudioDsp.hpp"
#include "modules/production/ProductionGraph.hpp"
#include "modules/production/ProductionTypes.hpp"
#include "platform/IAudio.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bps::production {

// The graph output node the speaker endpoint is. Only audio buses with an edge to it
// are mixed into the endpoint.
inline constexpr const char kSpeakersOutputId[] = "out:speakers";

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

    // The mix of ONE graph output, rendered straight to a device of its own: an
    // HDMI monitor's speakers are a separate WASAPI client, so its stream asks for
    // its own frames at ITS rate instead of sharing the program's. Same buses the
    // output's tap hears (its edges), master-scaled like the program; no meter is
    // published (the program meter stays the program). Safe to call from a second
    // render thread: Render() and this one serialise on the mixer's mutex.
    bool RenderOutput(std::string_view outputNodeId, float *out, uint32_t frameCount,
                      uint32_t channelCount, uint32_t sampleRateHz);

    // The format the mix is rendered at; zero until the first chunk.
    struct MixFormat {
        uint32_t rateHz = 0;
        uint32_t channels = 0;
    };
    MixFormat Format() const;

    // A listener on the rendered mix: every chunk, silence included, interleaved, on
    // the render thread. A tap must only copy what it needs and must not add or
    // remove taps.
    // `startFrame` is the chunk's first frame on the mixer's sample clock (see
    // FramesRendered), so a listener can place it exactly in time.
    using MixTap = std::function<void(const float *samples, uint32_t frames,
                                      uint32_t channels, uint32_t rateHz,
                                      uint64_t startFrame)>;
    // Frames rendered since the mixer was built, silence included: the sample clock.
    uint64_t FramesRendered() const { return framesRendered_.load(std::memory_order_relaxed); }
    // A listener on the mix of one graph output: the sum of the audio buses wired to
    // `outputNodeId` (a bus node id means that bus alone; empty means the speakers).
    // A recording taps the output it was started on, so it hears only what is wired there.
    uint64_t AddMixTap(std::string outputNodeId, MixTap tap);
    void RemoveMixTap(uint64_t id);

    // The routing pump maps a graph source node to its capture device.
    // `waveinId` UINT32_MAX = unmapped (contributes nothing).
    void BindSourceDevice(const std::string &sourceNodeId, uint32_t waveinId);
    void UnbindSource(const std::string &sourceNodeId);

    // ---- program-mix metering ---------------------------------------------
    // What this mixer ACTUALLY renders, per channel, for the UI's output meters.
    // Those meters used to read a WASAPI LOOPBACK capture of the machine's default
    // render endpoint, which was wrong in two visible ways: merely going live opened
    // a capture session on the OS (the app appeared in Windows' audio-session list
    // uninvited), and a loopback picks up every OTHER app's audio on that endpoint,
    // so the bars moved whenever the machine made any sound. Reading the mix the app
    // itself renders is the honest source: silence here means the program mix really
    // is silent.
    struct ProgramLevels {
        uint32_t channelCount = 0;          // 0 = nothing rendered yet
        float peaks[2] = {0.0f, 0.0f};      // peak-held across chunks, decaying
        float rms[2] = {0.0f, 0.0f};
        uint64_t framesRendered = 0;        // since construction
    };
    // Safe from any thread (the UI polls it at ~20 Hz while the render thread
    // writes it). Peaks are HELD across chunks with a decay, so a reader slower
    // than the render callback sees a level, not a peak-then-zero flicker.
    ProgramLevels ProgramLevel() const;

    // Diagnostics for the settings screen.
    bool Attached() const { return graph_ != nullptr; }

private:
    // Builds the per-bus plane buffers for this chunk. The caller holds mutex_
    // (Render and RenderOutput both take it around their whole mix, so a second
    // output stream can never rebuild the planes under the program's taps).
    void BuildPlanes(const GraphSnapshot &snap, uint32_t frameCount, uint32_t channelCount,
                     uint32_t sampleRateHz);
    // The mix itself: fills `out` and returns false when nothing was produced.
    // Caller holds mutex_.
    bool MixInto(const GraphSnapshot &snap, float *out, uint32_t frameCount,
                 uint32_t channelCount, uint32_t sampleRateHz);
    // Sums the planes that feed `nodeId` into `dst` (zeroed first). False when none of
    // them produced sound.
    // True when something reads this bus: an output wired from it, or a tap naming it.
    // A bus with no reader is not summed, so its source taps are not drained for no one.
    bool BusIsHeard(const GraphSnapshot &snap, const std::string &busId);
    bool MixSelected(const GraphSnapshot &snap, std::string_view nodeId, float *dst,
                     uint32_t frameCount, uint32_t channelCount);
    void NotifyTaps(const GraphSnapshot *snap, uint32_t frames, uint32_t channels,
                    uint32_t rateHz, uint64_t startFrame);

    // One route's contribution: the source's REAL captured frames (drained
    // from its tap FIFO) through the branch gain/pan, summed into the
    // interleaved buffer. True when it actually contributed. outRateHz is
    // the RENDER endpoint's rate — when it differs from the tap's own
    // TapFormat().sampleRateHz, the tap is linearly resampled to match
    // (found in review: this used to be silently skipped, playing a
    // mismatched-rate tap at the wrong pitch/speed).
    //
    // busGainL/busGainR are the BUS plane's own per-side trims (its node's
    // "uiChannelGain<n>" meta, read by Render) — a bus card's L/R buttons
    // write those, so closing one side of a BUS closes that side of the bus's
    // summed output here, exactly the way closing a side of a SOURCE closes
    // it in this same function.
    //
    // busNodeId is the BUS PLANE being summed ("bus:N#a"): a source row's
    // routing matrix writes a per-channel mask FOR THAT BUS
    // ("uiRouteMask:<busNodeId>"), so one source can contribute its left channel
    // to one bus and its right to another. No mask = the whole source, the
    // contract an older graph and the bus dialog's own route carry.
    bool SumSource(const GraphSnapshot &snap,
                   const std::string &sourceNodeId, const std::string &busNodeId,
                   const VolumeControl &busVol,
                   float busGainL, float busGainR,
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
        // EQ: one entry per Eq stage in the chain, in chain order (a band's filter
        // state survives across callbacks, like the envelopes above).
        struct EqBandState {
            dsp::PeakingBand left, right;
        };
        std::vector<EqBandState> eq;
        // Reverb: one entry per reverb stage in the chain, in chain order.
        struct ReverbState {
            dsp::Reverb left, right;
        };
        std::vector<ReverbState> reverb;
        // Denoise: one entry per denoise stage in the chain, in chain order.
        std::vector<dsp::NoiseReducer> denoise;
    };
    void ApplyProcessing(const NodeInfo &node, SourceDspState &state,
                        float &l, float &r, uint32_t outRateHz);

    // Publishes the program levels of one rendered chunk (see ProgramLevels).
    // `out` is interleaved at `channels`; only the first two channels are kept.
    void PublishLevels(const float *out, uint32_t frames, uint32_t channels);
    // A chunk that rendered nothing: decay the held peaks toward zero instead of
    // leaving the meter frozen at the last thing it happened to catch.
    void PublishSilence();

    ProductionGraph *graph_ = nullptr;   // not owned
    bps::platform::IAudio *taps_ = nullptr;   // not owned; the sample source
    std::mutex mutex_;                   // guards deviceTable_ and dspState_
    std::mutex tapMutex_;                // guards mixTaps_
    struct MixTapEntry {
        uint64_t id = 0;
        std::string nodeId;
        MixTap tap;
    };
    std::vector<MixTapEntry> mixTaps_;

    // One audio bus plane's sound for the current chunk. Render thread only.
    struct PlaneBuffer {
        std::string id;
        std::vector<float> samples;
        bool any = false;
    };
    std::vector<PlaneBuffer> planes_;   // reused between chunks; the first planeCount_ are live
    size_t planeCount_ = 0;
    std::vector<float> tapScratch_;     // one tap's mix, reused between taps
    uint64_t nextTapId_ = 1;
    std::atomic<uint32_t> formatRate_{0};
    std::atomic<uint32_t> formatChannels_{0};
    std::atomic<uint64_t> framesRendered_{0};
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
    // Program-level publication: written on the render thread, read on the UI
    // thread. A separate mutex from mutex_ (which the render path already holds
    // when it publishes) and never taken while holding that one.
    mutable std::mutex levelMutex_;
    ProgramLevels programLevel_;
    // How much of the held peak survives one chunk — a readable fall-off for a
    // meter, not an instant drop to zero the moment a source goes quiet.
    static constexpr float kPeakHold = 0.85f;
};

} // namespace bps::production
