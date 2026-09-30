#pragma once

// PAL audio device subsystem (Phase 2): enumerates audio *devices* and their
// capabilities. Not media playback and not audio mixing — feature modules
// (media module, streaming) consume device info through this interface.
//
// Input metering (added 2026-09-26): a per-device capture tap computes real
// per-channel peak/RMS levels from the OS mix format — the only audio the
// engine touches. Backends without capture support keep the enumeration
// surface and return Unsupported / empty levels (the UI reads "no signal",
// never a synthetic waveform).

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

    virtual Result<AudioDeviceInfo> DefaultOutput() const = 0;
    virtual Result<AudioDeviceInfo> DefaultInput() const = 0;

    // Stable summary string of the current device set (ids + direction). The
    // Kernel's platform watcher compares successive values to emit
    // DeviceConnected / DeviceRemoved OS events (DoD §13 hot plug).
    virtual std::string Fingerprint() const = 0;

    // ---- Input metering ------------------------------------------------
    // deviceId is the input device's own index (Windows: the waveIn device
    // number — AudioDeviceInfo::id is "wavein:<n>").

    // Channels beyond this many in the mix format are not metered.
    static constexpr int kMaxInputMeterChannels = 16;

    // Per-channel level snapshot for one capture tap. peaks[]/rms[] are
    // 0..1 fractions of full scale (channel i of channelCount).
    // (Named InputMeterLevels, not InputLevels: the getter below is named
    // InputLevels and a same-named member type would be shadowed by it —
    // even qualified — inside this class.)

    // The tap's channel LAYOUT — what the UI's channel rows render (a mono
    // mic shows one strip, a stereo line-in two). Derived from the live
    // capture format, so it is engine truth, not the roster's guess.
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

    // Begin capturing the given input device for metering (a real WASAPI
    // capture client on Windows — 50 ms analysis windows, no rendering, no
    // output). Idempotent: a second Start on a running tap is a no-op.
    // NotFound when the device doesn't exist, Unsupported on platforms
    // without capture support.
    virtual Result<void> StartInputMeter(uint32_t deviceId)
    {
        (void)deviceId;
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
    // A running input tap keeps a small FIFO of its CONVERTED float32
    // frames (interleaved, the device's own rate/channel count) behind its
    // meter snapshots. The engine's mixer drains them per render chunk —
    // the actual samples a source routed into the graph produces, not a
    // level-carried approximation. Underrun reads return fewer frames than
    // asked (the mixer pads with silence); a stalled tap drains dry and
    // stays silent — never a repeated buffer.
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

    // ---- OUTPUT metering (the program mix) ------------------------------
    // The SAME snapshot shape, taken from a LOOPBACK capture on a RENDER
    // endpoint (Windows WASAPI loopback): the levels of everything the
    // machine — and therefore the engine's playout — is playing right now.
    // deviceId is the OUTPUT device's roster number (AudioDeviceInfo::id is
    // "waveout:<n>"); the default render endpoint is what the UI meters.
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
    // A REAL shared-mode WASAPI render client: an OS thread pulls float32
    // frames through `renderCallback` at the endpoint's mix rate and writes
    // them to the speaker. The FIRST render client in this codebase that
    // makes sound; metering taps stay read-only. One stream at a time (the
    // engine mixes everything into it).
    // The mixer body: fill `frames` interleaved frames for `channels`
    // channels at `sampleRateHz`. Return false to emit silence (a mixer
    // with no sources, or a fault). Called from the WASAPI thread — no
    // locks it can't afford, no allocations in the steady state.
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
};

} // namespace bps::platform
