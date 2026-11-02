#pragma once

#include "platform/IAudio.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <memory>

namespace bps::platform {
class WindowsAudio final : public IAudio {
public:
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

    TapAudio TapFormat(uint32_t deviceId) override;
    size_t ReadTapAudio(uint32_t deviceId, float *dst, size_t frames) override;

    // ---- STREAM taps (media files, NDI streams) --------------------------
    Result<void> StartStreamTap(uint32_t tapId, const std::string &source,
                                const std::string &kind) override;
    Result<void> StopStreamTap(uint32_t tapId) override;
    std::vector<uint32_t> ActiveStreamTaps() const override;
    Result<void> PushStreamTapAudio(uint32_t tapId, const float *interleaved,
                                    uint32_t channels, uint32_t frames,
                                    uint32_t rate) override;

    Result<void> StartOutputMeter(uint32_t deviceId) override;
    Result<void> StopOutputMeter(uint32_t deviceId) override;
    InputMeterLevels OutputLevels(uint32_t deviceId) override;

    Result<void> StartRender(RenderCallback callback, uint32_t deviceId = UINT32_MAX) override;
    Result<void> StopRender() override;
    bool Rendering() const override;
    // Per-output streams (see IAudio): one extra WASAPI client per output that has
    // a device of its own. The program stream above is untouched by these.
    Result<void> StartRenderStream(uint64_t streamId, RenderCallback callback,
                                   uint32_t deviceId) override;
    Result<void> StopRenderStream(uint64_t streamId) override;

private:
    // The one walk Enumerate()/EnumerateFor() share: `only` narrows both the
    // WinMM caps loops and the WASAPI mix-format queries to the named endpoints.
    std::vector<AudioDeviceInfo> EnumerateFiltered(const std::set<std::string> *only) const;
    struct MeterTap;
    struct MeterTable;   // the pimpl: mutex + one tap per waveIn/waveOut device id
    struct RenderStream;
    static void MeterThread(MeterTap &tap, std::mutex &publishMutex, bool loopback);
    static void StreamThread(MeterTap &tap, MeterTable &table, std::string source);
    static Result<void> PushToTapTable(MeterTable &table, uint32_t tapId,
                                       const float *interleaved, uint32_t channels,
                                       uint32_t frames, uint32_t rate);
    static void RenderThread(RenderStream &stream);

    std::unique_ptr<MeterTable> meters_;

    std::unique_ptr<MeterTable> streams_;

    // The render stream (pimpl'd like the meter table: the thread handle and
    // its stop flag live out-of-line). The PROGRAM stream — one at a time.
    std::unique_ptr<RenderStream> render_;
    std::mutex renderStreamsMutex_;
    std::map<uint64_t, std::unique_ptr<RenderStream>> renderStreams_;
};
} // namespace bps::platform