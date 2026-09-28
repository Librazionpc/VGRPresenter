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
};

} // namespace bps::platform
