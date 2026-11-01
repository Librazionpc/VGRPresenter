#include "modules/recording/MatroskaContainer.hpp"

#include "modules/audio/AudioEngine.hpp"
#include "modules/recording/MediaFoundationEncode.hpp"
#include "modules/recording/MfCommon.hpp"

#include <cstdio>
#include <deque>

namespace bps::recording {

namespace {

using namespace mf;
using Bytes = std::vector<uint8_t>;

constexpr const char* kModule = "MatroskaContainer";
constexpr int64_t kClusterTargetMs = 2000;       // a new cluster at the first keyframe after this
constexpr int64_t kClusterLimitMs = 30000;       // a relative timecode is 16 bits of milliseconds

Error Fail(std::string message, ErrorCode code = Err::Recording_InvalidState) {
    return Error::Make(code, kModule, std::move(message));
}

// ---- EBML --------------------------------------------------------------------------

void PutId(Bytes& b, uint32_t id) {
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<uint8_t>(id >> (8 * i)));
}

// The smallest EBML variable-length size that holds `size`.
void PutSize(Bytes& b, uint64_t size) {
    int n = 1;
    while (n < 8 && size >= ((uint64_t(1) << (7 * n)) - 1)) ++n;
    const uint64_t marked = size | (uint64_t(1) << (7 * n));
    for (int i = n - 1; i >= 0; --i) b.push_back(static_cast<uint8_t>(marked >> (8 * i)));
}

// Always eight bytes, so the value can be patched in place later.
void PutSize8(Bytes& b, uint64_t size) {
    b.push_back(0x01);
    for (int i = 6; i >= 0; --i) b.push_back(static_cast<uint8_t>(size >> (8 * i)));
}

Bytes Elem(uint32_t id, const Bytes& payload) {
    Bytes b;
    PutId(b, id);
    PutSize(b, payload.size());
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

Bytes UIntElem(uint32_t id, uint64_t value) {
    Bytes payload;
    int n = 1;
    while (n < 8 && (value >> (8 * n)) != 0) ++n;
    for (int i = n - 1; i >= 0; --i) payload.push_back(static_cast<uint8_t>(value >> (8 * i)));
    return Elem(id, payload);
}

Bytes UInt8Elem(uint32_t id, uint64_t value) {
    Bytes payload;
    for (int i = 7; i >= 0; --i) payload.push_back(static_cast<uint8_t>(value >> (8 * i)));
    return Elem(id, payload);
}

Bytes FloatElem(uint32_t id, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return UInt8Elem(id, bits);   // an 8-byte big-endian IEEE double has the same layout
}

Bytes StrElem(uint32_t id, std::string_view text) {
    return Elem(id, Bytes(text.begin(), text.end()));
}

Bytes Cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// Element ids.
constexpr uint32_t kEbml = 0x1A45DFA3, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB,
                   kSeekId = 0x53AB, kSeekPosition = 0x53AC, kInfo = 0x1549A966, kTracks = 0x1654AE6B,
                   kCluster = 0x1F43B675, kCues = 0x1C53BB6B;

} // namespace

struct MatroskaContainer::Impl {
    std::FILE* file = nullptr;
    std::string path;
    std::unique_ptr<H264Encoder> video;
    std::unique_ptr<AacEncoder> audio;
    int width = 0, height = 0;
    int64_t frameDurationHns = 0;
    uint32_t audioRate = 0;
    Bytes nv12;
    std::vector<int16_t> pcm;
    std::string encoderName;

    // File layout.
    bool headerWritten = false;
    int64_t segmentSizePos = 0;
    int64_t segmentStart = 0;
    int64_t infoPos = 0, tracksPos = 0;
    int64_t durationPos = 0;
    int64_t seekPositionPos[3] = {0, 0, 0};
    int64_t lastMs = 0;

    // The open cluster.
    bool clusterOpen = false;
    int64_t clusterSizePos = 0;
    int64_t clusterPos = 0;       // relative to segmentStart
    int64_t clusterMs = 0;
    struct Cue { int64_t ms; int64_t clusterPos; };
    std::vector<Cue> cues;

    std::deque<EncodedAudio> pendingAudio;   // sound that arrived before the file's head

    int64_t Tell() const { return _ftelli64(file); }
    bool SeekTo(int64_t at) const { return _fseeki64(file, at, SEEK_SET) == 0; }
    bool Put(const Bytes& b) const { return b.empty() || std::fwrite(b.data(), 1, b.size(), file) == b.size(); }

