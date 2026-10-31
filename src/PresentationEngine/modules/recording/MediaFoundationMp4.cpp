#include "modules/recording/MediaFoundationMp4.hpp"

#include "modules/audio/AudioEngine.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace bps::recording {

namespace {

constexpr const char *kModule = "MediaFoundationMp4";
// H.264 Main profile (eAVEncH264VProfile_Main) and the AAC bitrate the container asks
// for: 192 kbit/s is 24000 bytes per second.
constexpr uint32_t kH264ProfileMain = 77;
constexpr uint32_t kAacBytesPerSecond = 24000;

// Media Foundation is started once per user of it. Each container holds a count
// while it is open, so MFShutdown runs only when the last one closes.
std::mutex gMfMutex;
int gMfUsers = 0;

Result<void> AcquireMf()
{
    std::lock_guard<std::mutex> lock(gMfMutex);
    if (gMfUsers == 0) {
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr))
            return Error::Make(Err::Recording_EncoderUnavailable, kModule,
                               "Media Foundation did not start");
    }
    ++gMfUsers;
    return Ok();
}

void ReleaseMf()
{
    std::lock_guard<std::mutex> lock(gMfMutex);
    if (gMfUsers > 0 && --gMfUsers == 0)
        MFShutdown();
}

// A failed Media Foundation call, with the HRESULT in the message.
auto HrError(std::string_view what, HRESULT hr)
{
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    return Error::Make(Err::Recording_InvalidState, kModule,
                       std::string(what) + " failed (" + code + ")");
}

