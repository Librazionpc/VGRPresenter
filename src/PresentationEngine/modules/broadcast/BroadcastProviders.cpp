#include "modules/broadcast/BroadcastEngine.hpp"

#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"
#include "platform/ILibrary.hpp"

#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <mutex>

namespace bps::broadcast {

namespace {

constexpr const char* kNdiModule = "NdiProvider";
constexpr const char* kSdiModule = "SdiProvider";

// ===========================================================================
// NDI provider — runtime binding to the NewTek/Vizrt NDI SDK (v5 C API).
// The SDK is resolved at runtime through the PAL library loader; when libndi
// is not installed every call degrades to Err::Broadcast_Unsupported.
// ===========================================================================

// NDI v5 opaque instances.
using NdiSendInstance = void*;
using NdiRecvInstance = void*;
using NdiFindInstance = void*;

// Minimal NDI v5 ABI shapes used by the provider (fields we actually touch;
// larger SDK structs are passed by pointer and left opaque).
struct NdiSourceT {
    const char* p_ndi_name;      // "CAM 1 (192.168.1.10)"
    const char* p_url_address;   // "ndi://192.168.1.10/CAM%201"
};

struct NdiVideoFrameV2 {
    int xres;
    int yres;
    int fourCC;                  // NDIlib_FourCC_video_type_e
    int frame_rate_N;
    int frame_rate_D;
    float picture_aspect_ratio;
    int frame_format_type;
    int64_t timecode;
    void* p_metadata;
    void* p_data;
    int line_stride_in_bytes;
};

struct NdiSendCreateT {
    const char* p_ndi_name;
    const char* p_groups;
    int clock_video;
    int clock_audio;
};

struct NdiRecvCreateV3 {
    NdiSourceT source_to_connect_to;
    int color_format;
    int bandwidth;
    int allow_video_fields;
    const char* p_ndi_recv_name;
    int p_allow_video_1080p;
    int p_allow_video_2160p;
    int p_allow_video_4k;
    int p_allow_video_8k;
};

struct NdiFindCreateT {
    int show_local_sources;
    const char* p_groups;
    const char** p_extra_ips;
};

struct NdiAudioFrameV2 {
    int sample_rate;
    int no_channels;
    int no_samples;
    int64_t timecode;
    int audio_format;
    void* p_data;
    int channel_stride_in_bytes;
};

struct NdiMetadataFrame {
    int64_t length;
    void* p_data;
    int64_t timecode;
};

// Function table resolved from libndi.so / Processing.NDI.Lib.x64.dll.
struct NdiApi {
    int (*initialize)(void);
    void (*destroy)(void);
    NdiFindInstance (*find_create_v2)(const NdiFindCreateT*);
    const NdiSourceT* (*find_get_current_sources)(NdiFindInstance, uint32_t* count);
    void (*find_destroy)(NdiFindInstance);
    NdiSendInstance (*send_create)(const NdiSendCreateT*);
    void (*send_send_video_v2)(NdiSendInstance, const NdiVideoFrameV2*);
    void (*send_send_audio_v2)(NdiSendInstance, const NdiAudioFrameV2*);
    void (*send_destroy)(NdiSendInstance);
    NdiRecvInstance (*recv_create_v3)(const NdiRecvCreateV3*);
    int (*recv_capture_v2)(NdiRecvInstance, NdiVideoFrameV2*, NdiAudioFrameV2*,
                           NdiMetadataFrame*, uint32_t timeout_ms);
    void (*recv_free_v2)(NdiRecvInstance, const NdiVideoFrameV2*, const NdiAudioFrameV2*,
                         const NdiMetadataFrame*);
    void (*recv_destroy)(NdiRecvInstance);
};

class NdiProvider final : public IBroadcastProvider {
public:
    const char* Name() const noexcept override { return "ndi"; }
    ProviderKind Kind() const noexcept override { return ProviderKind::Ndi; }

    Result<ProviderState> Probe() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (api_.initialize) return ProviderState::Available;
        auto loaded = LoadApi();
        if (!loaded.ok()) return loaded.error();
        return ProviderState::Available;
    }