    void Patch8(int64_t at, uint64_t value) {
        const int64_t back = Tell();
        Bytes b;
        for (int i = 7; i >= 0; --i) b.push_back(static_cast<uint8_t>(value >> (8 * i)));
        if (SeekTo(at)) (void)Put(b);
        (void)SeekTo(back);
    }
    void PatchSize8(int64_t at, uint64_t size) {   // the 8-byte size vint
        Patch8(at, (uint64_t(1) << 56) | size);
    }

    Result<void> WriteHead(const Bytes& sps, const Bytes& pps) {
        // EBML header.
        Bytes ebml = Elem(kEbml, Cat({UIntElem(0x4286, 1), UIntElem(0x42F7, 1), UIntElem(0x42F2, 4),
                                      UIntElem(0x42F3, 8), StrElem(0x4282, "matroska"),
                                      UIntElem(0x4287, 4), UIntElem(0x4285, 2)}));
        if (!Put(ebml)) return Fail("writing the MKV header failed");

        // Segment, its size patched when the file closes.
        Bytes seg;
        PutId(seg, kSegment);
        segmentSizePos = Tell() + static_cast<int64_t>(seg.size());
        PutSize8(seg, 0x00FFFFFFFFFFFFFFull);   // "unknown" until the end
        if (!Put(seg)) return Fail("writing the MKV segment failed");
        segmentStart = Tell();

        // SeekHead: where Info, Tracks and Cues are (patched at the end).
        Bytes seekHead;
        const uint32_t ids[3] = {kInfo, kTracks, kCues};
        for (int i = 0; i < 3; ++i) {
            Bytes idBytes;
            PutId(idBytes, ids[i]);
            Bytes entry = Elem(kSeek, Cat({Elem(kSeekId, idBytes), UInt8Elem(kSeekPosition, 0)}));
            seekHead.insert(seekHead.end(), entry.begin(), entry.end());
        }
        Bytes seekElem = Elem(kSeekHead, seekHead);
        const int64_t seekStart = Tell();
        const size_t headerLen = seekElem.size() - seekHead.size();
        const size_t entryLen = seekHead.size() / 3;
        for (int i = 0; i < 3; ++i)
            seekPositionPos[i] = seekStart + static_cast<int64_t>(headerLen + i * entryLen + entryLen - 8);
        if (!Put(seekElem)) return Fail("writing the MKV seek head failed");

        // Info.
        infoPos = Tell() - segmentStart;
        Bytes infoPrefix = Cat({UIntElem(0x2AD7B1, 1000000), StrElem(0x4D80, "VGR Presenter"), StrElem(0x5741, "VGR Presenter")});
        Bytes info = Elem(kInfo, Cat({infoPrefix, FloatElem(0x4489, 0.0)}));
        const size_t infoHeader = info.size() - infoPrefix.size() - 11;   // id + size bytes of the Info element
        durationPos = Tell() + static_cast<int64_t>(infoHeader + infoPrefix.size() + 3);   // + id(2) + size(1)
        if (!Put(info)) return Fail("writing the MKV info failed");

        // Tracks: video (H.264 with an avcC) and, when there is sound, AAC.
        tracksPos = Tell() - segmentStart;
        Bytes avcc;
        avcc.push_back(1);                 // configurationVersion
        avcc.push_back(sps.size() > 1 ? sps[1] : 77);   // profile
        avcc.push_back(sps.size() > 2 ? sps[2] : 0);    // profile compatibility
        avcc.push_back(sps.size() > 3 ? sps[3] : 40);   // level
        avcc.push_back(0xFF);              // 4-byte NAL lengths
        avcc.push_back(0xE1);              // one SPS
        avcc.push_back(static_cast<uint8_t>(sps.size() >> 8));
        avcc.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
        avcc.insert(avcc.end(), sps.begin(), sps.end());
        avcc.push_back(1);                 // one PPS
        avcc.push_back(static_cast<uint8_t>(pps.size() >> 8));
        avcc.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
        avcc.insert(avcc.end(), pps.begin(), pps.end());

        Bytes videoTrack = Elem(0xAE, Cat({UIntElem(0xD7, 1), UIntElem(0x73C5, 1), UIntElem(0x83, 1),
                                           UIntElem(0x9C, 0), StrElem(0x86, "V_MPEG4/ISO/AVC"),
                                           Elem(0x63A2, avcc),
                                           Elem(0xE0, Cat({UIntElem(0xB0, static_cast<uint64_t>(width)),
                                                           UIntElem(0xBA, static_cast<uint64_t>(height))}))}));
        Bytes trackList = videoTrack;
        if (audio) {
            Bytes audioTrack = Elem(0xAE, Cat({UIntElem(0xD7, 2), UIntElem(0x73C5, 2), UIntElem(0x83, 2),
                                               UIntElem(0x9C, 0), StrElem(0x86, "A_AAC"),
                                               Elem(0x63A2, audio->AudioSpecificConfig()),
                                               Elem(0xE1, Cat({FloatElem(0xB5, static_cast<double>(audioRate)),
                                                               UIntElem(0x9F, 2)}))}));
            trackList.insert(trackList.end(), audioTrack.begin(), audioTrack.end());
        }
        if (!Put(Elem(kTracks, trackList))) return Fail("writing the MKV tracks failed");

        headerWritten = true;
        return Ok();
    }