// Owns one COM interface pointer and releases it once.
template <typename T>
class ComRef {
public:
    ComRef() = default;
    ComRef(const ComRef &) = delete;
    ComRef &operator=(const ComRef &) = delete;
    ComRef(ComRef &&other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ComRef &operator=(ComRef &&other) noexcept
    {
        if (this != &other) {
            Reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }
    ~ComRef() { Reset(); }

    T *get() const { return p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    // For the Media Foundation calls that fill an out-pointer.
    T **Out()
    {
        Reset();
        return &p_;
    }
    void Reset()
    {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T *p_ = nullptr;
};

std::wstring ToWide(std::string_view text)
{
    if (text.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed);
    return out;
}

// Media Foundation time is in 100 ns units.
LONGLONG ToHundredNs(double milliseconds)
{
    return static_cast<LONGLONG>(std::llround(milliseconds * 10000.0));
}

// One packed-RGBA pixel of a source of size srcW x srcH, sampled at (x, y) of a
// w x h target (nearest neighbour).
uint32_t SampleAt(const uint32_t *src, int srcW, int srcH, int x, int y, int w, int h)
{
    const int sx = std::min(srcW - 1, static_cast<int>((static_cast<int64_t>(x) * srcW) / w));
    const int sy = std::min(srcH - 1, static_cast<int>((static_cast<int64_t>(y) * srcH) / h));
    return src[static_cast<size_t>(sy) * static_cast<size_t>(srcW) + static_cast<size_t>(sx)];
}

uint8_t Clamp8(int v)
{
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

// Packed RGBA to NV12 (BT.601, limited range): a full-resolution Y plane, then one
// interleaved UV plane at half resolution, each chroma sample averaged over its 2x2
// block of target pixels.
void PackedToNv12(const uint32_t *src, int srcW, int srcH, uint8_t *dst, int w, int h)
{
    uint8_t *yPlane = dst;
    uint8_t *uvPlane = dst + static_cast<size_t>(w) * static_cast<size_t>(h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t px = SampleAt(src, srcW, srcH, x, y, w, h);
            const int r = static_cast<int>(px & 0xFF);
            const int g = static_cast<int>((px >> 8) & 0xFF);
            const int b = static_cast<int>((px >> 16) & 0xFF);
            yPlane[static_cast<size_t>(y) * w + x] = Clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int cy = 0; cy < h / 2; ++cy) {
        for (int cx = 0; cx < w / 2; ++cx) {
            int sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const uint32_t px = SampleAt(src, srcW, srcH, cx * 2 + dx, cy * 2 + dy, w, h);
                    sr += static_cast<int>(px & 0xFF);
                    sg += static_cast<int>((px >> 8) & 0xFF);
                    sb += static_cast<int>((px >> 16) & 0xFF);
                }
            }
            const int r = sr / 4, g = sg / 4, b = sb / 4;
            uint8_t *uv = uvPlane + static_cast<size_t>(cy) * w + static_cast<size_t>(cx) * 2;
            uv[0] = Clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv[1] = Clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

int16_t ToPcm16(float sample)
{
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrint(clamped * 32767.0f));
}

// The media types one stream of the sink writer is described by.
Result<void> MakeVideoType(const GUID &subtype, int width, int height, uint32_t fpsNum,
                           uint32_t fpsDen, uint32_t bitrate, ComRef<IMFMediaType> &out)
{
    HRESULT hr = MFCreateMediaType(out.Out());
    if (FAILED(hr))
        return HrError("creating a video media type", hr);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, subtype);
    if (bitrate > 0)
        out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (IsEqualGUID(subtype, MFVideoFormat_H264))
        out->SetUINT32(MF_MT_MPEG2_PROFILE, kH264ProfileMain);
    MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                       static_cast<UINT32>(height));
    MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    return Ok();
}

Result<void> MakeAudioType(const GUID &subtype, uint32_t rateHz, uint32_t channels, uint32_t bits,
                           uint32_t bytesPerSecond, ComRef<IMFMediaType> &out)
{
    HRESULT hr = MFCreateMediaType(out.Out());
    if (FAILED(hr))
        return HrError("creating an audio media type", hr);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, subtype);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rateHz);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
    if (bytesPerSecond > 0)
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
    if (IsEqualGUID(subtype, MFAudioFormat_PCM)) {
        const uint32_t blockAlign = channels * bits / 8;
        out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
    }
    return Ok();
}

// Is there at least one Media Foundation encoder from `category` that turns the
// `subtype` output into something? The software encoders are sync MFTs, so the
// enumeration includes them; hardware ones are added where the machine has them.
Result<void> ProbeMft(const GUID &category, const GUID &majorType, const GUID &outSubtype,
                      std::string_view label)
{
    {
        auto acquired = AcquireMf();
        if (!acquired.ok())
            return acquired;
    }
    MFT_REGISTER_TYPE_INFO output{majorType, outSubtype};
    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(category,
                                 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE,
                                 nullptr, &output, &activates, &count);
    const bool found = SUCCEEDED(hr) && count > 0;
    if (activates) {
        for (UINT32 i = 0; i < count; ++i)
            if (activates[i])
                activates[i]->Release();
        CoTaskMemFree(activates);
    }
    ReleaseMf();
    if (!found)
        return Error::Make(Err::Recording_EncoderUnavailable, kModule,
                           std::string(label) + " encoder is not available through Media Foundation");
    return Ok();
}

} // namespace

// ---------------------------------------------------------------------------
// MediaFoundationMp4

struct MediaFoundationMp4::Impl {
    ComRef<IMFSinkWriter> writer;
    bool mfHeld = false;
    int videoStream = -1;
    int audioStream = -1;
    int width = 0;
    int height = 0;
    LONGLONG frameDuration = 0;
    uint32_t audioRate = 0;
    std::vector<uint8_t> nv12;
    std::vector<int16_t> pcm;
};

MediaFoundationMp4::MediaFoundationMp4() : impl_(std::make_unique<Impl>()) {}

MediaFoundationMp4::~MediaFoundationMp4()
{
    impl_->writer.Reset();
    if (impl_->mfHeld) {
        ReleaseMf();
        impl_->mfHeld = false;
    }
}