    std::vector<NdiSourceInfo> DiscoverSources() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<NdiSourceInfo> out;
        if (!api_.initialize || !api_.find_create_v2) return out;
        // The finder is PERSISTENT: NDI's discovery browses the network in a
        // background thread and find_get_current_sources only reports the
        // cache it has accumulated so far — a create/query/destroy finder
        // per call would read an always-fresh (always empty) cache and never
        // see a single source on a real network. Created once here; the SDK
        // keeps refreshing it; destroyed in ShutdownProvider.
        if (!find_) {
            NdiFindCreateT cfg;
            std::memset(&cfg, 0, sizeof(cfg));
            cfg.show_local_sources = 0;   // remote sources only (local is the send instance)
            find_ = api_.find_create_v2(&cfg);
            if (!find_) return out;
        }
        uint32_t count = 0;
        const NdiSourceT* srcs = api_.find_get_current_sources(find_, &count);
        for (uint32_t i = 0; i < count && srcs; ++i) {
            NdiSourceInfo info;
            if (srcs[i].p_ndi_name) info.name = srcs[i].p_ndi_name;
            if (srcs[i].p_url_address) info.urlAddress = srcs[i].p_url_address;
            if (!info.name.empty()) out.push_back(std::move(info));
        }
        return out;
    }

