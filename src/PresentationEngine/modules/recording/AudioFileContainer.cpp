#include "modules/recording/AudioFileContainer.hpp"

#include "modules/audio/AudioEngine.hpp"
#include "modules/recording/MediaFoundationEncode.hpp"
#include "modules/recording/MfCommon.hpp"

#include <cstdio>

namespace bps::recording {

namespace {

using namespace mf;

constexpr const char* kModule = "AudioFileContainer";

Error Fail(std::string message, ErrorCode code = Err::Recording_InvalidState) {
    return Error::Make(code, kModule, std::move(message));
}

// MP3 bitrates the encoder offers (bytes per second): the profile's kbps rounds to the nearest.
uint32_t Mp3BytesPerSecond(int kbps) {
    const int choices[5] = {128, 160, 192, 256, 320};
    int best = choices[2];
    for (const int c : choices)
        if (std::abs(kbps - c) < std::abs(kbps - best)) best = c;
    return static_cast<uint32_t>(best) * 125u;
}

void Put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void Put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

} // namespace

struct AudioFileContainer::Impl {
    ContainerKind kind = ContainerKind::WAV;
    Session session;
    std::FILE* wav = nullptr;               // WAV
    ComRef<IMFSinkWriter> writer;            // M4A / MP3
    DWORD stream = 0;
    uint32_t rate = 0;
    uint64_t framesWritten = 0;
    std::vector<int16_t> pcm;
    bool open = false;
};

AudioFileContainer::AudioFileContainer(ContainerKind kind) : impl_(std::make_unique<Impl>()) { impl_->kind = kind; }

AudioFileContainer::~AudioFileContainer() {
    if (impl_->wav) std::fclose(impl_->wav);
}

const char* AudioFileContainer::Name() const noexcept {
    switch (impl_->kind) {
        case ContainerKind::M4A: return "m4a";
        case ContainerKind::MP3: return "mp3";
        default: return "wav";
    }
}

Result<std::string> AudioFileContainer::Open(const RecordingProfile& profile, std::string_view path) {
    Impl& I = *impl_;
    if (I.open) return Fail("container is already open");
    if (profile.container != I.kind)
        return Fail("this container writes a different format", Err::Recording_ContainerNotFound);

    const auto mix = audio::AudioEngine::Instance().MixFormat();
    if (mix.rateHz == 0)
        return Fail("the output mix is not running, so there is no sound to record", Err::Recording_ValidationFailed);
    if (I.kind != ContainerKind::WAV && mix.rateHz != 44100 && mix.rateHz != 48000)
        return Fail("this format records at 44.1 or 48 kHz; the output runs at " + std::to_string(mix.rateHz) + " Hz",
                    Err::Recording_ValidationFailed);
    I.rate = mix.rateHz;

    const int needed = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(0, needed)), L'\0');
    if (needed > 0) MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wpath.data(), needed);

    if (I.kind == ContainerKind::WAV) {
        I.wav = _wfopen(wpath.c_str(), L"w+b");
        if (!I.wav) return Fail("cannot create " + std::string(path));
        std::vector<uint8_t> header;
        header.insert(header.end(), {'R', 'I', 'F', 'F'});
        Put32(header, 0);   // patched at Close
        header.insert(header.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        Put32(header, 16);
        Put16(header, 1);   // PCM
        Put16(header, 2);   // stereo
        Put32(header, I.rate);
        Put32(header, I.rate * 4);
        Put16(header, 4);
        Put16(header, 16);
        header.insert(header.end(), {'d', 'a', 't', 'a'});
        Put32(header, 0);   // patched at Close
        if (std::fwrite(header.data(), 1, header.size(), I.wav) != header.size()) {
            std::fclose(I.wav);
            I.wav = nullptr;
            return Fail("writing the WAV header failed (is the disk full?)");
        }
    } else {
        if (!I.session.Acquire()) return Fail("Media Foundation did not start", Err::Recording_EncoderUnavailable);
        ComRef<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(attrs.Out(), 1))) return Fail("creating the writer settings failed");
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, I.kind == ContainerKind::MP3 ? MFTranscodeContainerType_MP3
                                                                                 : MFTranscodeContainerType_MPEG4);
        HRESULT hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs.get(), I.writer.Out());
        if (FAILED(hr)) return Fail(HrText("opening the audio writer", hr), Err::Recording_EncoderUnavailable);

        const bool mp3 = I.kind == ContainerKind::MP3;
        const uint32_t bytesPerSecond = mp3 ? Mp3BytesPerSecond(profile.audioBitrateKbps)
                                            : AacBytesPerSecond(profile.audioBitrateKbps);
        ComRef<IMFMediaType> outType, inType;
        if (FAILED(MFCreateMediaType(outType.Out())) || FAILED(MFCreateMediaType(inType.Out())))
            return Fail("creating the audio types failed");
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outType->SetGUID(MF_MT_SUBTYPE, mp3 ? MFAudioFormat_MP3 : MFAudioFormat_AAC);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, I.rate);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
        if (mp3) {
            outType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        } else {
            outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        }
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, I.rate);
        inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, I.rate * 4);
        if (FAILED(hr = I.writer->AddStream(outType.get(), &I.stream)))
            return Fail(HrText(mp3 ? "adding the MP3 stream" : "adding the AAC stream", hr), Err::Recording_EncoderUnavailable);
        if (FAILED(hr = I.writer->SetInputMediaType(I.stream, inType.get(), nullptr)))
            return Fail(HrText("setting the audio input", hr), Err::Recording_EncoderUnavailable);
        if (FAILED(hr = I.writer->BeginWriting())) return Fail(HrText("starting the audio writer", hr));
    }
    I.open = true;
    I.framesWritten = 0;
    return std::string(path);
}

