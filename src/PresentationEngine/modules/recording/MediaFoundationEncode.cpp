#include "modules/recording/MediaFoundationEncode.hpp"

#include "modules/recording/MfCommon.hpp"
#include "modules/recording/RecordingEngine.hpp"
#include "modules/recording/RecordingPlugin.hpp"

#include <strmif.h>   // ICodecAPI

#include <chrono>
#include <cstdlib>
#include <utility>
#include <mutex>
#include <thread>

namespace bps::recording {
namespace {
using namespace mf;

constexpr const char* kModule = "MediaFoundationEncode";

Error Fail(std::string message, ErrorCode code = Err::Recording_EncoderUnavailable) {
    return Error::Make(code, kModule, std::move(message));
}

// The codec properties the encoders are tuned with (codecapi.h, spelled here as plain
// GUIDs so nothing has to link the DirectShow GUID library for them).
constexpr GUID kIidCodecApi = {0x901db4c7, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};
constexpr GUID kRateControlMode = {0x1c0608e9, 0x370c, 0x4710, {0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23}};
constexpr GUID kMeanBitRate = {0xf7222374, 0x2144, 0x4815, {0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52}};
constexpr GUID kGopSize = {0x95f31b26, 0x95a4, 0x41aa, {0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1}};
constexpr GUID kBPictureCount = {0x8d390aac, 0xdc5c, 0x4200, {0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2}};
constexpr GUID kLowLatency = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

// COM on the calling thread for the lifetime of the scope (the engine's worker threads
// are not guaranteed to have it). A thread already in another apartment mode is fine.
class ComScope {
public:
    ComScope() { hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() {
        if (hr_ == S_OK || hr_ == S_FALSE) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    HRESULT hr_ = E_FAIL;
};

void SetCodecU32(ICodecAPI* api, const GUID& key, ULONG value) {
    if (!api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    (void)api->SetValue(&key, &v);   // every property is a hint: an encoder without it keeps its default
}

void SetCodecBool(ICodecAPI* api, const GUID& key, bool value) {
    if (!api) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    (void)api->SetValue(&key, &v);
}

std::string Narrow(const wchar_t* text, UINT32 length) {
    if (!text || length == 0) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, needed)), '\0');
    if (needed > 0)
        WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), out.data(), needed, nullptr, nullptr);
    return out;
}

// The vendor id the MFT enumeration publishes for a hardware encoder: "VEN_10DE".
EncoderKind KindForVendor(const std::string& vendor) {
    const auto at = vendor.find("VEN_");
    if (at == std::string::npos || vendor.size() < at + 8) return EncoderKind::Auto;
    const unsigned long id = std::strtoul(vendor.substr(at + 4, 4).c_str(), nullptr, 16);
    switch (id) {
        case 0x10DE: return EncoderKind::NVENC;
        case 0x1002: return EncoderKind::AMF;
        case 0x8086: return EncoderKind::QuickSync;
        default: return EncoderKind::Auto;
    }
}

struct HardwareEntry {
    EncoderKind kind = EncoderKind::Auto;
    std::string name;
};

void ReleaseActivates(IMFActivate** acts, UINT32 count) {
    if (!acts) return;
    for (UINT32 i = 0; i < count; ++i)
        if (acts[i]) acts[i]->Release();
    CoTaskMemFree(acts);
}

std::string ActivateString(IMFActivate* act, const GUID& key) {
    wchar_t* text = nullptr;
    UINT32 length = 0;
    if (FAILED(act->GetAllocatedString(key, &text, &length)) || !text) return {};
    std::string out = Narrow(text, length);
    CoTaskMemFree(text);
    return out;
}

const std::vector<HardwareEntry>& HardwareEntries() {
    static std::vector<HardwareEntry> entries;
    static std::once_flag once;
    std::call_once(once, [] {
        ComScope com;
        Session session;
        if (!session.Acquire()) return;
        MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                     MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                     nullptr, &output, &acts, &count);
        if (SUCCEEDED(hr)) {
            for (UINT32 i = 0; i < count; ++i) {
                const EncoderKind kind = KindForVendor(ActivateString(acts[i], MFT_ENUM_HARDWARE_VENDOR_ID_Attribute));
                if (kind == EncoderKind::Auto) continue;
                const bool seen = std::any_of(entries.begin(), entries.end(),
                                              [kind](const HardwareEntry& e) { return e.kind == kind; });
                if (!seen) entries.push_back({kind, ActivateString(acts[i], MFT_FRIENDLY_NAME_Attribute)});
            }
        }
        ReleaseActivates(acts, count);
    });
    return entries;
}

