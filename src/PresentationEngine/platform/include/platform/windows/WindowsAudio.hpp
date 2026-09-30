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

    // Tap sample-through: the capture thread also FIFOs its converted
    // float32 frames; the engine's mixer drains them (real playout carrier).
    TapAudio TapFormat(uint32_t deviceId) override;
    size_t ReadTapAudio(uint32_t deviceId, float *dst, size_t frames) override;

    // OUTPUT metering — WASAPI LOOPBACK capture on the render endpoint (the
    // program mix the engine plays out). Same tap/snapshot discipline as the
    // input meters; deviceId is the waveout:<n> roster number.
    Result<void> StartOutputMeter(uint32_t deviceId) override;
    Result<void> StopOutputMeter(uint32_t deviceId) override;
    InputMeterLevels OutputLevels(uint32_t deviceId) override;

    // RENDER — a REAL shared-mode WASAPI render client (IAudioRenderClient),
    // the first output path in this codebase that makes sound: an OS thread
    // pulls the mixer's float32 frames and writes them to the speaker.
    Result<void> StartRender(RenderCallback callback, uint32_t deviceId = UINT32_MAX) override;
    Result<void> StopRender() override;
    bool Rendering() const override;

private:
    struct MeterTap;
    struct MeterTable;   // the pimpl: mutex + one tap per waveIn/waveOut device id
    // Forward-declared here (not just below, by render_) because
    // RenderThread's own declaration, right after, names it — a type must
    // be at least forward-declared before its first use, same reason
    // MeterTap/MeterTable sit above MeterThread rather than below meters_.
    struct RenderStream;
    // The capture thread body (one per running tap) — a static member so it
    // can touch the private nested type. Takes the table's mutex (the same
    // one Start/Stop/InputLevels callers hold) so snapshots publish
    // consistently. loopback=false → input capture; true → render-endpoint
    // loopback (the program mix).
    static void MeterThread(MeterTap &tap, std::mutex &publishMutex, bool loopback);
    // The render thread body (one stream at a time) — a static member so it
    // can touch the private nested RenderStream, same discipline as
    // MeterThread. Owns the whole COM/device lifetime; the stream's stop
    // flag is the only thing StopRender touches from outside.
    static void RenderThread(RenderStream &stream);

    // unique_ptr<incomplete MeterTable>: its destructor is only ever used
    // inside this class's own out-of-line destructor (an in-header cleanup
    // path — e.g. an enclosing implicit constructor's exception handler —
    // would otherwise need the table's full definition).
    std::unique_ptr<MeterTable> meters_;

    // The render stream (pimpl'd like the meter table: the thread handle and
    // its stop flag live out-of-line). One stream at a time.
    std::unique_ptr<RenderStream> render_;
};

} // namespace bps::platform
