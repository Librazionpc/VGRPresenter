#pragma once

// Windows PAL backend for the audio device subsystem (winmm waveIn/waveOut
// enumeration — no mixing, no playback) plus a REAL WASAPI capture tap per
// metered input device: Start/Stop spawn a capture thread, InputLevels()
// reads the lock-protected per-channel peak/RMS snapshot it computes.

#include "platform/IAudio.hpp"

#include <set>
#include <string>
#include <vector>

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
    // Targeted walk (see IAudio::EnumerateFor): only the named endpoints get
    // activated + mix-format-queried — the rest of the machine is never touched.
    std::vector<AudioDeviceInfo> EnumerateFor(
        const std::vector<std::string> &names) const override;
    Result<AudioDeviceInfo> DefaultOutput() const override;
    Result<AudioDeviceInfo> DefaultInput() const override;
    std::string Fingerprint() const override;

    Result<void> StartInputMeter(uint32_t deviceId,
                                 const std::string &deviceName = {}) override;
    Result<void> StopInputMeter(uint32_t deviceId) override;
    InputMeterLevels InputLevels(uint32_t deviceId) override;
    std::vector<uint32_t> ActiveInputMeters() const override;

    // Tap sample-through: the capture thread also FIFOs its converted
    // float32 frames; the engine's mixer drains them (real playout carrier).
    // A STREAM tap (see StartStreamTap) answers the same three calls, so the
    // mixer and the meters consume a media file or an NDI stream through the
    // exact path they consume a capture device through.
    TapAudio TapFormat(uint32_t deviceId) override;
    size_t ReadTapAudio(uint32_t deviceId, float *dst, size_t frames) override;

    // ---- STREAM taps (media files, NDI streams) --------------------------
    // A stream tap is a MeterTap WITHOUT a capture device: either this backend
    // decodes the source's audio itself (kind "media" — a Media Foundation
    // audio-stream reader on its own thread, paced by the samples' own
    // timestamps so the meter moves in real time) or the source's owner pushes
    // the samples (kind "ndi" — the receiver calls PushStreamTapAudio).
    Result<void> StartStreamTap(uint32_t tapId, const std::string &source,
                                const std::string &kind) override;
    Result<void> StopStreamTap(uint32_t tapId) override;
    std::vector<uint32_t> ActiveStreamTaps() const override;
    Result<void> PushStreamTapAudio(uint32_t tapId, const float *interleaved,
                                    uint32_t channels, uint32_t frames,
                                    uint32_t rate) override;

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
    // The one walk Enumerate()/EnumerateFor() share: `only` narrows both the
    // WinMM caps loops and the WASAPI mix-format queries to the named endpoints.
    std::vector<AudioDeviceInfo> EnumerateFiltered(const std::set<std::string> *only) const;
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
    // The decode thread body of a kind="media" stream tap: a Media Foundation
    // audio-stream reader that hands each decoded block to the tap through the
    // SAME push path an NDI receiver uses (PushStreamTapAudio), so both kinds of
    // stream tap share one implementation of the levels, the ring and the
    // windowing.
    static void StreamThread(MeterTap &tap, MeterTable &table, std::string source);
    // The single implementation of "stream samples arrived": the decode thread
    // and every external pusher go through it, so the levels, the ring and the
    // ~50 ms windowing cannot differ between them.
    static Result<void> PushToTapTable(MeterTable &table, uint32_t tapId,
                                       const float *interleaved, uint32_t channels,
                                       uint32_t frames, uint32_t rate);
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

    // Stream taps (media files / NDI streams), keyed by their stream ids. A
    // separate table on purpose: ActiveInputMeters() answers the UI's "is a
    // DEVICE being metered" question, and a media file's audio is not a device.
    std::unique_ptr<MeterTable> streams_;

    // The render stream (pimpl'd like the meter table: the thread handle and
    // its stop flag live out-of-line). One stream at a time.
    std::unique_ptr<RenderStream> render_;
};

} // namespace bps::platform