// The marker the engine's encoder registry holds for a hardware kind. The pictures are
// encoded by H264Encoder; this says "the machine has it".
class HardwareEncoderPlugin final : public IVideoEncoder {
public:
    explicit HardwareEncoderPlugin(EncoderKind kind) : kind_(kind) {}
    const char* Codec() const noexcept override { return "H.264"; }
    EncoderKind Kind() const noexcept override { return kind_; }
    Result<void> Initialize(const RecordingProfile&) override {
        const auto kinds = HardwareEncoderKinds();
        if (std::find(kinds.begin(), kinds.end(), kind_) == kinds.end())
            return Fail("this machine has no such hardware encoder");
        return Ok();
    }
    Result<void> Shutdown() override { return Ok(); }

private:
    EncoderKind kind_;
};

const char* RegistryName(EncoderKind kind) {
    switch (kind) {
        case EncoderKind::NVENC: return "nvenc";
        case EncoderKind::AMF: return "amf";
        case EncoderKind::QuickSync: return "quicksync";
        default: return "software";
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Machine capabilities

std::vector<EncoderKind> HardwareEncoderKinds() {
    std::vector<EncoderKind> out;
    for (const HardwareEntry& e : HardwareEntries()) out.push_back(e.kind);
    return out;
}

std::string HardwareEncoderName(EncoderKind kind) {
    for (const HardwareEntry& e : HardwareEntries())
        if (e.kind == kind) return e.name;
    return {};
}

EncoderKind ResolveEncoderKind(EncoderKind preference) {
    if (preference != EncoderKind::Auto) return preference;
    const auto kinds = HardwareEncoderKinds();
    for (const EncoderKind candidate : {EncoderKind::NVENC, EncoderKind::AMF, EncoderKind::QuickSync})
        if (std::find(kinds.begin(), kinds.end(), candidate) != kinds.end()) return candidate;
    return EncoderKind::Software;
}

size_t RegisterHardwareEncoders(RecordingEngine& engine) {
    size_t count = 0;
    for (const EncoderKind kind : HardwareEncoderKinds()) {
        ++count;
        (void)engine.RegisterVideoEncoder(kind, [kind]() -> std::shared_ptr<IVideoEncoder> {
            return std::make_shared<HardwareEncoderPlugin>(kind);
        });   // AlreadyExists on a second call is the answer we want
    }
    return count;
}

std::vector<std::pair<size_t, size_t>> SplitAnnexB(const uint8_t* data, size_t size) {
    std::vector<std::pair<size_t, size_t>> nals;
    size_t i = 0;
    size_t start = std::string::npos;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (start != std::string::npos) {
                size_t end = i;
                while (end > start && data[end - 1] == 0) --end;   // the zero of a 4-byte start code
                if (end > start) nals.emplace_back(start, end - start);
            }
            start = i + 3;
            i += 3;
        } else {
            ++i;
        }
    }
    if (start != std::string::npos && start < size) nals.emplace_back(start, size - start);
    return nals;
}

// ---------------------------------------------------------------------------
// H264Encoder

struct H264Encoder::Impl {
    ComScope com;
    Session session;
    ComRef<IMFTransform> mft;
    ComRef<IMFMediaEventGenerator> events;
    EncoderKind kind = EncoderKind::Software;
    std::string name;
    bool async = false;
    bool providesSamples = false;
    DWORD inId = 0;
    DWORD outId = 0;
    DWORD outBytes = 0;
    int needInput = 0;
    bool drained = false;
    size_t inputBytes = 0;

    ~Impl() {
        if (mft) {
            (void)mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            mft.Reset();
        }
        events.Reset();
    }