Result<std::string> MediaFoundationMp4::Open(const RecordingProfile &profile, std::string_view path)
{
    if (impl_->writer)
        return Error::Make(Err::Recording_InvalidState, kModule, "container is already open");
    if (profile.container != ContainerKind::MP4)
        return Error::Make(Err::Recording_ContainerNotFound, kModule, "this container writes MP4 only");
    if (profile.videoCodec != VideoCodec::H264)
        return Error::Make(Err::Recording_EncoderUnavailable, kModule,
                           "MP4 carries H.264 video in this build");

    // Even dimensions: H.264 4:2:0 cannot carry odd sizes.
    const int width = profile.width & ~1;
    const int height = profile.height & ~1;
    if (width < 2 || height < 2)
        return Error::Make(Err::Recording_ValidationFailed, kModule, "frame size is too small");
    const double fps = profile.fps > 0.0 ? profile.fps : 30.0;
    const auto fpsNum = static_cast<uint32_t>(std::llround(fps * 1000.0));

    // The audio stream exists when the mix is running and the profile asks for AAC.
    // The mix's rate is what the file is written at.
    const auto mix = audio::AudioEngine::Instance().MixFormat();
    const bool wantAudio = profile.audioCodec == AudioCodec::AAC && mix.rateHz > 0;
    if (wantAudio && mix.rateHz != 44100 && mix.rateHz != 48000)
        return Error::Make(Err::Recording_ValidationFailed, kModule,
                           "AAC records at 44.1 or 48 kHz; the output runs at " +
                               std::to_string(mix.rateHz) + " Hz");
    if (profile.audioCodec != AudioCodec::AAC && profile.audioCodec != AudioCodec::None)
        return Error::Make(Err::Recording_EncoderUnavailable, kModule,
                           "MP4 carries AAC audio in this build");

    auto acquired = AcquireMf();
    if (!acquired.ok())
        return acquired.error();
    impl_->mfHeld = true;

    const std::wstring wpath = ToWide(path);
    HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, nullptr, impl_->writer.Out());
    if (FAILED(hr)) {
        impl_->writer.Reset();
        ReleaseMf();
        impl_->mfHeld = false;
        return HrError("opening the MP4 writer", hr);
    }

    // Video: H.264 out, NV12 in.
    const uint32_t bitrate = static_cast<uint32_t>(std::max(1, profile.videoBitrateKbps)) * 1000u;
    ComRef<IMFMediaType> videoOut, videoIn;
    if (auto made = MakeVideoType(MFVideoFormat_H264, width, height, fpsNum, 1000, bitrate, videoOut); !made.ok())
        return made.error();
    if (auto made = MakeVideoType(MFVideoFormat_NV12, width, height, fpsNum, 1000, 0, videoIn); !made.ok())
        return made.error();
    DWORD videoIndex = 0;
    hr = impl_->writer->AddStream(videoOut.get(), &videoIndex);
    impl_->videoStream = static_cast<int>(videoIndex);
    if (FAILED(hr))
        return HrError("adding the video stream", hr);
    hr = impl_->writer->SetInputMediaType(static_cast<DWORD>(impl_->videoStream), videoIn.get(), nullptr);
    if (FAILED(hr))
        return HrError("setting the video input", hr);

    // Audio: AAC out, 16-bit PCM in, stereo.
    if (wantAudio) {
        ComRef<IMFMediaType> audioOut, audioIn;
        if (auto made = MakeAudioType(MFAudioFormat_AAC, mix.rateHz, 2, 16, kAacBytesPerSecond, audioOut); !made.ok())
            return made.error();
        if (auto made = MakeAudioType(MFAudioFormat_PCM, mix.rateHz, 2, 16, mix.rateHz * 4, audioIn); !made.ok())
            return made.error();
        DWORD audioIndex = 0;
        hr = impl_->writer->AddStream(audioOut.get(), &audioIndex);
        impl_->audioStream = static_cast<int>(audioIndex);
        if (FAILED(hr))
            return HrError("adding the audio stream", hr);
        hr = impl_->writer->SetInputMediaType(static_cast<DWORD>(impl_->audioStream), audioIn.get(), nullptr);
        if (FAILED(hr))
            return HrError("setting the audio input", hr);
    }

    hr = impl_->writer->BeginWriting();
    if (FAILED(hr))
        return HrError("starting the MP4 writer", hr);

    impl_->width = width;
    impl_->height = height;
    impl_->frameDuration = static_cast<LONGLONG>(std::llround(10000000.0 / fps));
    impl_->audioRate = wantAudio ? mix.rateHz : 0;
    return std::string(path);
}

