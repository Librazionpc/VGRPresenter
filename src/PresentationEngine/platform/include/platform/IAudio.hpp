#pragma once

#include "core/common/Common.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bps::platform {
struct AudioDeviceInfo {
    std::string id;            // stable id (e.g. ALSA "hw:0,0", "default")
    std::string name;          // friendly name
    bool isInput = false;      // false = output device
    bool isDefault = false;
    uint32_t sampleRateHz = 0; // 0 = unknown
    uint32_t channels = 0;     // 0 = unknown
};

class IAudio {
public:
    virtual ~IAudio() = default;

    // All devices visible to the OS (best effort; may be empty on systems
    // without a supported audio stack).
    virtual    std::vector<AudioDeviceInfo> Enumerate() const = 0;

    virtual std::vector<AudioDeviceInfo> EnumerateFor(
        const std::vector<std::string> &names) const
    {
        std::vector<AudioDeviceInfo> out;
        if (names.empty())
            return out;
        for (const AudioDeviceInfo &d : Enumerate()) {
            for (const std::string &want : names) {
                if (d.name == want) {
                    out.push_back(d);
                    break;
                }
            }
        }
        return out;
    }

    virtual Result<AudioDeviceInfo> DefaultOutput() const = 0;
    virtual Result<AudioDeviceInfo> DefaultInput() const = 0;

    virtual std::string Fingerprint() const = 0;

    // ---- Input metering ------------------------------------------------
    // deviceId is the input device's own index (Windows: the waveIn device
    // number — AudioDeviceInfo::id is "wavein:<n>").

    // Channels beyond this many in the mix format are not metered.
    static constexpr int kMaxInputMeterChannels = 16;

    // Per-channel level snapshot for one capture tap. peaks[]/rms[] are
    // 0..1 fractions of full scale (channel i of channelCount).

    enum class ChannelLayout : int { None = 0, Mono = 1, Stereo = 2, Multi = 3 };

    struct InputMeterLevels {
        uint32_t deviceId = 0;
        uint8_t channelCount = 0;    // 0 = no tap / no data yet ("no signal")
        uint32_t sampleRateHz = 0;
        uint64_t framesCaptured = 0; // monotonic sample frames since Start
        ChannelLayout layout = ChannelLayout::None;
        float peaks[kMaxInputMeterChannels] = {};
        float rms[kMaxInputMeterChannels] = {};

        // Layout for a given mix-format channel count (0 → None).
        static constexpr ChannelLayout LayoutFor(int channels)
        {
            return channels <= 0 ? ChannelLayout::None
                 : channels == 1 ? ChannelLayout::Mono
                 : channels == 2 ? ChannelLayout::Stereo
                 : ChannelLayout::Multi;
        }
        static const char *ToString(ChannelLayout l)
        {
            switch (l) {
                case ChannelLayout::Mono:   return "mono";
                case ChannelLayout::Stereo: return "stereo";
                case ChannelLayout::Multi:  return "multi";
                default:                    return "none";
            }
        }
    };

    virtual Result<void> StartInputMeter(uint32_t deviceId,
                                         const std::string &deviceName = {})
    {
        (void)deviceId;
        (void)deviceName;
        return Error::Make(Err::Unsupported, "Audio",
                           "input metering is not implemented on this platform");
    }
    // Stop capturing and release the tap. Idempotent; stopping an unknown
    // device is Ok (nothing to do).
    virtual Result<void> StopInputMeter(uint32_t deviceId)
    {
        (void)deviceId;
        return Error::Make(Err::Unsupported, "Audio",
                           "input metering is not implemented on this platform");
    }
    // The current snapshot for a device — zeros (channelCount 0) when no tap
    // is running: an absent meter reads "no signal", never a synthetic one.
    virtual InputMeterLevels InputLevels(uint32_t deviceId)
    {
        (void)deviceId;
        return InputMeterLevels{};
    }
    // Devices with a live meter tap (drives the UI's metering refresh pump).
    virtual std::vector<uint32_t> ActiveInputMeters() const { return {}; }

