#include "platform/windows/WindowsAudio.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <mmsystem.h>

namespace bps::platform {

std::vector<AudioDeviceInfo> WindowsAudio::Enumerate() const {
    std::vector<AudioDeviceInfo> out;

    // Output devices (winmm). dwFormats is a bitmask; 44.1k/48k are near
    // universal, so those are reported as the nominal rate.
    UINT outputs = waveOutGetNumDevs();
    for (UINT i = 0; i < outputs; ++i) {
        // Explicit *W struct, not the TCHAR-generic WAVEOUTCAPS: this file
        // isn't compiled with UNICODE defined, so the generic name resolves
        // to the ANSI (szPname: CHAR[]) variant, which doesn't match the
        // explicit waveOutGetDevCapsW() call below (LPWAVEOUTCAPSW).
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        AudioDeviceInfo d;
        d.id = "waveout:" + std::to_string(i);
        d.name = win::Utf8(caps.szPname);
        d.isInput = false;
        d.isDefault = (i == 0);
        d.sampleRateHz = 48000;   // nominal; caps are a bitmask of supported rates
        d.channels = 2;
        out.push_back(std::move(d));
    }

    // Input devices.
    UINT inputs = waveInGetNumDevs();
    for (UINT i = 0; i < inputs; ++i) {
        WAVEINCAPSW caps{};   // see the WAVEOUTCAPSW note above
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        AudioDeviceInfo d;
        d.id = "wavein:" + std::to_string(i);
        d.name = win::Utf8(caps.szPname);
        d.isInput = true;
        d.isDefault = (i == 0);
        d.sampleRateHz = 48000;
        d.channels = 2;
        out.push_back(std::move(d));
    }
    return out;
}

Result<AudioDeviceInfo> WindowsAudio::DefaultOutput() const {
    auto devs = Enumerate();
    for (const auto& d : devs)
        if (!d.isInput && d.isDefault) return d;
    for (const auto& d : devs)
        if (!d.isInput) return d;
    return Error::Make(Err::NotFound, "Audio", "no output devices found");
}

Result<AudioDeviceInfo> WindowsAudio::DefaultInput() const {
    auto devs = Enumerate();
    for (const auto& d : devs)
        if (d.isInput && d.isDefault) return d;
    for (const auto& d : devs)
        if (d.isInput) return d;
    return Error::Make(Err::NotFound, "Audio", "no input devices found");
}

std::string WindowsAudio::Fingerprint() const {
    std::string fp;
    for (const auto& d : Enumerate())
        fp += d.id + (d.isInput ? ":in;" : ":out;");
    return fp;
}

} // namespace bps::platform