    Result<BroadcastSenderId> CreateSender(std::string_view name,
                                           const NdiSenderConfig& cfg) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto probe = EnsureApi();
        if (!probe.ok()) return probe.error();
        NdiSendCreateT sc;
        std::memset(&sc, 0, sizeof(sc));
        const std::string nameStr(name);
        const std::string groupsStr(cfg.groups);
        sc.p_ndi_name = nameStr.c_str();
        sc.p_groups = groupsStr.empty() ? nullptr : groupsStr.c_str();
        sc.clock_video = cfg.clockVideo ? 1 : 0;
        sc.clock_audio = cfg.clockAudio ? 1 : 0;
        NdiSendInstance inst = api_.send_create(&sc);
        if (!inst) {
            return Error::Make(Err::Broadcast_SendFailed, kNdiModule,
                               "NDIlib_send_create failed for '" + nameStr + "'");
        }
        const std::string id = std::format("ndi-send-{}", ++seq_);
        sends_[id] = inst;
        sendNames_[id] = nameStr;
        return id;
    }

    Result<void> SendVideo(std::string_view senderId, const VideoFrameInfo& info,
                           const void* data, size_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sends_.find(std::string(senderId));
        if (it == sends_.end())
            return Error::Make(Err::Broadcast_NotFound, kNdiModule, "unknown sender");
        if (!data || bytes == 0)
            return Error::Make(Err::Broadcast_InvalidFrame, kNdiModule, "empty frame");
        if (info.width == 0 || info.height == 0)
            return Error::Make(Err::Broadcast_InvalidFrame, kNdiModule, "bad geometry");
        NdiVideoFrameV2 f;
        std::memset(&f, 0, sizeof(f));
        f.xres = static_cast<int>(info.width);
        f.yres = static_cast<int>(info.height);
        f.fourCC = static_cast<int>(info.fourCC);
        f.frame_rate_N = 30000;
        f.frame_rate_D = 1001;   // ~29.97; providers may refine from info.fps
        f.picture_aspect_ratio = 16.0f / 9.0f;
        f.frame_format_type = 1;   // progressive
        f.p_data = const_cast<void*>(data);
        f.line_stride_in_bytes = static_cast<int>(info.width * 2);  // UYVY
        api_.send_send_video_v2(it->second, &f);
        return Ok();
    }

    Result<void> SendAudio(std::string_view senderId, const AudioFrameInfo& info,
                           const void* data, size_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sends_.find(std::string(senderId));
        if (it == sends_.end())
            return Error::Make(Err::Broadcast_NotFound, kNdiModule, "unknown sender");
        if (!data || bytes == 0) return Ok();   // silence is valid audio
        NdiAudioFrameV2 a;
        std::memset(&a, 0, sizeof(a));
        a.sample_rate = info.sampleRate;
        a.no_channels = info.channels;
        a.no_samples = info.samples;
        a.audio_format = 1;   // floating point
        a.p_data = const_cast<void*>(data);
        a.channel_stride_in_bytes = static_cast<int>(info.samples * sizeof(float));
        api_.send_send_audio_v2(it->second, &a);
        return Ok();
    }

    Result<void> StopSender(std::string_view senderId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sends_.find(std::string(senderId));
        if (it == sends_.end()) return Ok();
        if (api_.send_destroy) api_.send_destroy(it->second);
        sends_.erase(it);
        sendNames_.erase(std::string(senderId));
        return Ok();
    }

    Result<BroadcastReceiverId> CreateReceiver(std::string_view sourceName) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto probe = EnsureApi();
        if (!probe.ok()) return probe.error();
        NdiRecvCreateV3 rc;
        std::memset(&rc, 0, sizeof(rc));
        const std::string src(sourceName);
        rc.source_to_connect_to.p_ndi_name = src.c_str();
        rc.color_format = 1;          // UYVY_BGRA
        rc.bandwidth = 100;           // highest
        rc.allow_video_fields = 0;
        NdiRecvInstance inst = api_.recv_create_v3(&rc);
        if (!inst)
            return Error::Make(Err::Broadcast_ReceiveFailed, kNdiModule,
                               "NDIlib_recv_create_v3 failed for '" + src + "'");
        const std::string id = std::format("ndi-recv-{}", ++seq_);
        recvs_[id] = inst;
        return id;
    }

    Result<bool> ReceiveFrame(std::string_view receiverId, VideoFrameInfo& info,
                              std::vector<uint8_t>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = recvs_.find(std::string(receiverId));
        if (it == recvs_.end())
            return Error::Make(Err::Broadcast_NotFound, kNdiModule, "unknown receiver");
        if (!api_.recv_capture_v2) return false;
        NdiVideoFrameV2 v;
        NdiAudioFrameV2 a;
        NdiMetadataFrame m;
        std::memset(&v, 0, sizeof(v));
        std::memset(&a, 0, sizeof(a));
        std::memset(&m, 0, sizeof(m));
        int r = api_.recv_capture_v2(it->second, &v, &a, &m, 0);   // non-blocking
        if (r != 1 || !v.p_data) return false;   // NDIlib_frame_type_video == 1
        info.width = static_cast<uint32_t>(v.xres);
        info.height = static_cast<uint32_t>(v.yres);
        info.fourCC = static_cast<uint32_t>(v.fourCC);
        const size_t bytes = static_cast<size_t>(v.line_stride_in_bytes) * v.yres;
        const auto* raw = static_cast<const uint8_t*>(v.p_data);
        out.assign(raw, raw + bytes);
        if (api_.recv_free_v2) api_.recv_free_v2(it->second, &v, &a, &m);
        return true;
    }

    Result<void> DisconnectReceiver(std::string_view receiverId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = recvs_.find(std::string(receiverId));
        if (it == recvs_.end()) return Ok();
        if (api_.recv_destroy) api_.recv_destroy(it->second);
        recvs_.erase(it);
        return Ok();
    }

    Result<void> ShutdownProvider() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (find_ && api_.find_destroy) api_.find_destroy(find_);
        find_ = nullptr;
        for (auto& [id, inst] : sends_) if (api_.send_destroy) api_.send_destroy(inst);
        sends_.clear();
        for (auto& [id, inst] : recvs_) if (api_.recv_destroy) api_.recv_destroy(inst);
        recvs_.clear();
        if (api_.destroy) api_.destroy();
        api_ = NdiApi{};
        if (libHandle_) {
            (void)platform::PlatformAccessor::Get().Library().Unload(libHandle_);
            libHandle_ = nullptr;
        }
        return Ok();
    }