Result<void> MediaFoundationMp4::Close(bool /*finalized*/)
{
    if (!impl_->writer)
        return Ok();
    // Finalize writes the MP4 index. Without it the file cannot be played, so a
    // segment boundary finalizes just as a stop does.
    const HRESULT hr = impl_->writer->Finalize();
    impl_->writer.Reset();
    impl_->videoStream = -1;
    impl_->audioStream = -1;
    impl_->audioRate = 0;
    if (impl_->mfHeld) {
        ReleaseMf();
        impl_->mfHeld = false;
    }
    if (FAILED(hr))
        return HrError("finalizing the MP4", hr);
    return Ok();
}

Result<void> MediaFoundationMp4::WriteVideo(const VideoFrameIn &frame)
{
    if (!impl_->writer || impl_->videoStream < 0)
        return Error::Make(Err::Recording_InvalidState, kModule, "container is not open");
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0)
        return Ok();

    const size_t bytes = static_cast<size_t>(impl_->width) * impl_->height * 3 / 2;
    impl_->nv12.resize(bytes);
    PackedToNv12(frame.pixels, frame.width, frame.height, impl_->nv12.data(),
                 impl_->width, impl_->height);

    ComRef<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), buffer.Out());
    if (FAILED(hr))
        return HrError("allocating a video buffer", hr);
    BYTE *dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr))
        return HrError("locking a video buffer", hr);
    std::memcpy(dst, impl_->nv12.data(), bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes));

    ComRef<IMFSample> sample;
    hr = MFCreateSample(sample.Out());
    if (FAILED(hr))
        return HrError("creating a video sample", hr);
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(ToHundredNs(frame.timestampMs));
    sample->SetSampleDuration(impl_->frameDuration);
    hr = impl_->writer->WriteSample(static_cast<DWORD>(impl_->videoStream), sample.get());
    if (FAILED(hr))
        return HrError("writing a video frame", hr);
    return Ok();
}

Result<void> MediaFoundationMp4::WriteAudio(const AudioBlockIn &block)
{
    if (!impl_->writer)
        return Error::Make(Err::Recording_InvalidState, kModule, "container is not open");
    // A file opened without audio (no mix was running) has no audio stream to fill.
    if (impl_->audioStream < 0 || block.frames == 0 || block.channels == 0)
        return Ok();
    if (block.rateHz != impl_->audioRate)
        return Error::Make(Err::Recording_ValidationFailed, kModule,
                           "the mix changed rate during the recording");

    // Stereo 16-bit PCM: a mono block is duplicated, more than two channels keeps the first two.
    const size_t frames = block.frames;
    impl_->pcm.resize(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        const float left = block.samples[f * block.channels];
        const float right = block.channels >= 2 ? block.samples[f * block.channels + 1] : left;
        impl_->pcm[f * 2] = ToPcm16(left);
        impl_->pcm[f * 2 + 1] = ToPcm16(right);
    }

    const size_t bytes = frames * 4;
    ComRef<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), buffer.Out());
    if (FAILED(hr))
        return HrError("allocating an audio buffer", hr);
    BYTE *dst = nullptr;
    hr = buffer->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr))
        return HrError("locking an audio buffer", hr);
    std::memcpy(dst, impl_->pcm.data(), bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes));

    ComRef<IMFSample> sample;
    hr = MFCreateSample(sample.Out());
    if (FAILED(hr))
        return HrError("creating an audio sample", hr);
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(ToHundredNs(block.timestampMs));
    sample->SetSampleDuration(static_cast<LONGLONG>(frames) * 10000000LL / impl_->audioRate);
    hr = impl_->writer->WriteSample(static_cast<DWORD>(impl_->audioStream), sample.get());
    if (FAILED(hr))
        return HrError("writing audio", hr);
    return Ok();
}

Result<void> ProbeH264Encoder()
{
    return ProbeMft(MFT_CATEGORY_VIDEO_ENCODER, MFMediaType_Video, MFVideoFormat_H264, "H.264");
}

Result<void> ProbeAacEncoder()
{
    return ProbeMft(MFT_CATEGORY_AUDIO_ENCODER, MFMediaType_Audio, MFAudioFormat_AAC, "AAC");
}

} // namespace bps::recording
