#include "modules/recording/WaveAudioContainer.hpp"

#include "modules/audio/AudioEngine.hpp"
#include "platform/PlatformAccessor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace bps::recording {
namespace {
constexpr const char* kModule = "WaveAudioContainer";
constexpr uint64_t kMaxDataBytes = std::numeric_limits<uint32_t>::max() - 36ull;

Error Fail(std::string message, ErrorCode code = Err::Recording_InvalidState)
{
    return Error::Make(code, kModule, std::move(message));
}

void Put16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xff);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void Put32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xff);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

Result<void> WriteAll(bps::platform::IFileWriteStream& stream,
                      const uint8_t* bytes, size_t size)
{
    size_t written = 0;
    while (written < size) {
        auto part = stream.Write(bytes + written, size - written);
        if (!part.ok()) return part.error();
        if (part.value() == 0)
            return Fail("the filesystem stream stopped accepting data");
        written += part.value();
    }
    return Ok();
}

int16_t ToPcm16(float sample)
{
    if (!std::isfinite(sample)) sample = 0.0f;
    const float clipped = std::clamp(sample, -1.0f, 1.0f);
    const long scaled = std::lround(clipped * (clipped < 0.0f ? 32768.0f : 32767.0f));
    return static_cast<int16_t>(scaled);
}

} // namespace

WaveAudioContainer::WaveAudioContainer() = default;

WaveAudioContainer::~WaveAudioContainer()
{
    if (stream_) (void)Close(false);
}

Result<std::string> WaveAudioContainer::Open(const RecordingProfile& profile,
                                             std::string_view path)
{
    if (stream_) return Fail("container is already open");
    if (profile.container != ContainerKind::WAV || profile.videoCodec != VideoCodec::None
        || profile.audioCodec != AudioCodec::PCM || profile.audioChannels != 2)
        return Fail("WAV supports audio-only PCM profiles", Err::Recording_ContainerNotFound);
    if (path.empty()) return Fail("output path is empty", Err::Recording_ValidationFailed);

    const auto mix = audio::AudioEngine::Instance().MixFormat();
    if (mix.rateHz == 0 || mix.rateHz > std::numeric_limits<uint32_t>::max() / 4u)
        return Fail("the output mix is not running", Err::Recording_ValidationFailed);

    path_.assign(path);
    auto opened = platform::PlatformAccessor::Get().Filesystem().OpenWriteStream(path_);
    if (!opened.ok()) return opened.error();
    stream_ = std::move(opened.value());

    rateHz_ = mix.rateHz;
    dataBytes_ = 0;
    std::array<uint8_t, 44> header{};
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    Put32(header.data() + 4, 0); // finalized at Close
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    Put32(header.data() + 16, 16); // PCM fmt chunk
    Put16(header.data() + 20, 1);  // PCM
    Put16(header.data() + 22, 2);  // stereo
    Put32(header.data() + 24, rateHz_);
    Put32(header.data() + 28, rateHz_ * 4u);
    Put16(header.data() + 32, 4);  // block alignment
    Put16(header.data() + 34, 16);
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    Put32(header.data() + 40, 0); // finalized at Close
    if (auto result = WriteAll(*stream_, header.data(), header.size()); !result.ok()) {
        (void)stream_->Close();
        stream_.reset();
        return result.error();
    }
    return path_;
}

Result<void> WaveAudioContainer::WriteAudio(const AudioBlockIn& block)
{
    if (!stream_) return Fail("container is not open");
    if (block.frames == 0) return Ok();
    if (!block.samples || block.channels == 0)
        return Fail("audio block has no samples", Err::Recording_ValidationFailed);
    if (block.rateHz != rateHz_)
        return Fail("the mix changed rate during the recording", Err::Recording_ValidationFailed);
    if (block.frames > std::numeric_limits<size_t>::max() / 4u
        || block.frames > std::numeric_limits<size_t>::max() / block.channels)
        return Fail("audio block is too large", Err::Recording_ValidationFailed);

    const size_t bytes = block.frames * 4u;
    if (bytes > kMaxDataBytes - dataBytes_)
        return Fail("WAV reached the RIFF 4 GiB size limit", Err::Recording_ValidationFailed);
    std::vector<uint8_t> pcm(bytes);
    for (size_t frame = 0; frame < block.frames; ++frame) {
        const float left = block.samples[frame * block.channels];
        const float right = block.channels >= 2 ? block.samples[frame * block.channels + 1] : left;
        const uint16_t l = static_cast<uint16_t>(ToPcm16(left));
        const uint16_t r = static_cast<uint16_t>(ToPcm16(right));
        Put16(pcm.data() + frame * 4u, l);
        Put16(pcm.data() + frame * 4u + 2u, r);
    }
    if (auto result = WriteAll(*stream_, pcm.data(), pcm.size()); !result.ok())
        return result.error();
    dataBytes_ += bytes;
    return Ok();
}

Result<void> WaveAudioContainer::Close(bool)
{
    if (!stream_) return Ok();
    std::array<uint8_t, 4> size{};
    Put32(size.data(), static_cast<uint32_t>(36u + dataBytes_));
    auto result = stream_->Seek(4);
    if (result.ok()) result = WriteAll(*stream_, size.data(), size.size());
    Put32(size.data(), static_cast<uint32_t>(dataBytes_));
    if (result.ok()) result = stream_->Seek(40);
    if (result.ok()) result = WriteAll(*stream_, size.data(), size.size());
    if (result.ok()) result = stream_->Flush();
    const Result<void> closed = stream_->Close();
    stream_.reset();
    rateHz_ = 0;
    if (!result.ok()) return result.error();
    return closed;
}

} // namespace bps::recording
