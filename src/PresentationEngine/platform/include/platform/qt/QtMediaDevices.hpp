#pragma once

#include "platform/IAudio.hpp"
#include "platform/IVideo.hpp"

#include <memory>

namespace bps::platform {

// Qt Multimedia's cross-platform device catalogue, audio I/O, camera preview,
// and Linux/X11 screen preview adapter.
class QtAudioDevices final : public IAudio {
public:
    QtAudioDevices();
    ~QtAudioDevices() override;
    QtAudioDevices(const QtAudioDevices&) = delete;
    QtAudioDevices& operator=(const QtAudioDevices&) = delete;

    std::vector<AudioDeviceInfo> Enumerate() const override;
    Result<AudioDeviceInfo> DefaultOutput() const override;
    Result<AudioDeviceInfo> DefaultInput() const override;
    std::string Fingerprint() const override;

    Result<void> StartInputMeter(uint32_t deviceId,
                                 const std::string& deviceName = {}) override;
    Result<void> StopInputMeter(uint32_t deviceId) override;
    InputMeterLevels InputLevels(uint32_t deviceId) override;
    std::vector<uint32_t> ActiveInputMeters() const override;
    TapAudio TapFormat(uint32_t deviceId) override;
    size_t ReadTapAudio(uint32_t deviceId, float* dst, size_t frames) override;
    Result<void> StartStreamTap(uint32_t tapId, const std::string& source,
                                const std::string& kind) override;
    Result<void> StopStreamTap(uint32_t tapId) override;
    std::vector<uint32_t> ActiveStreamTaps() const override;
    Result<void> PushStreamTapAudio(uint32_t tapId, const float* interleaved,
                                    uint32_t channels, uint32_t frames,
                                    uint32_t rate) override;
    Result<void> StartRender(RenderCallback callback, uint32_t deviceId = UINT32_MAX) override;
    Result<void> StopRender() override;
    bool Rendering() const override;
    Result<void> StartRenderStream(uint64_t streamId, RenderCallback callback,
                                   uint32_t deviceId) override;
    Result<void> StopRenderStream(uint64_t streamId) override;
    // The level of what this app plays (measured in the render stream: one mechanism on every OS).
    Result<void> StartOutputMeter(uint32_t deviceId) override;
    Result<void> StopOutputMeter(uint32_t deviceId) override;
    InputMeterLevels OutputLevels(uint32_t deviceId) override;

private:
    struct State;
    std::shared_ptr<State> state_;
};

class QtVideoDevices final : public IVideo {
public:
    QtVideoDevices();
    ~QtVideoDevices() override;
    std::vector<VideoDeviceInfo> Enumerate() const override;
    std::string Fingerprint() const override;
    Result<void> StartPreview(const std::string& deviceId, const std::string& mode,
                              const std::string& deviceName = {}) override;
    Result<void> StopPreview(const std::string& deviceId) override;
    std::vector<uint8_t> PreviewFrame(const std::string& deviceId) override;
    std::string PreviewFailure(const std::string& deviceId) const override;
    std::vector<std::string> ActivePreviews() const override;
    Result<void> StartScreenPreview(const std::string& monitorId) override;
    Result<void> StopScreenPreview(const std::string& monitorId) override;
    DecodedFrame PreviewFramePixels(const std::string& deviceId) override;
    SharedFrame PreviewFrameShared(const std::string& deviceId) override;
    uint64_t PreviewFrameSeq(const std::string& deviceId) override;
    void SetPreviewLive(const std::string& deviceId, bool live) override;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace bps::platform