    void CloseCluster() {
        if (!clusterOpen) return;
        const int64_t end = Tell();
        PatchSize8(clusterSizePos, static_cast<uint64_t>(end - (clusterSizePos + 8)));
        clusterOpen = false;
    }

    void OpenCluster(int64_t ms) {
        CloseCluster();
        clusterPos = Tell() - segmentStart;
        clusterMs = ms;
        Bytes head;
        PutId(head, kCluster);
        clusterSizePos = Tell() + static_cast<int64_t>(head.size());
        PutSize8(head, 0);
        Bytes tc = UIntElem(0xE7, static_cast<uint64_t>(std::max<int64_t>(0, ms)));
        head.insert(head.end(), tc.begin(), tc.end());
        (void)Put(head);
        clusterOpen = true;
    }

    Result<void> WriteBlock(uint8_t track, int64_t ms, bool key, const Bytes& data) {
        ms = std::max<int64_t>(0, ms);
        const int64_t rel = ms - clusterMs;
        const bool start = !clusterOpen || rel > kClusterLimitMs || rel < -1000 ||
                           (track == 1 && key && rel >= kClusterTargetMs);
        if (start) OpenCluster(ms);
        if (track == 1 && key) cues.push_back({ms, clusterPos});
        const int64_t relative = ms - clusterMs;

        Bytes block;
        PutId(block, 0xA3);   // SimpleBlock
        PutSize8(block, data.size() + 4);
        block.push_back(static_cast<uint8_t>(0x80 | track));
        block.push_back(static_cast<uint8_t>((relative >> 8) & 0xFF));
        block.push_back(static_cast<uint8_t>(relative & 0xFF));
        block.push_back(key ? 0x80 : 0x00);
        if (!Put(block) || !Put(data)) return Fail("writing to the MKV file failed (is the disk full?)");
        lastMs = std::max(lastMs, ms);
        return Ok();
    }

    // Length-prefixed NAL units from an Annex B picture; the parameter sets and access
    // unit delimiter live in the track header / are not needed.
    static Bytes ToAvc(const EncodedVideo& pic) {
        Bytes out;
        for (const auto& nal : SplitAnnexB(pic.data.data(), pic.data.size())) {
            const int type = NalType(pic.data.data() + nal.first);
            if (type == 7 || type == 8 || type == 9) continue;
            out.push_back(static_cast<uint8_t>(nal.second >> 24));
            out.push_back(static_cast<uint8_t>(nal.second >> 16));
            out.push_back(static_cast<uint8_t>(nal.second >> 8));
            out.push_back(static_cast<uint8_t>(nal.second));
            out.insert(out.end(), pic.data.begin() + static_cast<std::ptrdiff_t>(nal.first),
                       pic.data.begin() + static_cast<std::ptrdiff_t>(nal.first + nal.second));
        }
        return out;
    }

    Result<void> WriteAudioFrame(const EncodedAudio& a) {
        return WriteBlock(2, a.timeHns / 10000, true, a.data);
    }