    // 1 = a picture was produced, 0 = the encoder wants input, 2 = the output format
    // changed (ask again), negative = failure (the HRESULT is in `hr`).
    int PullOne(std::vector<EncodedVideo>& out, HRESULT& hr) {
        MFT_OUTPUT_DATA_BUFFER buffer{};
        buffer.dwStreamID = outId;
        ComRef<IMFSample> own;
        if (!providesSamples) {
            ComRef<IMFMediaBuffer> media;
            if (FAILED(hr = MFCreateSample(own.Out())) ||
                FAILED(hr = MFCreateMemoryBuffer(std::max<DWORD>(outBytes, 1u << 20), media.Out())) ||
                FAILED(hr = own->AddBuffer(media.get())))
                return -1;
            buffer.pSample = own.get();
        }
        DWORD status = 0;
        hr = mft->ProcessOutput(0, 1, &buffer, &status);
        if (buffer.pEvents) buffer.pEvents->Release();
        IMFSample* produced = buffer.pSample;
        const bool ownedByMft = providesSamples && produced;
        struct Releaser {
            IMFSample* s;
            ~Releaser() { if (s) s->Release(); }
        } release{ownedByMft ? produced : nullptr};

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { hr = S_OK; return 0; }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComRef<IMFMediaType> type;
            if (SUCCEEDED(mft->GetOutputAvailableType(outId, 0, type.Out())))
                (void)mft->SetOutputType(outId, type.get(), 0);
            hr = S_OK;
            return 2;
        }
        if (FAILED(hr)) return -1;
        if (!produced) { hr = S_OK; return 0; }

        ComRef<IMFMediaBuffer> contiguous;
        if (FAILED(hr = produced->ConvertToContiguousBuffer(contiguous.Out()))) return -1;
        BYTE* bytes = nullptr;
        DWORD length = 0;
        if (FAILED(hr = contiguous->Lock(&bytes, nullptr, &length))) return -1;
        EncodedVideo picture;
        picture.data.assign(bytes, bytes + length);
        contiguous->Unlock();
        LONGLONG time = 0, duration = 0;
        (void)produced->GetSampleTime(&time);
        (void)produced->GetSampleDuration(&duration);
        picture.timeHns = time;
        picture.durationHns = duration;
        for (const auto& nal : SplitAnnexB(picture.data.data(), picture.data.size()))
            if (NalType(picture.data.data() + nal.first) == 5) picture.keyframe = true;
        out.push_back(std::move(picture));
        hr = S_OK;
        return 1;
    }

    void PullAll(std::vector<EncodedVideo>& out, HRESULT& hr) {
        for (;;) {
            const int r = PullOne(out, hr);
            if (r == 1 || r == 2) continue;
            return;
        }
    }

    // One event from the async encoder. 1 = handled one, 0 = none waiting, -1 = failure.
    int PollEvent(std::vector<EncodedVideo>& out, HRESULT& hr) {
        ComRef<IMFMediaEvent> event;
        hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.Out());
        if (hr == MF_E_NO_EVENTS_AVAILABLE) { hr = S_OK; return 0; }
        if (FAILED(hr)) return -1;
        MediaEventType type = MEUnknown;
        (void)event->GetType(&type);
        switch (type) {
            case METransformNeedInput: ++needInput; break;
            case METransformHaveOutput: {
                const int r = PullOne(out, hr);
                if (r < 0) return -1;
                break;
            }
            case METransformDrainComplete: drained = true; break;
            case MEError: {
                HRESULT status = E_FAIL;
                (void)event->GetStatus(&status);
                hr = status;
                return -1;
            }
            default: break;
        }
        return 1;
    }
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;
EncoderKind H264Encoder::Kind() const { return impl_->kind; }
const std::string& H264Encoder::Name() const { return impl_->name; }

