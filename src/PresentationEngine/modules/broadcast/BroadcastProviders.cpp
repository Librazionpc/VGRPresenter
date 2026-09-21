#include "modules/broadcast/BroadcastEngine.hpp"

#include "core/logging/Logger.hpp"
#include "platform/PlatformAccessor.hpp"
#include "platform/ILibrary.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>

namespace bps::broadcast {

namespace {

constexpr const char* kNdiModule = "NdiProvider";
constexpr const char* kSdiModule = "SdiProvider";

// ===========================================================================
// NDI provider — runtime binding to the NDI runtime's C API (NDI 5/6 ABI).
// The runtime is resolved at run time through the PAL library loader (it is a
// separate, vendor-licensed install — never bundled). When it is absent every
// call degrades to an error, and Probe() reports Err::Broadcast_SdkNotInstalled
// so the UI can send the user to the download page instead of guessing.
// ===========================================================================

// NDI opaque instances.
using NdiSendInstance = void*;
using NdiRecvInstance = void*;
using NdiFindInstance = void*;

// ABI mirror of Processing.NDI.structs.h (NDI 5/6, 64-bit), field for field.
// The runtime READS AND WRITES these by layout: a wrong field order or a
// short struct is silent memory corruption (recv_capture writes every field),
// which is why the sizes are pinned by static_assert below.
struct NdiSourceT {
    const char* p_ndi_name;      // "HOST (Camera 1)"
    const char* p_url_address;   // union with p_ip_address
};

struct NdiVideoFrameV2 {
    int xres;
    int yres;
    uint32_t fourCC;             // NDIlib_FourCC_video_type_e
    int frame_rate_N;
    int frame_rate_D;
    float picture_aspect_ratio;
    int frame_format_type;
    int64_t timecode;
    uint8_t* p_data;
    int line_stride_in_bytes;    // union with data_size_in_bytes
    const char* p_metadata;
    int64_t timestamp;
};

struct NdiAudioFrameV2 {
    int sample_rate;
    int no_channels;
    int no_samples;
    int64_t timecode;
    float* p_data;               // planar float32
    int channel_stride_in_bytes;
    const char* p_metadata;
    int64_t timestamp;
};

struct NdiMetadataFrame {
    int length;
    int64_t timecode;
    char* p_data;
};

struct NdiSendCreateT {
    const char* p_ndi_name;
    const char* p_groups;
    bool clock_video;
    bool clock_audio;
};

struct NdiRecvCreateV3 {
    NdiSourceT source_to_connect_to;
    int color_format;            // NDIlib_recv_color_format_e
    int bandwidth;               // NDIlib_recv_bandwidth_e
    bool allow_video_fields;
    const char* p_ndi_recv_name;
};

struct NdiFindCreateT {
    bool show_local_sources;
    const char* p_groups;
    const char* p_extra_ips;
};

static_assert(sizeof(void*) != 8 || sizeof(NdiSourceT) == 16, "NDI source ABI");
static_assert(sizeof(void*) != 8 || sizeof(NdiVideoFrameV2) == 72, "NDI video frame ABI");
static_assert(sizeof(void*) != 8 || sizeof(NdiAudioFrameV2) == 56, "NDI audio frame ABI");
static_assert(sizeof(void*) != 8 || sizeof(NdiMetadataFrame) == 24, "NDI metadata frame ABI");
static_assert(sizeof(void*) != 8 || sizeof(NdiRecvCreateV3) == 40, "NDI recv-create ABI");
static_assert(sizeof(void*) != 8 || sizeof(NdiFindCreateT) == 24, "NDI find-create ABI");
static_assert(sizeof(void*) != 8 || sizeof(NdiSendCreateT) == 24, "NDI send-create ABI");

constexpr int kNdiFrameTypeVideo = 1;                  // NDIlib_frame_type_video
constexpr int kNdiRecvColorUyvyBgra = 1;               // NDIlib_recv_color_format_UYVY_BGRA
constexpr int kNdiRecvBandwidthHighest = 100;          // NDIlib_recv_bandwidth_highest
constexpr int kNdiFrameProgressive = 1;                // NDIlib_frame_format_type_progressive
constexpr int64_t kNdiTimecodeSynthesize = INT64_MAX;  // let the SDK stamp frames

// FourCCs the send path understands, with their bytes per pixel.
constexpr uint32_t kFourCcUyvy = 0x59565955;   // 'UYVY'
constexpr uint32_t kFourCcBgra = 0x41524742;   // 'BGRA'
constexpr uint32_t kFourCcBgrx = 0x58524742;   // 'BGRX'
constexpr uint32_t kFourCcRgba = 0x41424752;   // 'RGBA'
constexpr uint32_t kFourCcRgbx = 0x58424752;   // 'RGBX'

int BytesPerPixel(uint32_t fourCC) {
    switch (fourCC) {
        case kFourCcUyvy: return 2;
        case kFourCcBgra:
        case kFourCcBgrx:
        case kFourCcRgba:
        case kFourCcRgbx: return 4;
        default: return 0;
    }
}

// fps -> the rational NDI wants. NTSC-family rates (23.976/29.97/59.94/...)
// must be N*1000/1001 or downstream tools drift against their clocks.
void FrameRateRational(double fps, int& n, int& d) {
    if (fps <= 0.0) fps = 30.0;
    const double nominal = std::round(fps);
    if (nominal > 0.0 && std::fabs(fps - nominal * 1000.0 / 1001.0) < 0.01) {
        n = static_cast<int>(nominal) * 1000;
        d = 1001;
        return;
    }
    n = static_cast<int>(std::lround(fps * 1000.0));
    d = 1000;
}

// Function table resolved from the NDI runtime. Return types matter: the
// runtime's `bool` returns only define the low byte, so they must be declared
// bool here, not int.
struct NdiApi {
    bool (*initialize)(void);
    void (*destroy)(void);
    const char* (*version)(void);
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
    void (*recv_free_video_v2)(NdiRecvInstance, const NdiVideoFrameV2*);
    void (*recv_destroy)(NdiRecvInstance);
};

// Where the NDI runtime's shared library might live, best candidate first.
// The vendor installers do NOT put it on PATH: NDI 5/6 record its folder in
// NDILIB_REDIST_FOLDER and (for the Tools bundle) drop it under
// "Program Files\NDI\NDI <n> Tools\Runtime" — a bare LoadLibrary by name finds
// neither, which made NDI look "not installed" on machines that had it.
struct NdiLibraryCandidates {
    std::vector<std::string> existing;   // absolute paths that exist on disk
    std::vector<std::string> byName;     // bare names for the OS loader's own search
};

NdiLibraryCandidates FindNdiLibraryCandidates() {
    NdiLibraryCandidates c;
#if defined(_WIN32)
    namespace fs = std::filesystem;
    constexpr const char* kDll = "Processing.NDI.Lib.x64.dll";
    auto addDir = [&](const fs::path& dir) {
        std::error_code ec;
        const fs::path p = dir / kDll;
        if (fs::is_regular_file(p, ec)) c.existing.push_back(p.string());
    };
    if (const char* redist = std::getenv("NDILIB_REDIST_FOLDER"); redist && *redist)
        addDir(redist);
    const char* pf = std::getenv("ProgramFiles");
    const fs::path ndiRoot = fs::path(pf && *pf ? pf : "C:\\Program Files") / "NDI";
    std::error_code ec;
    std::vector<fs::path> installs;
    for (const auto& e : fs::directory_iterator(ndiRoot, ec))
        if (e.is_directory(ec)) installs.push_back(e.path());
    // "NDI 6 ..." before "NDI 5 ..." — newest runtime wins.
    std::sort(installs.begin(), installs.end(),
              [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
    for (const auto& dir : installs) {
        addDir(dir);
        addDir(dir / "Runtime");
        std::vector<fs::path> versioned;   // "NDI 6 Runtime\v6"
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory(ec)) versioned.push_back(e.path());
        std::sort(versioned.begin(), versioned.end(),
                  [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
        for (const auto& v : versioned) addDir(v);
    }
    c.byName.push_back(kDll);
#else
    c.byName = {"libndi.so.6", "libndi.so.5", "libndi.so", "libndi.dylib"};
#endif
    return c;
}

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

    std::string RuntimeVersion() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return version_;
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
            NdiFindCreateT cfg{};
            // Sources on THIS machine are the common case for a single-PC
            // production (OBS, NDI Tools' Test Patterns/Screen Capture,
            // another VGR window) — hiding them made those invisible. Our
            // own senders are filtered out below instead.
            cfg.show_local_sources = true;
            find_ = api_.find_create_v2(&cfg);
            if (!find_) return out;
        }
        uint32_t count = 0;
        const NdiSourceT* srcs = api_.find_get_current_sources(find_, &count);
        for (uint32_t i = 0; i < count && srcs; ++i) {
            NdiSourceInfo info;
            if (srcs[i].p_ndi_name) info.name = srcs[i].p_ndi_name;
            if (srcs[i].p_url_address) info.urlAddress = srcs[i].p_url_address;
            if (info.name.empty() || IsOwnSender(info.name)) continue;
            out.push_back(std::move(info));
        }
        return out;
    }

    Result<BroadcastSenderId> CreateSender(std::string_view name,
                                           const NdiSenderConfig& cfg) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto probe = EnsureApi();
        if (!probe.ok()) return probe.error();
        NdiSendCreateT sc{};
        const std::string nameStr(name);
        const std::string groupsStr(cfg.groups);
        sc.p_ndi_name = nameStr.c_str();
        sc.p_groups = groupsStr.empty() ? nullptr : groupsStr.c_str();
        sc.clock_video = cfg.clockVideo;
        sc.clock_audio = cfg.clockAudio;
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
        const int bpp = BytesPerPixel(info.fourCC);
        if (bpp == 0)
            return Error::Make(Err::Broadcast_InvalidFrame, kNdiModule,
                               "unsupported pixel format (UYVY/BGRA/BGRX/RGBA/RGBX only)");
        const size_t stride = static_cast<size_t>(info.width) * static_cast<size_t>(bpp);
        // The runtime reads stride*height bytes from `data` — a short buffer is
        // an out-of-bounds read, so refuse it here.
        if (bytes < stride * info.height)
            return Error::Make(Err::Broadcast_InvalidFrame, kNdiModule,
                               "frame buffer smaller than width*height*bytes-per-pixel");
        NdiVideoFrameV2 f{};
        f.xres = static_cast<int>(info.width);
        f.yres = static_cast<int>(info.height);
        f.fourCC = info.fourCC;
        FrameRateRational(info.fps, f.frame_rate_N, f.frame_rate_D);
        f.picture_aspect_ratio = static_cast<float>(info.width) / static_cast<float>(info.height);
        f.frame_format_type = kNdiFrameProgressive;
        f.timecode = kNdiTimecodeSynthesize;
        f.p_data = static_cast<uint8_t*>(const_cast<void*>(data));
        f.line_stride_in_bytes = static_cast<int>(stride);
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
        if (info.channels <= 0 || info.samples <= 0)
            return Error::Make(Err::Broadcast_InvalidFrame, kNdiModule, "bad audio geometry");
        const size_t needed = static_cast<size_t>(info.channels) * static_cast<size_t>(info.samples)
                              * sizeof(float);
        if (bytes < needed)
            return Error::Make(Err::Broadcast_InvalidFrame, kNdiModule,
                               "audio buffer smaller than channels*samples*4");
        NdiAudioFrameV2 a{};
        a.sample_rate = info.sampleRate;
        a.no_channels = info.channels;
        a.no_samples = info.samples;
        a.timecode = kNdiTimecodeSynthesize;
        a.p_data = static_cast<float*>(const_cast<void*>(data));   // planar float32
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
        NdiRecvCreateV3 rc{};
        const std::string src(sourceName);
        rc.source_to_connect_to.p_ndi_name = src.c_str();
        rc.color_format = kNdiRecvColorUyvyBgra;
        rc.bandwidth = kNdiRecvBandwidthHighest;
        rc.allow_video_fields = false;
        rc.p_ndi_recv_name = "VGR Presenter";
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
        NdiVideoFrameV2 v{};
        // Audio/metadata slots are null: this call asks for VIDEO only (the
        // runtime then discards audio/metadata instead of queueing frames
        // nobody will ever free).
        const int type = api_.recv_capture_v2(it->second, &v, nullptr, nullptr, 0);   // non-blocking
        if (type != kNdiFrameTypeVideo || !v.p_data) return false;
        info.width = static_cast<uint32_t>(v.xres);
        info.height = static_cast<uint32_t>(v.yres);
        info.fourCC = v.fourCC;
        if (v.frame_rate_D > 0)
            info.fps = static_cast<double>(v.frame_rate_N) / static_cast<double>(v.frame_rate_D);
        const size_t stride = v.line_stride_in_bytes > 0
                                  ? static_cast<size_t>(v.line_stride_in_bytes)
                                  : static_cast<size_t>(v.xres) * 2;
        const size_t bytes = stride * static_cast<size_t>(v.yres);
        out.assign(v.p_data, v.p_data + bytes);
        // Every captured frame MUST be handed back or the runtime's receive
        // queue fills and the stream stalls.
        if (api_.recv_free_video_v2) api_.recv_free_video_v2(it->second, &v);
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
        sendNames_.clear();
        for (auto& [id, inst] : recvs_) if (api_.recv_destroy) api_.recv_destroy(inst);
        recvs_.clear();
        if (api_.destroy) api_.destroy();
        api_ = NdiApi{};
        version_.clear();
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

    // NDI names a source "<HOST> (<sender name>)"; ours are the senders this
    // provider created, and must not show up as inputs to ourselves.
    bool IsOwnSender(const std::string& sourceName) const {
        for (const auto& [id, name] : sendNames_) {
            const std::string suffix = "(" + name + ")";
            if (sourceName.size() >= suffix.size() &&
                sourceName.compare(sourceName.size() - suffix.size(), suffix.size(), suffix) == 0)
                return true;
        }
        return false;
    }

    Result<void> LoadApi() {
        if (libHandle_) return Ok();
        auto& platform = platform::PlatformAccessor::Get();
        auto& loader = platform.Library();

        const NdiLibraryCandidates candidates = FindNdiLibraryCandidates();
        platform::ILibrary::Handle handle = nullptr;
        std::string lastError;
        auto tryLoad = [&](const std::string& what) {
            auto h = loader.Load(what);
            if (h.ok()) { handle = h.value(); return true; }
            lastError = h.error().message;
            return false;
        };
        for (const auto& path : candidates.existing)
            if (tryLoad(path)) break;
        if (!handle)
            for (const auto& name : candidates.byName)
                if (tryLoad(name)) break;
        if (!handle) {
            // A library file that exists but would not load is a different
            // problem (wrong bitness, corrupt install) from "not installed".
            if (!candidates.existing.empty())
                return Error::Make(Err::Broadcast_SdkLoadFailed, kNdiModule,
                                   "NDI runtime found at '" + candidates.existing.front() +
                                   "' but could not be loaded: " + lastError);
            return Error::Make(Err::Broadcast_SdkNotInstalled, kNdiModule,
                               "NDI runtime not installed: " + lastError);
        }
        libHandle_ = handle;

        NdiApi api{};
        auto sym = [&](void** out, const char* name) {
            auto s = loader.Symbol(libHandle_, name);
            if (s.ok()) *out = s.value();
        };
        sym((void**)&api.initialize, "NDIlib_initialize");
        sym((void**)&api.destroy, "NDIlib_destroy");
        sym((void**)&api.version, "NDIlib_version");
        sym((void**)&api.find_create_v2, "NDIlib_find_create_v2");
        sym((void**)&api.find_get_current_sources, "NDIlib_find_get_current_sources");
        sym((void**)&api.find_destroy, "NDIlib_find_destroy");
        sym((void**)&api.send_create, "NDIlib_send_create");
        sym((void**)&api.send_send_video_v2, "NDIlib_send_send_video_v2");
        sym((void**)&api.send_send_audio_v2, "NDIlib_send_send_audio_v2");
        sym((void**)&api.send_destroy, "NDIlib_send_destroy");
        sym((void**)&api.recv_create_v3, "NDIlib_recv_create_v3");
        sym((void**)&api.recv_capture_v2, "NDIlib_recv_capture_v2");
        sym((void**)&api.recv_free_video_v2, "NDIlib_recv_free_video_v2");
        sym((void**)&api.recv_destroy, "NDIlib_recv_destroy");
        if (!api.initialize) {
            (void)loader.Unload(libHandle_);
            libHandle_ = nullptr;
            return Error::Make(Err::Broadcast_SdkLoadFailed, kNdiModule,
                               "NDI SDK present but core symbol NDIlib_initialize missing");
        }
        // NDIlib_initialize returns false when the runtime cannot start (e.g.
        // an unsupported CPU); do not report Available for a broken runtime.
        if (!api.initialize()) {
            (void)loader.Unload(libHandle_);
            libHandle_ = nullptr;
            return Error::Make(Err::Broadcast_SdkLoadFailed, kNdiModule,
                               "NDI runtime failed to initialize");
        }
        api_ = api;
        if (api_.version)
            if (const char* v = api_.version()) version_ = v;
        return Ok();
    }

    mutable std::mutex mutex_;
    NdiApi api_{};
    platform::ILibrary::Handle libHandle_ = nullptr;
    NdiFindInstance find_ = nullptr;   // persistent discovery cache (see DiscoverSources)
    std::string version_;              // runtime's own version string, once loaded
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