private:
    Result<void> EnsureApi() {
        if (api_.initialize) return Ok();
        return LoadApi();
    }

    Result<void> LoadApi() {
        if (libHandle_) return Ok();
        auto& platform = platform::PlatformAccessor::Get();
        auto& loader = platform.Library();
        // Windows ships the SDK as Processing.NDI.Lib.x64.dll; Linux as the
        // versioned/unversioned libndi.so. Try every name — the loader just
        // fails on whichever aren't present.
        auto h = loader.Load("Processing.NDI.Lib.x64.dll");
        if (!h.ok()) h = loader.Load("libndi.so.5");
        if (!h.ok()) h = loader.Load("libndi.so");
        if (!h.ok())
            return Error::Make(Err::Broadcast_SdkLoadFailed, kNdiModule,
                               "NDI SDK not installed: " + h.error().message);
        libHandle_ = h.value();
        NdiApi api{};
        auto sym = [&](void** out, const char* name) {
            auto s = loader.Symbol(libHandle_, name);
            if (s.ok()) *out = s.value();
        };
        sym((void**)&api.initialize, "NDIlib_initialize");
        sym((void**)&api.destroy, "NDIlib_destroy");
        sym((void**)&api.find_create_v2, "NDIlib_find_create_v2");
        sym((void**)&api.find_get_current_sources, "NDIlib_find_get_current_sources");
        sym((void**)&api.find_destroy, "NDIlib_find_destroy");
        sym((void**)&api.send_create, "NDIlib_send_create");
        sym((void**)&api.send_send_video_v2, "NDIlib_send_send_video_v2");
        sym((void**)&api.send_send_audio_v2, "NDIlib_send_send_audio_v2");
        sym((void**)&api.send_destroy, "NDIlib_send_destroy");
        sym((void**)&api.recv_create_v3, "NDIlib_recv_create_v3");
        sym((void**)&api.recv_capture_v2, "NDIlib_recv_capture_v2");
        sym((void**)&api.recv_free_v2, "NDIlib_recv_free_v2");
        sym((void**)&api.recv_destroy, "NDIlib_recv_destroy");
        if (!api.initialize) {
            (void)loader.Unload(libHandle_);
            libHandle_ = nullptr;
            return Error::Make(Err::Broadcast_SdkLoadFailed, kNdiModule,
                               "NDI SDK present but core symbol NDIlib_initialize missing");
        }
        // NDIlib_initialize returns 0 (false) when the SDK cannot start; do not
        // report Available for a broken SDK — unload and degrade gracefully.
        if (api.initialize() == 0) {
            (void)loader.Unload(libHandle_);
            libHandle_ = nullptr;
            return Error::Make(Err::Broadcast_SdkLoadFailed, kNdiModule,
                               "NDI SDK failed to initialize");
        }
        api_ = api;
        return Ok();
    }

    mutable std::mutex mutex_;
    NdiApi api_{};
    platform::ILibrary::Handle libHandle_ = nullptr;
    NdiFindInstance find_ = nullptr;   // persistent discovery cache (see DiscoverSources)
    uint64_t seq_ = 0;
    std::map<std::string, NdiSendInstance, std::less<>> sends_;
    std::map<std::string, std::string, std::less<>> sendNames_;
    std::map<std::string, NdiRecvInstance, std::less<>> recvs_;
};

// ===========================================================================
// SDI provider — runtime binding to the Blackmagic DeckLink SDK. The SDK
// (libDeckLinkAPI / DeckLinkAPI.dll) provides the IDeckLink COM interfaces;
// we resolve the iterator entry point and enumerate devices. Without the SDK
// the provider degrades to Unsupported, exactly like NDI.
// ===========================================================================

using IDeckLink = void*;

// IDeckLinkIterator (vtable: IUnknown + Next).
struct DeckLinkIteratorVtbl {
    void* queryInterface;
    void* addRef;
    void* release;
    void* next;
};

struct DeckLinkIterator {
    DeckLinkIteratorVtbl* vtbl;
};

// IDeckLink (vtable: IUnknown + GetModelName + GetDisplayName + ...).
struct DeckLinkVtbl {
    void* queryInterface;
    void* addRef;
    void* release;
    void* getModelName;     // (IDeckLink*, BSTR*) -> HRESULT
    void* getDisplayName;   // (IDeckLink*, BSTR*) -> HRESULT
};

struct DeckLink {
    DeckLinkVtbl* vtbl;
};

// HRESULT codes used by the iterator walk.
constexpr long kSdkOk = 0;          // S_OK
constexpr long kSdkFalse = 1;       // S_FALSE (iterator exhausted)

class SdiProvider final : public IBroadcastProvider {
public:
    const char* Name() const noexcept override { return "sdi"; }
    ProviderKind Kind() const noexcept override { return ProviderKind::Sdi; }