Result<void> H264Encoder::Create(EncoderKind kind, int width, int height, uint32_t fpsNum,
                                 int bitrateKbps, std::unique_ptr<H264Encoder>& out) {
    if (kind == EncoderKind::Auto) kind = ResolveEncoderKind(kind);
    if (width < 2 || height < 2 || (width & 1) || (height & 1))
        return Fail("H.264 needs an even frame size", Err::Recording_ValidationFailed);

    std::unique_ptr<H264Encoder> enc(new H264Encoder());
    Impl& I = *enc->impl_;
    if (!I.session.Acquire()) return Fail("Media Foundation did not start");
    I.kind = kind;

    // Find the encoder: a hardware one by its vendor, the software one by exclusion.
    const bool hardware = kind != EncoderKind::Software;
    if (hardware) {
        const auto kinds = HardwareEncoderKinds();
        if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
            return Fail("this machine has no " + std::string(RegistryName(kind)) + " hardware encoder");
    }
    MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    const UINT32 flags = hardware ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
                                  : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER);
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outputInfo, &acts, &count);
    if (FAILED(hr)) return Fail(HrText("listing the H.264 encoders", hr));
    IMFActivate* chosen = nullptr;
    for (UINT32 i = 0; i < count && !chosen; ++i) {
        if (!hardware || KindForVendor(ActivateString(acts[i], MFT_ENUM_HARDWARE_VENDOR_ID_Attribute)) == kind)
            chosen = acts[i];
    }
    if (!chosen) {
        ReleaseActivates(acts, count);
        return Fail("no " + std::string(RegistryName(kind)) + " H.264 encoder was found");
    }
    I.name = ActivateString(chosen, MFT_FRIENDLY_NAME_Attribute);
    hr = chosen->ActivateObject(IID_PPV_ARGS(I.mft.Out()));
    ReleaseActivates(acts, count);
    if (FAILED(hr)) return Fail(HrText("starting " + I.name, hr));

    // An asynchronous MFT (every hardware encoder) has to be unlocked, and is driven by events.
    ComRef<IMFAttributes> attrs;
    if (SUCCEEDED(I.mft->GetAttributes(attrs.Out())) && attrs) {
        UINT32 async = 0;
        if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
            I.async = true;
            (void)attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }
        (void)attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    if (I.async && FAILED(hr = I.mft->QueryInterface(IID_PPV_ARGS(I.events.Out()))))
        return Fail(HrText("reading " + I.name + "'s events", hr));

    DWORD inIds[1] = {0}, outIds[1] = {0};
    if (SUCCEEDED(I.mft->GetStreamIDs(1, inIds, 1, outIds))) {
        I.inId = inIds[0];
        I.outId = outIds[0];
    }

    // The picture format: H.264 out, NV12 in, at the profile's size and rate.
    const uint32_t bitrate = static_cast<uint32_t>(std::max(1, bitrateKbps)) * 1000u;
    ComRef<IMFMediaType> outType;
    if (FAILED(hr = MFCreateMediaType(outType.Out()))) return Fail(HrText("creating the output type", hr));
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, 77);   // eAVEncH264VProfile_Main
    MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, fpsNum, 1000);
    MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr = I.mft->SetOutputType(I.outId, outType.get(), 0)))
        return Fail(HrText(I.name + " refused the output format", hr));

    ComRef<IMFMediaType> inType;
    if (FAILED(hr = MFCreateMediaType(inType.Out()))) return Fail(HrText("creating the input type", hr));
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inType.get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
    MFSetAttributeRatio(inType.get(), MF_MT_FRAME_RATE, fpsNum, 1000);
    MFSetAttributeRatio(inType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr = I.mft->SetInputType(I.inId, inType.get(), 0)))
        return Fail(HrText(I.name + " refused NV12 input", hr));

    // Tuning: constant bitrate, a keyframe every two seconds, no B-frames (a picture's
    // time is then its decode time, which the containers rely on), low latency.
    ComRef<ICodecAPI> api;
    if (SUCCEEDED(I.mft->QueryInterface(kIidCodecApi, reinterpret_cast<void**>(api.Out())))) {
        SetCodecU32(api.get(), kRateControlMode, 0);   // eAVEncCommonRateControlMode_CBR
        SetCodecU32(api.get(), kMeanBitRate, bitrate);
        SetCodecU32(api.get(), kGopSize, static_cast<ULONG>(std::max(1.0, std::round(fpsNum / 1000.0 * 2.0))));
        SetCodecU32(api.get(), kBPictureCount, 0);
        SetCodecBool(api.get(), kLowLatency, true);
    }

    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(I.mft->GetOutputStreamInfo(I.outId, &info))) {
        I.outBytes = info.cbSize;
        I.providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    }
    I.inputBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;

    if (FAILED(hr = I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(hr = I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
        return Fail(HrText("starting " + I.name, hr));

    out = std::move(enc);
    return Ok();
}

Result<void> H264Encoder::Encode(const uint8_t* nv12, size_t bytes, int64_t timeHns, int64_t durationHns,
                                 std::vector<EncodedVideo>& out) {
    Impl& I = *impl_;
    if (!I.mft) return Fail("the encoder is closed", Err::Recording_InvalidState);
    if (bytes < I.inputBytes) return Fail("a picture is smaller than the encoder's frame", Err::Recording_ValidationFailed);

    ComRef<IMFMediaBuffer> media;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(I.inputBytes), media.Out());
    if (FAILED(hr)) return Fail(HrText("allocating a picture", hr), Err::Recording_InvalidState);
    BYTE* dst = nullptr;
    if (FAILED(hr = media->Lock(&dst, nullptr, nullptr))) return Fail(HrText("locking a picture", hr), Err::Recording_InvalidState);
    std::memcpy(dst, nv12, I.inputBytes);
    media->Unlock();
    media->SetCurrentLength(static_cast<DWORD>(I.inputBytes));
    ComRef<IMFSample> sample;
    if (FAILED(hr = MFCreateSample(sample.Out()))) return Fail(HrText("creating a sample", hr), Err::Recording_InvalidState);
    sample->AddBuffer(media.get());
    sample->SetSampleTime(timeHns);
    sample->SetSampleDuration(durationHns);

    if (I.async) {
        // The encoder asks for each picture by event: wait for the request, hand it over,
        // then collect whatever it already finished.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (I.needInput == 0) {
            const int r = I.PollEvent(out, hr);
            if (r < 0) return Fail(HrText(I.name + " reported an error", hr), Err::Recording_InvalidState);
            if (r == 0) {
                if (std::chrono::steady_clock::now() > deadline)
                    return Fail(I.name + " stopped responding", Err::Recording_InvalidState);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (FAILED(hr = I.mft->ProcessInput(I.inId, sample.get(), 0)))
            return Fail(HrText("giving " + I.name + " a picture", hr), Err::Recording_InvalidState);
        --I.needInput;
        for (;;) {
            const int r = I.PollEvent(out, hr);
            if (r < 0) return Fail(HrText(I.name + " reported an error", hr), Err::Recording_InvalidState);
            if (r == 0) break;
        }
        return Ok();
    }

    // A synchronous encoder: feed it, then take everything it has.
    hr = I.mft->ProcessInput(I.inId, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        I.PullAll(out, hr);
        hr = I.mft->ProcessInput(I.inId, sample.get(), 0);
    }
    if (FAILED(hr)) return Fail(HrText("giving " + I.name + " a picture", hr), Err::Recording_InvalidState);
    I.PullAll(out, hr);
    if (FAILED(hr)) return Fail(HrText("reading " + I.name + "'s output", hr), Err::Recording_InvalidState);
    return Ok();
}

Result<void> H264Encoder::Drain(std::vector<EncodedVideo>& out) {
    Impl& I = *impl_;
    if (!I.mft) return Ok();
    HRESULT hr = S_OK;
    if (I.async) {
        (void)I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        (void)I.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!I.drained) {
            const int r = I.PollEvent(out, hr);
            if (r < 0) return Fail(HrText(I.name + " reported an error", hr), Err::Recording_InvalidState);
            if (r == 0) {
                if (std::chrono::steady_clock::now() > deadline) break;   // what it finished is kept
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return Ok();
    }
    (void)I.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    I.PullAll(out, hr);
    return Ok();
}

// ---------------------------------------------------------------------------
// AacEncoder

struct AacEncoder::Impl {
    ComScope com;
    Session session;
    ComRef<IMFTransform> mft;
    DWORD outBytes = 0;
    bool providesSamples = false;
    uint32_t rate = 48000;
    std::vector<uint8_t> asc;
    int64_t baseHns = -1;
    int64_t framesOut = 0;
    ~Impl() {
        if (mft) (void)mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    void Pull(std::vector<EncodedAudio>& out) {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER buffer{};
            ComRef<IMFSample> own;
            if (!providesSamples) {
                ComRef<IMFMediaBuffer> media;
                if (FAILED(MFCreateSample(own.Out())) ||
                    FAILED(MFCreateMemoryBuffer(std::max<DWORD>(outBytes, 8192), media.Out())) ||
                    FAILED(own->AddBuffer(media.get())))
                    return;
                buffer.pSample = own.get();
            }
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &buffer, &status);
            if (buffer.pEvents) buffer.pEvents->Release();
            IMFSample* produced = buffer.pSample;
            struct Releaser {
                IMFSample* s;
                ~Releaser() { if (s) s->Release(); }
            } release{providesSamples ? produced : nullptr};
            if (FAILED(hr) || !produced) return;

            ComRef<IMFMediaBuffer> contiguous;
            if (FAILED(produced->ConvertToContiguousBuffer(contiguous.Out()))) return;
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (FAILED(contiguous->Lock(&bytes, nullptr, &length))) return;
            EncodedAudio frame;
            frame.data.assign(bytes, bytes + length);
            contiguous->Unlock();
            // Each output is one 1024-sample AAC frame; time them from the first input so
            // the track is exact however the encoder buffers.
            frame.durationHns = static_cast<int64_t>(1024) * 10000000LL / rate;
            frame.timeHns = std::max<int64_t>(0, baseHns) + framesOut * frame.durationHns;
            ++framesOut;
            if (!frame.data.empty()) out.push_back(std::move(frame));
        }
    }
};

AacEncoder::AacEncoder() : impl_(std::make_unique<Impl>()) {}
AacEncoder::~AacEncoder() = default;
const std::vector<uint8_t>& AacEncoder::AudioSpecificConfig() const { return impl_->asc; }

Result<void> AacEncoder::Create(uint32_t rateHz, uint32_t bytesPerSecond, std::unique_ptr<AacEncoder>& out) {
    int freqIndex = -1;
    switch (rateHz) {
        case 48000: freqIndex = 3; break;
        case 44100: freqIndex = 4; break;
        default: return Fail("AAC records at 44.1 or 48 kHz", Err::Recording_ValidationFailed);
    }
    std::unique_ptr<AacEncoder> enc(new AacEncoder());
    Impl& I = *enc->impl_;
    if (!I.session.Acquire()) return Fail("Media Foundation did not start");
    I.rate = rateHz;

    MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, &outputInfo, &acts, &count);
    if (FAILED(hr) || count == 0) {
        ReleaseActivates(acts, count);
        return Fail("no AAC encoder is available through Media Foundation");
    }
    hr = acts[0]->ActivateObject(IID_PPV_ARGS(I.mft.Out()));
    ReleaseActivates(acts, count);
    if (FAILED(hr)) return Fail(HrText("starting the AAC encoder", hr));

    ComRef<IMFMediaType> outType;
    if (FAILED(hr = MFCreateMediaType(outType.Out()))) return Fail(HrText("creating the AAC type", hr));
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rateHz);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
    outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);                       // raw frames, no ADTS header
    outType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC-LC, level 2
    if (FAILED(hr = I.mft->SetOutputType(0, outType.get(), 0))) return Fail(HrText("the AAC encoder refused its format", hr));

    ComRef<IMFMediaType> inType;
    if (FAILED(hr = MFCreateMediaType(inType.Out()))) return Fail(HrText("creating the PCM type", hr));
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rateHz);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rateHz * 4);
    if (FAILED(hr = I.mft->SetInputType(0, inType.get(), 0))) return Fail(HrText("the AAC encoder refused PCM input", hr));

    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(I.mft->GetOutputStreamInfo(0, &info))) {
        I.outBytes = info.cbSize;
        I.providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    }
    (void)I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    (void)I.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // AudioSpecificConfig: object type 2 (AAC-LC), the rate's index, 2 channels.
    const uint16_t config = static_cast<uint16_t>((2 << 11) | (freqIndex << 7) | (2 << 3));
    I.asc = {static_cast<uint8_t>(config >> 8), static_cast<uint8_t>(config & 0xFF)};

    out = std::move(enc);
    return Ok();
}