    // ---- Tap sample-through (the mixer's REAL carrier) -------------------
    struct TapAudio {
        uint32_t channels = 0;      // 0 = no tap / not supported
        uint32_t sampleRateHz = 0;
    };
    // The tap's live capture format (what ReadTapAudio delivers).
    virtual TapAudio TapFormat(uint32_t deviceId)
    {
        (void)deviceId;
        return TapAudio{};
    }
    // Drain up to `frames` frames into `dst` (interleaved, tap's channel
    // count). Returns the frames actually read (0 = drained or no tap).
    virtual size_t ReadTapAudio(uint32_t deviceId, float *dst, size_t frames)
    {
        (void)deviceId;
        (void)dst;
        (void)frames;
        return 0;
    }

    // ---- STREAM taps (audio that is not a capture device) ---------------
    static constexpr uint32_t kStreamTapBase = 0x40000000u;
    static bool IsStreamTap(uint32_t id) { return id >= kStreamTapBase; }

    virtual Result<void> StartStreamTap(uint32_t tapId, const std::string &source,
                                        const std::string &kind)
    {
        (void)tapId;
        (void)source;
        (void)kind;
        return Error::Make(Err::Unsupported, "Audio",
                           "stream audio taps are not implemented on this platform");
    }
    // Close the tap and release whatever feeds it. Idempotent.
    virtual Result<void> StopStreamTap(uint32_t tapId)
    {
        (void)tapId;
        return Ok();
    }
    // Stream taps with a live feed (the caller's own bookkeeping check).
    virtual std::vector<uint32_t> ActiveStreamTaps() const { return {}; }

    virtual Result<void> PushStreamTapAudio(uint32_t tapId, const float *interleaved,
                                            uint32_t channels, uint32_t frames,
                                            uint32_t rate)
    {
        (void)tapId;
        (void)interleaved;
        (void)channels;
        (void)frames;
        (void)rate;
        return Ok();
    }

    // ---- OUTPUT metering (the program mix) ------------------------------
    virtual Result<void> StartOutputMeter(uint32_t deviceId)
    {
        (void)deviceId;
        return Error::Make(Err::Unsupported, "Audio",
                           "output metering is not implemented on this platform");
    }
    virtual Result<void> StopOutputMeter(uint32_t deviceId)
    {
        (void)deviceId;
        return Error::Make(Err::Unsupported, "Audio",
                           "output metering is not implemented on this platform");
    }
    // Zeros (channelCount 0) when no loopback tap is running.
    virtual InputMeterLevels OutputLevels(uint32_t deviceId)
    {
        (void)deviceId;
        return InputMeterLevels{};
    }

    // ---- Render (the engine's PLAYOUT — what the user actually hears) ----
    using RenderCallback = std::function<bool(float *frames, uint32_t frameCount,
                                              uint32_t channels, uint32_t sampleRateHz)>;
    // Start rendering through `callback` on the default output device (the
    // roster's waveout:<n> when given). Already-running → Ok (no-op).
    virtual Result<void> StartRender(RenderCallback callback, uint32_t deviceId = UINT32_MAX)
    {
        (void)callback;
        (void)deviceId;
        return Error::Make(Err::Unsupported, "Audio",
                           "audio render is not implemented on this platform");
    }
    // Stop and join the render thread. Idempotent; stopping when idle is Ok.
    virtual Result<void> StopRender()
    {
        return Error::Make(Err::Unsupported, "Audio",
                           "audio render is not implemented on this platform");
    }
    // Is the render stream alive? (the UI's "engine audio on" state)
    virtual bool Rendering() const { return false; }

    // ---- A SECOND render stream, for an output with its own endpoint --------
    virtual Result<void> StartRenderStream(uint64_t streamId, RenderCallback callback,
                                           uint32_t deviceId)
    {
        (void)streamId;
        (void)callback;
        (void)deviceId;
        return Error::Make(Err::Unsupported, "Audio",
                           "per-output render streams are not implemented on this platform");
    }
    virtual Result<void> StopRenderStream(uint64_t streamId)
    {
        (void)streamId;
        return Ok();
    }
};
} // namespace bps::platform