Result<void> AudioFileContainer::WriteVideo(const VideoFrameIn&) { return Ok(); }

Result<void> AudioFileContainer::WriteAudio(const AudioBlockIn& block) {
    Impl& I = *impl_;
    if (!I.open) return Fail("container is not open");
    if (block.frames == 0 || block.channels == 0) return Ok();
    if (block.rateHz != I.rate)
        return Fail("the mix changed rate during the recording", Err::Recording_ValidationFailed);

    // Stereo 16-bit PCM: a mono block is duplicated, more than two channels keeps the first two.
    I.pcm.resize(block.frames * 2);
    for (size_t f = 0; f < block.frames; ++f) {
        const float left = block.samples[f * block.channels];
        const float right = block.channels >= 2 ? block.samples[f * block.channels + 1] : left;
        I.pcm[f * 2] = ToPcm16(left);
        I.pcm[f * 2 + 1] = ToPcm16(right);
    }
    const size_t bytes = block.frames * 4;
    if (I.kind == ContainerKind::WAV) {
        if (std::fwrite(I.pcm.data(), 1, bytes, I.wav) != bytes)
            return Fail("writing to the WAV file failed (is the disk full?)");
        I.framesWritten += block.frames;
        return Ok();
    }

    ComRef<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), buffer.Out());
    if (FAILED(hr)) return Fail(HrText("allocating audio", hr));
    BYTE* dst = nullptr;
    if (FAILED(hr = buffer->Lock(&dst, nullptr, nullptr))) return Fail(HrText("locking audio", hr));
    std::memcpy(dst, I.pcm.data(), bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes));
    ComRef<IMFSample> sample;
    if (FAILED(hr = MFCreateSample(sample.Out()))) return Fail(HrText("creating an audio sample", hr));
    sample->AddBuffer(buffer.get());
    // A continuous timeline from the frames written: a pause leaves no gap in the file.
    sample->SetSampleTime(static_cast<LONGLONG>(I.framesWritten) * 10000000LL / I.rate);
    sample->SetSampleDuration(static_cast<LONGLONG>(block.frames) * 10000000LL / I.rate);
    if (FAILED(hr = I.writer->WriteSample(I.stream, sample.get()))) return Fail(HrText("writing audio", hr));
    I.framesWritten += block.frames;
    return Ok();
}

Result<void> AudioFileContainer::Close(bool /*finalized*/) {
    Impl& I = *impl_;
    if (!I.open) return Ok();
    I.open = false;
    if (I.kind == ContainerKind::WAV) {
        const uint32_t dataBytes = static_cast<uint32_t>(std::min<uint64_t>(I.framesWritten * 4, 0xFFFFFFFFull - 36));
        std::vector<uint8_t> riff, data;
        Put32(riff, 36 + dataBytes);
        Put32(data, dataBytes);
        const bool ok = _fseeki64(I.wav, 4, SEEK_SET) == 0 && std::fwrite(riff.data(), 1, 4, I.wav) == 4 &&
                        _fseeki64(I.wav, 40, SEEK_SET) == 0 && std::fwrite(data.data(), 1, 4, I.wav) == 4 &&
                        std::fflush(I.wav) == 0;
        std::fclose(I.wav);
        I.wav = nullptr;
        if (!ok) return Fail("finishing the WAV file failed");
        return Ok();
    }
    const HRESULT hr = I.writer->Finalize();
    I.writer.Reset();
    if (FAILED(hr)) return Fail(HrText("finalizing the audio file", hr));
    return Ok();
}

bool Mp3EncoderAvailable() {
    Session session;
    if (!session.Acquire()) return false;
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Audio, MFAudioFormat_MP3};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
                                 nullptr, &output, &acts, &count);
    const bool found = SUCCEEDED(hr) && count > 0;
    if (acts) {
        for (UINT32 i = 0; i < count; ++i)
            if (acts[i]) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    return found;
}

} // namespace bps::recording