    Result<void> HandleVideo(const EncodedVideo& pic) {
        if (!headerWritten) {
            if (!pic.keyframe) return Ok();   // nothing can decode until a keyframe
            Bytes sps, pps;
            for (const auto& nal : SplitAnnexB(pic.data.data(), pic.data.size())) {
                const int type = NalType(pic.data.data() + nal.first);
                if (type == 7 && sps.empty())
                    sps.assign(pic.data.begin() + static_cast<std::ptrdiff_t>(nal.first),
                               pic.data.begin() + static_cast<std::ptrdiff_t>(nal.first + nal.second));
                if (type == 8 && pps.empty())
                    pps.assign(pic.data.begin() + static_cast<std::ptrdiff_t>(nal.first),
                               pic.data.begin() + static_cast<std::ptrdiff_t>(nal.first + nal.second));
            }
            if (sps.empty() || pps.empty())
                return Fail(encoderName + " produced a keyframe without its parameter sets");
            if (auto head = WriteHead(sps, pps); !head.ok()) return head;
            while (!pendingAudio.empty()) {
                if (auto w = WriteAudioFrame(pendingAudio.front()); !w.ok()) return w;
                pendingAudio.pop_front();
            }
        }
        return WriteBlock(1, pic.timeHns / 10000, pic.keyframe, ToAvc(pic));
    }

    Result<void> HandleAudio(const EncodedAudio& a) {
        if (!headerWritten) {
            pendingAudio.push_back(a);
            // Bounded: a keyframe is two seconds away at most; keep a little more than that.
            while (pendingAudio.size() > 600) pendingAudio.pop_front();
            return Ok();
        }
        return WriteAudioFrame(a);
    }
};

MatroskaContainer::MatroskaContainer() : impl_(std::make_unique<Impl>()) {}

MatroskaContainer::~MatroskaContainer() {
    if (impl_->file) {
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
}

std::string MatroskaContainer::EncoderName() const { return impl_->encoderName; }

Result<std::string> MatroskaContainer::Open(const RecordingProfile& profile, std::string_view path) {
    Impl& I = *impl_;
    if (I.file) return Fail("container is already open");
    if (profile.container != ContainerKind::MKV)
        return Fail("this container writes MKV only", Err::Recording_ContainerNotFound);
    if (profile.videoCodec != VideoCodec::H264)
        return Fail("MKV carries H.264 video in this build", Err::Recording_EncoderUnavailable);
    if (profile.audioCodec != AudioCodec::AAC && profile.audioCodec != AudioCodec::None)
        return Fail("MKV carries AAC audio in this build", Err::Recording_EncoderUnavailable);

    const int width = profile.width & ~1;
    const int height = profile.height & ~1;
    if (width < 2 || height < 2) return Fail("frame size is too small", Err::Recording_ValidationFailed);
    const double fps = profile.fps > 0.0 ? profile.fps : 30.0;
    const auto fpsNum = static_cast<uint32_t>(std::llround(fps * 1000.0));

    const auto mix = audio::AudioEngine::Instance().MixFormat();
    const bool wantAudio = profile.audioCodec == AudioCodec::AAC && mix.rateHz > 0;
    if (wantAudio && mix.rateHz != 44100 && mix.rateHz != 48000)
        return Fail("AAC records at 44.1 or 48 kHz; the output runs at " + std::to_string(mix.rateHz) + " Hz",
                    Err::Recording_ValidationFailed);

    // The encoder the profile asked for. "Auto" takes the best the machine has and falls
    // back to software; a specific choice that fails is refused, never swapped.
    const EncoderKind kind = ResolveEncoderKind(profile.encoderPreference);
    if (auto made = H264Encoder::Create(kind, width, height, fpsNum, profile.videoBitrateKbps, I.video); !made.ok()) {
        if (profile.encoderPreference != EncoderKind::Auto || kind == EncoderKind::Software) return made.error();
        if (auto soft = H264Encoder::Create(EncoderKind::Software, width, height, fpsNum, profile.videoBitrateKbps, I.video);
            !soft.ok())
            return soft.error();
    }
    I.encoderName = I.video->Name();
    if (wantAudio) {
        if (auto made = AacEncoder::Create(mix.rateHz, AacBytesPerSecond(profile.audioBitrateKbps), I.audio); !made.ok()) return made.error();
        I.audioRate = mix.rateHz;
    }

    const int needed = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(0, needed)), L'\0');
    if (needed > 0)
        MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wpath.data(), needed);
    I.file = _wfopen(wpath.c_str(), L"w+b");
    if (!I.file) {
        I.video.reset();
        I.audio.reset();
        return Fail("cannot create " + std::string(path), Err::Recording_InvalidState);
    }
    I.path = std::string(path);
    I.width = width;
    I.height = height;
    I.frameDurationHns = static_cast<int64_t>(std::llround(10000000.0 / fps));
    return std::string(path);
}