Result<void> AacEncoder::Encode(const int16_t* pcm, size_t frames, int64_t timeHns, std::vector<EncodedAudio>& out) {
    Impl& I = *impl_;
    if (!I.mft) return Fail("the AAC encoder is closed", Err::Recording_InvalidState);
    if (frames == 0) return Ok();
    if (I.baseHns < 0) I.baseHns = timeHns;

    const DWORD bytes = static_cast<DWORD>(frames * 4);
    ComRef<IMFMediaBuffer> media;
    HRESULT hr = MFCreateMemoryBuffer(bytes, media.Out());
    if (FAILED(hr)) return Fail(HrText("allocating audio", hr), Err::Recording_InvalidState);
    BYTE* dst = nullptr;
    if (FAILED(hr = media->Lock(&dst, nullptr, nullptr))) return Fail(HrText("locking audio", hr), Err::Recording_InvalidState);
    std::memcpy(dst, pcm, bytes);
    media->Unlock();
    media->SetCurrentLength(bytes);
    ComRef<IMFSample> sample;
    if (FAILED(hr = MFCreateSample(sample.Out()))) return Fail(HrText("creating an audio sample", hr), Err::Recording_InvalidState);
    sample->AddBuffer(media.get());
    sample->SetSampleTime(timeHns);
    sample->SetSampleDuration(static_cast<LONGLONG>(frames) * 10000000LL / I.rate);
    hr = I.mft->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        I.Pull(out);
        hr = I.mft->ProcessInput(0, sample.get(), 0);
    }
    if (FAILED(hr)) return Fail(HrText("giving the AAC encoder audio", hr), Err::Recording_InvalidState);
    I.Pull(out);
    return Ok();
}

