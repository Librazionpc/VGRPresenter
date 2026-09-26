#pragma once

// Windows PAL backend for the audio device subsystem (winmm waveIn/waveOut
// enumeration — no mixing, no playback) plus a REAL WASAPI capture tap per
// metered input device: Start/Stop spawn a capture thread, InputLevels()
// reads the lock-protected per-channel peak/RMS snapshot it computes.

#include "platform/IAudio.hpp"

#include <memory>

namespace bps::platform {

class WindowsAudio final : public IAudio {
public:
    // Out-of-line ctor AND dtor (the pimpl rule): an implicit ctor generated
    // in a TU that only sees this header would carry an exception-cleanup
    // path destroying meters_ — needing MeterTable's full definition (a
    // containing aggregate's make_unique, e.g. WindowsPlatform's, then fails
    // to compile). Both are defined in the .cpp beside the complete type.
    WindowsAudio();
    ~WindowsAudio() override;

    std::vector<AudioDeviceInfo> Enumerate() const override;
    Result<AudioDeviceInfo> DefaultOutput() const override;
    Result<AudioDeviceInfo> DefaultInput() const override;
    std::string Fingerprint() const override;

    Result<void> StartInputMeter(uint32_t deviceId) override;
    Result<void> StopInputMeter(uint32_t deviceId) override;
    InputMeterLevels InputLevels(uint32_t deviceId) override;
    std::vector<uint32_t> ActiveInputMeters() const override;

private:
    struct MeterTap;
    struct MeterTable;   // the pimpl: mutex + one tap per waveIn device id
    // The capture thread body (one per running tap) — a static member so it
    // can touch the private nested type. Takes the table's mutex (the same
    // one Start/Stop/InputLevels callers hold) so snapshots publish
    // consistently.
    static void MeterThread(MeterTap &tap, std::mutex &publishMutex);

    // unique_ptr<incomplete MeterTable>: its destructor is only ever used
    // inside this class's own out-of-line destructor (an in-header cleanup
    // path — e.g. an enclosing implicit constructor's exception handler —
    // would otherwise need the table's full definition).
    std::unique_ptr<MeterTable> meters_;
};

} // namespace bps::platform