Result<void> MatroskaContainer::WriteVideo(const VideoFrameIn& frame) {
    Impl& I = *impl_;
    if (!I.file || !I.video) return Fail("container is not open");
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0) return Ok();

    const size_t bytes = static_cast<size_t>(I.width) * I.height * 3 / 2;
    I.nv12.resize(bytes);
    PackedToNv12(frame.pixels, frame.width, frame.height, I.nv12.data(), I.width, I.height);
    std::vector<EncodedVideo> pictures;
    if (auto encoded = I.video->Encode(I.nv12.data(), bytes, MsToHns(frame.timestampMs), I.frameDurationHns, pictures);
        !encoded.ok())
        return encoded;
    for (const EncodedVideo& pic : pictures)
        if (auto w = I.HandleVideo(pic); !w.ok()) return w;
    return Ok();
}

Result<void> MatroskaContainer::WriteAudio(const AudioBlockIn& block) {
    Impl& I = *impl_;
    if (!I.file) return Fail("container is not open");
    if (!I.audio || block.frames == 0 || block.channels == 0) return Ok();
    if (block.rateHz != I.audioRate)
        return Fail("the mix changed rate during the recording", Err::Recording_ValidationFailed);

    I.pcm.resize(block.frames * 2);
    for (size_t f = 0; f < block.frames; ++f) {
        const float left = block.samples[f * block.channels];
        const float right = block.channels >= 2 ? block.samples[f * block.channels + 1] : left;
        I.pcm[f * 2] = ToPcm16(left);
        I.pcm[f * 2 + 1] = ToPcm16(right);
    }
    std::vector<EncodedAudio> frames;
    if (auto encoded = I.audio->Encode(I.pcm.data(), block.frames, MsToHns(block.timestampMs), frames); !encoded.ok())
        return encoded;
    for (const EncodedAudio& a : frames)
        if (auto w = I.HandleAudio(a); !w.ok()) return w;
    return Ok();
}

Result<void> MatroskaContainer::Close(bool /*finalized*/) {
    Impl& I = *impl_;
    if (!I.file) return Ok();

    std::string failure;
    // Whatever the encoders still hold goes into the file first.
    if (I.video) {
        std::vector<EncodedVideo> tail;
        (void)I.video->Drain(tail);
        for (const EncodedVideo& pic : tail)
            if (auto w = I.HandleVideo(pic); !w.ok() && failure.empty()) failure = w.error().message;
    }
    if (I.audio) {
        std::vector<EncodedAudio> tail;
        (void)I.audio->Drain(tail);
        for (const EncodedAudio& a : tail)
            if (auto w = I.HandleAudio(a); !w.ok() && failure.empty()) failure = w.error().message;
    }

    if (!I.headerWritten) {
        std::fclose(I.file);
        I.file = nullptr;
        I.video.reset();
        I.audio.reset();
        return Fail("no picture reached the MKV file, so there is nothing to play", Err::Recording_ValidationFailed);
    }

    I.CloseCluster();
    // Cues: one per keyframe, so a player can jump.
    const int64_t cuesPos = I.Tell() - I.segmentStart;
    Bytes points;
    for (const auto& cue : I.cues) {
        Bytes point = Elem(0xBB, Cat({UIntElem(0xB3, static_cast<uint64_t>(cue.ms)),
                                      Elem(0xB7, Cat({UIntElem(0xF7, 1), UIntElem(0xF1, static_cast<uint64_t>(cue.clusterPos))}))}));
        points.insert(points.end(), point.begin(), point.end());
    }
    if (!points.empty() && !I.Put(Elem(kCues, points)) && failure.empty()) failure = "writing the MKV cues failed";
    const int64_t end = I.Tell();

    // The numbers only known now.
    I.PatchSize8(I.segmentSizePos, static_cast<uint64_t>(end - I.segmentStart));
    {
        const double duration = static_cast<double>(I.lastMs);
        uint64_t bits = 0;
        std::memcpy(&bits, &duration, sizeof bits);
        I.Patch8(I.durationPos, bits);
    }
    I.Patch8(I.seekPositionPos[0], static_cast<uint64_t>(I.infoPos));
    I.Patch8(I.seekPositionPos[1], static_cast<uint64_t>(I.tracksPos));
    I.Patch8(I.seekPositionPos[2], static_cast<uint64_t>(points.empty() ? I.tracksPos : cuesPos));

    const bool flushed = std::fflush(I.file) == 0;
    std::fclose(I.file);
    I.file = nullptr;
    I.video.reset();
    I.audio.reset();
    if (!failure.empty()) return Fail(failure);
    if (!flushed) return Fail("the MKV file could not be flushed to disk");
    return Ok();
}

} // namespace bps::recording