Result<void> AacEncoder::Drain(std::vector<EncodedAudio>& out) {
    Impl& I = *impl_;
    if (!I.mft) return Ok();
    (void)I.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    I.Pull(out);
    return Ok();
}

// ---------------------------------------------------------------------------
// InspectMediaFile

MediaFileInfo InspectMediaFile(const std::string& path) {
    MediaFileInfo info;
    ComScope com;
    Session session;
    if (!session.Acquire()) {
        info.error = "Media Foundation did not start";
        return info;
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(0, needed)), L'\0');
    if (needed > 0) MultiByteToWideChar(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), wpath.data(), needed);

    ComRef<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, reader.Out());
    if (FAILED(hr)) {
        info.error = HrText("opening the file", hr);
        return info;
    }
    info.opened = true;

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8)
        info.seconds = static_cast<double>(duration.uhVal.QuadPart) / 10000000.0;
    PropVariantClear(&duration);

    ComRef<IMFMediaType> type;
    if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, type.Out()))) {
        GUID subtype{};
        if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)))
            info.videoSubtype = IsEqualGUID(subtype, MFVideoFormat_H264) ? "H264" : "other";
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &w, &h))) {
            info.width = static_cast<int>(w);
            info.height = static_cast<int>(h);
        }
    }

    // Read every stream to the end, still compressed (no decoder involved). Which stream
    // is which is asked of the file (a container may list audio first), never assumed.
    (void)reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, TRUE);
    std::vector<int> streamKind;   // by stream index: 1 video, 2 audio, 0 other
    for (DWORD index = 0;; ++index) {
        ComRef<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(index, 0, native.Out()))) break;
        GUID major{};
        (void)native->GetGUID(MF_MT_MAJOR_TYPE, &major);
        streamKind.push_back(IsEqualGUID(major, MFMediaType_Video) ? 1 : IsEqualGUID(major, MFMediaType_Audio) ? 2 : 0);
    }
    size_t ended = 0;
    for (int guard = 0; guard < 200000 && ended < streamKind.size(); ++guard) {
        DWORD stream = 0, flags = 0;
        LONGLONG stamp = 0;
        ComRef<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &stream, &flags, &stamp, sample.Out());
        if (FAILED(hr)) {
            info.error = HrText("reading the file", hr);
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            ++ended;
            continue;
        }
        if (!sample || stream >= streamKind.size()) continue;
        if (streamKind[stream] == 1) ++info.videoSamples;
        else if (streamKind[stream] == 2) ++info.audioSamples;
    }

    // The sound itself: decode the audio track to 16-bit PCM and keep its loudest sample, so
    // "there is an audio track" cannot pass for a file that holds silence.
    info.audioPeak = -1.0;
    if (info.audioSamples > 0) {
        ComRef<IMFSourceReader> second;
        if (SUCCEEDED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, second.Out()))) {
            (void)second->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
            (void)second->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
            ComRef<IMFMediaType> pcmType;
            if (SUCCEEDED(MFCreateMediaType(pcmType.Out()))) {
                pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                if (SUCCEEDED(second->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pcmType.get()))) {
                    int peak = 0;
                    for (int guard = 0; guard < 100000; ++guard) {
                        DWORD flags = 0;
                        ComRef<IMFSample> sample;
                        if (FAILED(second->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, sample.Out())))
                            break;
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                        if (!sample) continue;
                        ComRef<IMFMediaBuffer> buffer;
                        if (FAILED(sample->ConvertToContiguousBuffer(buffer.Out()))) continue;
                        BYTE* bytes = nullptr;
                        DWORD length = 0;
                        if (FAILED(buffer->Lock(&bytes, nullptr, &length))) continue;
                        const auto* samples16 = reinterpret_cast<const int16_t*>(bytes);
                        for (DWORD i = 0; i < length / 2; ++i) peak = std::max(peak, std::abs(static_cast<int>(samples16[i])));
                        buffer->Unlock();
                    }
                    info.audioPeak = peak / 32768.0;
                }
            }
        }
    }
    return info;
}
} // namespace bps::recording