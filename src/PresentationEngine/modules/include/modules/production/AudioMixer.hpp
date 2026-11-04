#pragma once

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
#include <set>
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

    void AttachTaps(bps::platform::IAudio *taps);

    bool Render(float *out, uint32_t frameCount, uint32_t channelCount, uint32_t sampleRateHz);

    bool RenderOutput(std::string_view outputNodeId, float *out, uint32_t frameCount,
                      uint32_t channelCount, uint32_t sampleRateHz);

    // The format the mix is rendered at; zero until the first chunk.
    struct MixFormat {
        uint32_t rateHz = 0;
        uint32_t channels = 0;
    };
    MixFormat Format() const;

    using MixTap = std::function<void(const float *samples, uint32_t frames,
                                      uint32_t channels, uint32_t rateHz,
                                      uint64_t startFrame)>;
    // Frames rendered since the mixer was built, silence included: the sample clock.
    uint64_t FramesRendered() const { return framesRendered_.load(std::memory_order_relaxed); }
    uint64_t AddMixTap(std::string outputNodeId, MixTap tap);
    void RemoveMixTap(uint64_t id);
    // The graph output nodes somebody is listening to through a tap right now (the speaker
    // node for a tap with no node of its own): what a recording or an NDI feed is asking for.
    std::set<std::string> TapTargets() const;

    // The routing pump maps a graph source node to its capture device.
    // `waveinId` UINT32_MAX = unmapped (contributes nothing).
    void BindSourceDevice(const std::string &sourceNodeId, uint32_t waveinId);
    void UnbindSource(const std::string &sourceNodeId);

    // ---- program-mix metering ---------------------------------------------
    struct ProgramLevels {
        uint32_t channelCount = 0;          // 0 = nothing rendered yet
        float peaks[2] = {0.0f, 0.0f};      // peak-held across chunks, decaying
        float rms[2] = {0.0f, 0.0f};
        uint64_t framesRendered = 0;        // since construction
    };
    ProgramLevels ProgramLevel() const;

    // Diagnostics for the settings screen.
    bool Attached() const { return graph_ != nullptr; }

private:
    void BuildPlanes(const GraphSnapshot &snap, uint32_t frameCount, uint32_t channelCount,
                     uint32_t sampleRateHz);
    // The mix itself: fills `out` and returns false when nothing was produced.
    // Caller holds mutex_.
    bool MixInto(const GraphSnapshot &snap, float *out, uint32_t frameCount,
                 uint32_t channelCount, uint32_t sampleRateHz);
    bool BusIsHeard(const GraphSnapshot &snap, const std::string &busId);
    bool MixSelected(const GraphSnapshot &snap, std::string_view nodeId, float *dst,
                     uint32_t frameCount, uint32_t channelCount);
    void NotifyTaps(const GraphSnapshot *snap, uint32_t frames, uint32_t channels,
                    uint32_t rateHz, uint64_t startFrame);

    bool SumSource(const GraphSnapshot &snap,
                   const std::string &sourceNodeId, const std::string &busNodeId,
                   const VolumeControl &busVol,
                   float busGainL, float busGainR,
                   float *out, uint32_t frames, uint32_t channels, uint32_t outRateHz);

    struct SourceDspState {
        float compEnvelope = 1.0f;
        float limiterEnvelope = 1.0f;
        // Gate: smoothed linear gain (0..1), not a hard on/off — an instant
        // gate clicks every time it opens/closes.
        float gateGain = 1.0f;
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
    mutable std::mutex tapMutex_;        // guards mixTaps_
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
    std::map<std::string, SourceDspState> dspState_;
    // Per-chunk scratch: the frames drained from one tap, mono→stereo handled
    // in SumSource. Resize-only, reused.
    std::vector<float> scratch_;
    std::atomic<float> masterGain_{1.0f};
    mutable std::mutex levelMutex_;
    ProgramLevels programLevel_;
    // How much of the held peak survives one chunk — a readable fall-off for a
    // meter, not an instant drop to zero the moment a source goes quiet.
    static constexpr float kPeakHold = 0.85f;
};
} // namespace bps::production