    Result<ProviderState> Probe() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (libHandle_) return ProviderState::Available;
        auto r = LoadApi();
        if (!r.ok()) return r.error();
        return ProviderState::Available;
    }

    std::vector<SdiDeviceInfo> EnumerateSdiDevices() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<SdiDeviceInfo> out;
        auto probe = EnsureApi();
        if (!probe.ok()) return out;
        if (!createIterator_) return out;
        void* iterRaw = nullptr;
        long hr = createIterator_(&iterRaw);
        if (hr != kSdkOk || !iterRaw) return out;
        auto* iter = static_cast<DeckLinkIterator*>(iterRaw);
        for (int idx = 0; ; ++idx) {
            void* deviceRaw = nullptr;
            using NextFn = long (*)(void*, void**);
            auto next = reinterpret_cast<NextFn>(iter->vtbl->next);
            if (!next) break;
            long r = next(iter, &deviceRaw);
            if (r != kSdkOk || !deviceRaw) break;
            SdiDeviceInfo info;
            info.index = idx;
            auto* dev = static_cast<DeckLink*>(deviceRaw);
            using GetNameFn = long (*)(void*, void**);
            if (dev->vtbl && dev->vtbl->getModelName) {
                auto getModel = reinterpret_cast<GetNameFn>(dev->vtbl->getModelName);
                void* bstr = nullptr;
                if (getModel(dev, &bstr) == kSdkOk && bstr)
                    info.modelName = WideToUtf8(static_cast<wchar_t*>(bstr));
            }
            out.push_back(std::move(info));
            // Release the device (IUnknown::Release).
            if (dev->vtbl && dev->vtbl->release) {
                using ReleaseFn = unsigned long (*)(void*);
                auto release = reinterpret_cast<ReleaseFn>(dev->vtbl->release);
                (void)release(dev);
            }
        }
        // Release the iterator.
        if (iter->vtbl && iter->vtbl->release) {
            using ReleaseFn = unsigned long (*)(void*);
            auto release = reinterpret_cast<ReleaseFn>(iter->vtbl->release);
            (void)release(iter);
        }
        return out;
    }

    Result<void> ShutdownProvider() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (libHandle_) (void)platform::PlatformAccessor::Get().Library().Unload(libHandle_);
        libHandle_ = nullptr;
        createIterator_ = nullptr;
        return Ok();
    }

private:
    Result<void> EnsureApi() {
        if (libHandle_) return Ok();
        return LoadApi();
    }

    Result<void> LoadApi() {
        auto& loader = platform::PlatformAccessor::Get().Library();
        auto h = loader.Load("libDeckLinkAPI.so");
        if (!h.ok()) h = loader.Load("libDeckLinkAPI.dylib");
        if (!h.ok())
            return Error::Make(Err::Broadcast_SdkLoadFailed, kSdiModule,
                               "DeckLink SDK not installed: " + h.error().message);
        libHandle_ = h.value();
        auto s = loader.Symbol(libHandle_, "CreateDeckLinkIteratorInstance");
        if (!s.ok()) {
            (void)loader.Unload(libHandle_);
            libHandle_ = nullptr;
            return Error::Make(Err::Broadcast_SdkLoadFailed, kSdiModule,
                               "DeckLink SDK present but iterator entry point missing");
        }
        createIterator_ = reinterpret_cast<CreateIteratorFn>(s.value());
        return Ok();
    }

    static std::string WideToUtf8(const wchar_t* wide) {
        std::string out;
        if (!wide) return out;
        for (const wchar_t* p = wide; *p; ++p) {
            unsigned long cp = static_cast<unsigned long>(*p);
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        return out;
    }

    using CreateIteratorFn = long (*)(void** out);
    mutable std::mutex mutex_;
    platform::ILibrary::Handle libHandle_ = nullptr;
    CreateIteratorFn createIterator_ = nullptr;
};

} // namespace

// Called by BroadcastEngine::Initialize to register the real providers.
Result<void> RegisterBuiltinBroadcastProviders(BroadcastEngine& engine) {
    auto r = engine.RegisterProvider(std::make_shared<NdiProvider>());
    if (!r.ok()) return r;
    return engine.RegisterProvider(std::make_shared<SdiProvider>());
}

} // namespace bps::broadcast
