#define INITGUID
#include "platform/windows/WindowsVideo.hpp"

#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <cfgmgr32.h>   // CM_Get_Device_Interface_List: the cheap camera roster
#include <cwchar>
#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <gdiplus.h>
#include <objidl.h>   // IStream for the JPEG encoder
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bps::platform {
namespace {
// Scoped COM init — same contract as WindowsAudio's (balanced ref only when
// WE initialized the thread).
class ScopedComInit {
public:
    ScopedComInit() { ok_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ScopedComInit() { if (ok_) CoUninitialize(); }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit &operator=(const ScopedComInit&) = delete;
private:
    bool ok_ = false;
};

// Rounded fps from an MF frame-rate ratio (out/in), 0 on nonsense.
uint32_t RatioToFps(UINT32 num, UINT32 den)
{
    if (den == 0) return 0;
    return static_cast<uint32_t>(std::lround(static_cast<double>(num) / den));
}

// ASCII-only lowercase — case-insensitive device-id comparisons.
std::string LowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool NamedBy(const std::set<std::string> *only, const std::string &name)
{
    if (!only) return true;
    if (only->count(name)) return true;
    const auto trim = [](const std::string &s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    };
    const std::string wanted = LowerAscii(trim(name));
    for (const std::string &w : *only)
        if (LowerAscii(trim(w)) == wanted) return true;
    return false;
}
} // namespace

std::vector<VideoDeviceInfo> WindowsVideo::Enumerate() const
{
    return EnumerateFiltered(nullptr);
}

std::vector<VideoDeviceInfo> WindowsVideo::EnumerateFor(
    const std::vector<std::string> &names) const {
    // Nothing named ⇒ nothing wanted: no MF enumeration at all.
    if (names.empty())
        return {};
    const std::set<std::string> only(names.begin(), names.end());
    return EnumerateFiltered(&only);
}

// The shared walk (see the header): `only` skips every camera the caller did
// not name BEFORE it is activated — the media-type walk is the expensive half.
std::vector<VideoDeviceInfo> WindowsVideo::EnumerateFiltered(
    const std::set<std::string> *only) const
{
    std::vector<VideoDeviceInfo> out;
    ScopedComInit com;

    IMFAttributes *attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || !attrs) return out;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate **activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs, &activates, &count))) {
        attrs->Release();
        return out;
    }

    for (UINT32 i = 0; i < count; ++i) {
        VideoDeviceInfo info;

        WCHAR *nameW = nullptr;
        UINT32 nameLen = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &nameW, &nameLen)) && nameW) {
            info.name = win::Utf8(std::wstring(nameW, nameLen));
            CoTaskMemFree(nameW);
        }
        WCHAR *symW = nullptr;
        UINT32 symLen = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symW, &symLen)) && symW) {
            info.id = win::Utf8(std::wstring(symW, symLen));
            CoTaskMemFree(symW);
        }
        if (!NamedBy(only, info.name))
            continue;
        info.isCamera = true;

        IMFMediaSource *source = nullptr;
        if (SUCCEEDED(activates[i]->ActivateObject(IID_IMFMediaSource,
                                                   reinterpret_cast<void **>(&source))) && source) {
            IMFPresentationDescriptor *pd = nullptr;
            if (SUCCEEDED(source->CreatePresentationDescriptor(&pd)) && pd) {
                DWORD sdCount = 0;
                if (SUCCEEDED(pd->GetStreamDescriptorCount(&sdCount))) {
                    std::map<std::pair<UINT32, UINT32>, std::vector<uint32_t>> bySize;
                    for (DWORD s = 0; s < sdCount; ++s) {
                        IMFStreamDescriptor *sd = nullptr;
                        BOOL selected = FALSE;
                        if (FAILED(pd->GetStreamDescriptorByIndex(s, &selected, &sd)) || !sd) continue;
                        IMFMediaTypeHandler *handler = nullptr;
                        if (SUCCEEDED(sd->GetMediaTypeHandler(&handler)) && handler) {
                            DWORD typeCount = 0;
                            handler->GetMediaTypeCount(&typeCount);
                            for (DWORD t = 0; t < typeCount; ++t) {
                                IMFMediaType *type = nullptr;
                                if (FAILED(handler->GetMediaTypeByIndex(t, &type)) || !type) continue;
                                GUID major{};
                                if (SUCCEEDED(type->GetMajorType(&major)) && major == MFMediaType_Video) {
                                    UINT64 frameSize = 0, frameRate = 0;
                                    if (SUCCEEDED(type->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))
                                        && SUCCEEDED(type->GetUINT64(MF_MT_FRAME_RATE, &frameRate))) {
                                        const UINT32 w = static_cast<UINT32>(frameSize >> 32);
                                        const UINT32 h = static_cast<UINT32>(frameSize & 0xFFFFFFFF);
                                        const UINT32 fps = RatioToFps(
                                            static_cast<UINT32>(frameRate >> 32),
                                            static_cast<UINT32>(frameRate & 0xFFFFFFFF));
                                        if (w > 0 && h > 0 && fps > 0)
                                            bySize[{w, h}].push_back(fps);
                                    }
                                }
                                type->Release();
                            }
                            handler->Release();
                        }
                        sd->Release();
                    }
                    for (auto &[size, rates] : bySize) {
                        std::sort(rates.begin(), rates.end());
                        rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
                        VideoModeInfo m;
                        m.width = size.first;
                        m.height = size.second;
                        m.fpsRates = std::move(rates);
                        for (uint32_t r : m.fpsRates) info.maxFps = std::max(info.maxFps, r);
                        info.modes.push_back(std::move(m));
                    }
                }
                pd->Release();
            }
            source->Release();
        }
        out.push_back(std::move(info));
    }

    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    attrs->Release();
    return out;
}

struct EnumWindowsCtx {
    std::vector<WindowInfo> out;
    DWORD selfPid = 0;
};

BOOL CALLBACK WindowEnumProc(HWND hwnd, LPARAM lParam)
{
    auto *ctx = reinterpret_cast<EnumWindowsCtx *>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    // TOOLWINDOW: palettes/tooltips sized 0×0 or worse, no taskbar presence —
    // capture noise either way.
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked == DWM_CLOAKED_APP)
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ctx->selfPid) return TRUE;
    WCHAR titleW[256] = {};
    int n = GetWindowTextW(hwnd, titleW, 256);
    if (n <= 0) return TRUE;
    WindowInfo info;
    info.id = "win:" + std::to_string(reinterpret_cast<uintptr_t>(hwnd));
    info.title = win::Utf8(std::wstring(titleW, n));
    ctx->out.push_back(std::move(info));
    return TRUE;
}

std::vector<WindowInfo> WindowsVideo::EnumerateWindows() const
{
    EnumWindowsCtx ctx;
    ctx.selfPid = GetCurrentProcessId();
    EnumWindows(WindowEnumProc, reinterpret_cast<LPARAM>(&ctx));
    std::sort(ctx.out.begin(), ctx.out.end(),
              [](const WindowInfo &a, const WindowInfo &b) { return a.title < b.title; });
    return ctx.out;
}

namespace {
constexpr GUID kCameraInterfaceClass = {
    0xE5323777, 0xF976, 0x4f5b, {0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44}};

std::optional<std::string> CheapCaptureFingerprint()
{
    std::vector<std::string> paths;
    {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID *>(&kCameraInterfaceClass),
                                               nullptr,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
            return std::nullopt;
        if (len > 0) {
            std::wstring list(len, L'\0');
            if (CM_Get_Device_Interface_ListW(const_cast<GUID *>(&kCameraInterfaceClass), nullptr,
                                              list.data(), len,
                                              CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
                return std::nullopt;
            for (const wchar_t *p = list.c_str(); *p; p += std::wcslen(p) + 1)
                paths.push_back(win::Utf8(p));
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    std::string fp;
    for (const auto &p : paths)
        fp += p + ";";
    return fp;
}
} // namespace

std::string WindowsVideo::Fingerprint() const
{
    if (auto cheap = CheapCaptureFingerprint(); cheap.has_value())
        return *cheap;
    // The config manager could not answer: the MF roster (and its cost) stays.
    std::string fp;
    for (const auto &d : Enumerate())
        fp += d.id + ";";
    return fp;
}

// ============================================================================
// ============================================================================
struct VideoTapShared {
    std::atomic<bool> running{false};   // the stop flag the table's Erase/Clear sets
    std::mutex mutex;                   // guards latestJpeg/latestRgba + the read handshake below
    std::vector<uint8_t> latestJpeg;    // swapped wholesale, copied out under `mutex`
    // ---- the LIVE half (see IVideo::SetPreviewLive) -----------------------
    std::atomic<bool> live{false};
    std::shared_ptr<const std::vector<uint8_t>> latestRgba;   // tightly packed RGBA8
    uint32_t rgbaW = 0, rgbaH = 0;
    std::atomic<uint64_t> frameSeq{0};
    int64_t lastStoreMs = 0;
    std::atomic<int64_t> frameIntervalMs{66}; // pane/JPEG cadence; live RGBA follows device rate
    // When a JPEG was last READ off this tap. While live the RGBA buffer is the picture (the engine's
    // outputs and the GPU surfaces read it), so a JPEG encode is only worth the capture thread's time
    // when a pane is actually pulling one.
    std::atomic<int64_t> lastJpegPullMs{0};
    static int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    bool JpegWanted() const
    {
        const int64_t at = lastJpegPullMs.load(std::memory_order_relaxed);
        return at != 0 && NowMs() - at < 1500;
    }

    void Publish(std::vector<uint8_t> jpeg)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        latestJpeg = std::move(jpeg);
    }
    std::vector<uint8_t> Frame()
    {
        lastJpegPullMs.store(NowMs(), std::memory_order_relaxed);
        const std::lock_guard<std::mutex> lock(mutex);
        return latestJpeg;
    }

    void PublishRgba(uint32_t w, uint32_t h, std::vector<uint8_t> rgba)
    {
        PublishRgbaFrame(std::make_shared<const std::vector<uint8_t>>(std::move(rgba)), w, h);
    }

    void PublishRgbaFrame(std::shared_ptr<const std::vector<uint8_t>> rgba, uint32_t w, uint32_t h)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        latestRgba = std::move(rgba);
        rgbaW = w;
        rgbaH = h;
        frameSeq.fetch_add(1, std::memory_order_release);
    }

    // ---- the ASYNC camera read: the MF callback <-> this tap's worker ----
    std::condition_variable cv;        // stop / flush / stream-death wakeups
    std::atomic<IMFSourceReader *> reader{nullptr};   // borrowed; owned by the worker
    bool flushDone = false;            // OnFlush arrived: nothing is pending any more
    bool stopping = false;             // the worker released the reader — callbacks no-op
    std::atomic<bool> failed{false};
    HRESULT failure = S_OK;
    std::string failureText;           // WHY, in the platform's own words (guarded by `mutex`)
    int64_t lastEncodeMs = 0;

    void Fail(HRESULT hr, std::string why)
    {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            failureText = std::move(why);
        }
        failure = hr;
        failed.store(true);
        running.store(false);
        cv.notify_all();
    }
    std::string Failure()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return failureText;
    }
};

struct WindowsVideo::PreviewTap {
    std::shared_ptr<VideoTapShared> shared = std::make_shared<VideoTapShared>();
    std::string deviceId;      // the symbolic-link id from Enumerate()
    std::string deviceName;    // the roster's friendly name — the reliable open key
    std::string mode;          // the UI's resolution pick ("1920x1080p60"), may be empty
    std::thread worker;        // joined by ~PreviewTap (same discipline as MeterTap)
    ~PreviewTap() { if (worker.joinable()) worker.join(); }
};

constexpr int64_t kDefaultPreviewFrameMs = 66;
constexpr int64_t kLiveJpegMs = 66; // desktop capture has no selected camera mode
constexpr auto kFlushWait = std::chrono::milliseconds(2000);

int64_t SteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool WaitOrStop(const std::shared_ptr<VideoTapShared> &shared, int ms)
{
    std::unique_lock<std::mutex> lock(shared->mutex);
    if (!shared->running.load())
        return false;
    shared->cv.wait_for(lock, std::chrono::milliseconds(ms));
    return shared->running.load();
}

constexpr UINT kWindowProbeMs = 200;
bool WindowIsAnswering(HWND hwnd)
{
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                               kWindowProbeMs, &result) != 0;
}

namespace {
std::vector<uint8_t> EncodeJpeg(const uint8_t *bgra, UINT32 w, UINT32 h, UINT32 stride, UINT32 quality);

inline void Yuv601ToBgra(int y, int cb, int cr, uint8_t *dst)
{
    const int r = y + ((91881 * cr) >> 16);
    const int g = y - ((22554 * cr + 46802 * cb) >> 16);
    const int b = y + ((116130 * cb) >> 16);
    dst[0] = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
    dst[1] = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
    dst[2] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
    dst[3] = 0xFF;
}
} // namespace

void StoreSampleFrame(VideoTapShared &shared, IMFSample *sample)
{
    IMFSourceReader *reader = shared.reader.load();
    IMFMediaBuffer *buf = nullptr;
    if (!reader || !sample || FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf)
        return;
    BYTE *data = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
        IMFMediaType *cur = nullptr;
        UINT32 w = 0, h = 0;
        GUID subtype = {};
        if (SUCCEEDED(reader->GetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
            UINT64 frameSize = 0;
            if (SUCCEEDED(cur->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))) {
                w = static_cast<UINT32>(frameSize >> 32);
                h = static_cast<UINT32>(frameSize & 0xFFFFFFFF);
            }
            cur->GetGUID(MF_MT_SUBTYPE, &subtype);
            cur->Release();
        }
        const auto publish = [&shared, w, h](const uint8_t *bgra, UINT32 stride) {
            const bool live = shared.live.load(std::memory_order_relaxed);
            if (live) {
                auto rgba = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4);
                uint8_t *dstBase = rgba->data();
                for (UINT32 y = 0; y < h; ++y) {
                    const uint8_t *src = bgra + static_cast<size_t>(y) * stride;
                    uint8_t *dst = dstBase + static_cast<size_t>(y) * w * 4;
                    for (UINT32 x = 0; x < w; ++x, src += 4, dst += 4) {
                        dst[0] = src[2];   // R <- the BGRA picture's red
                        dst[1] = src[1];
                        dst[2] = src[0];
                        dst[3] = 0xFF;
                    }
                }
                shared.PublishRgbaFrame(std::move(rgba), w, h);
            }
            {
                const std::lock_guard<std::mutex> lock(shared.mutex);
                const int64_t now = SteadyMs();
                const int64_t intervalMs = shared.frameIntervalMs.load(std::memory_order_relaxed);
                if (live && now - shared.lastEncodeMs < intervalMs)
                    return;
                // A LIVE tap with no pane reading JPEGs: the RGBA just published IS the picture for
                // every live consumer, so encoding a full frame here would spend the capture
                // callback's time on a buffer nobody reads — and this callback is the thread that
                // owes Media Foundation its next ReadSample on time.
                if (live && !shared.JpegWanted())
                    return;
                shared.lastEncodeMs = now;
            }
            std::vector<uint8_t> jpeg = EncodeJpeg(bgra, w, h, stride, 70);
            if (!jpeg.empty()) shared.Publish(std::move(jpeg));
        };
        if (w > 0 && h > 0 && subtype == MFVideoFormat_RGB32
            && curLen >= static_cast<DWORD>(w) * h * 4) {
            publish(data, w * 4);   // contiguous RGB32, BGRA byte order
        } else if (w > 0 && h > 0 && subtype == MFVideoFormat_YUY2
                   && curLen >= static_cast<DWORD>(w) * h * 2) {
            std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 4);
            for (UINT32 y = 0; y < h; ++y) {
                const uint8_t *src = data + static_cast<size_t>(y) * w * 2;
                uint8_t *dst = rgb.data() + static_cast<size_t>(y) * w * 4;
                for (UINT32 x = 0; x < w; x += 2) {
                    const int cb = static_cast<int>(src[1]) - 128;
                    const int cr = static_cast<int>(src[3]) - 128;
                    Yuv601ToBgra(src[0], cb, cr, dst);
                    Yuv601ToBgra(src[2], cb, cr, dst + 4);
                    src += 4;
                    dst += 8;
                }
            }
            publish(rgb.data(), w * 4);
        } else if (w > 0 && h > 0 && subtype == MFVideoFormat_NV12
                   && curLen >= static_cast<DWORD>(w) * h * 3 / 2) {
            std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 4);
            const uint8_t *yPlane = data;
            const uint8_t *uvPlane = data + static_cast<size_t>(w) * h;
            for (UINT32 y = 0; y < h; ++y) {
                const uint8_t *yRow = yPlane + static_cast<size_t>(y) * w;
                const uint8_t *uvRow = uvPlane + static_cast<size_t>(y / 2) * w;
                uint8_t *dst = rgb.data() + static_cast<size_t>(y) * w * 4;
                UINT32 x = 0;
                for (; x + 1 < w; x += 2) {
                    const int cb = static_cast<int>(uvRow[x]) - 128;
                    const int cr = static_cast<int>(uvRow[x + 1]) - 128;
                    Yuv601ToBgra(yRow[x], cb, cr, dst);
                    Yuv601ToBgra(yRow[x + 1], cb, cr, dst + 4);
                    dst += 8;
                }
                if (x < w) {   // odd width: one leftover column, its own pair's chroma
                    const int cb = static_cast<int>(uvRow[x & ~1u]) - 128;
                    const int cr = static_cast<int>(uvRow[(x & ~1u) + 1]) - 128;
                    Yuv601ToBgra(yRow[x], cb, cr, dst);
                }
            }
            publish(rgb.data(), w * 4);
        } else if (w > 0 && h > 0) {
            WCHAR subName[64] = {};
            StringFromGUID2(subtype, subName, 64);
            std::printf("[videopreview] frame skipped: w=%u h=%u curLen=%lu subtype=%ls\n",
                        w, h, static_cast<unsigned long>(curLen), subName);
            std::fflush(stdout);
        }
        buf->Unlock();
    }
    buf->Release();
}

class SourceReaderNotify final : public IMFSourceReaderCallback {
public:
    explicit SourceReaderNotify(std::shared_ptr<VideoTapShared> shared)
        : shared_(std::move(shared)) {}

    // ---- IUnknown (no shlwapi: QISearch would add a link dependency) ----
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&refs_));
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG left = static_cast<ULONG>(InterlockedDecrement(&refs_));
        if (left == 0) delete this;
        return left;
    }

    // ---- IMFSourceReaderCallback ----
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD /*stream*/, DWORD flags,
                              LONGLONG /*timestamp*/, IMFSample *sample) override
    {
        bool stopping = false;
        bool store = false;
        {
            const std::lock_guard<std::mutex> lock(shared_->mutex);
            // The worker is past this callback: the reader it owned is released,
            // so there is nothing to do and — critically — nothing to touch.
            if (shared_->stopping) return S_OK;
            stopping = !shared_->running.load();
            if (FAILED(hr)) {
                if (!stopping) {   // a cancelled read is not a dead stream
                    shared_->failed.store(true);
                    shared_->failure = hr;
                    shared_->cv.notify_all();
                }
                return S_OK;
            }
            if (sample && !(flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                const bool live = shared_->live.load(std::memory_order_relaxed);
                const int64_t now = SteadyMs();
                const int64_t intervalMs = shared_->frameIntervalMs.load(std::memory_order_relaxed);
                if (live || now - shared_->lastStoreMs >= intervalMs) {
                    shared_->lastStoreMs = now;
                    store = true;
                }
            }
        }
        if (store)
            StoreSampleFrame(*shared_, sample);
        {
            const std::lock_guard<std::mutex> lock(shared_->mutex);
            stopping = stopping || !shared_->running.load() || shared_->stopping;
            if (stopping) {
                shared_->cv.notify_all();   // the worker is waiting to shut this down
                return S_OK;
            }
            IMFSourceReader *reader = shared_->reader.load();
            if (reader)   // the next sample; this is the whole read loop
                reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                   nullptr, nullptr, nullptr, nullptr);
        }
        return S_OK;
    }

    STDMETHODIMP OnFlush(DWORD /*stream*/) override
    {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->flushDone = true;
        shared_->cv.notify_all();
        return S_OK;
    }

    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent *) override { return S_OK; }

private:
    ~SourceReaderNotify() = default;   // Release() owns deletion
    long refs_ = 1;
    std::shared_ptr<VideoTapShared> shared_;
};

struct WindowsVideo::PreviewTable {
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<PreviewTap>> taps;

    // ---- Why a tap died, answered WITHOUT the table's lock -------------------
    std::atomic<uint64_t> generation{0};

    std::string LastFailure(const std::string &deviceId, uint64_t gen) const
    {
        const std::lock_guard<std::mutex> lock(failureMutex_);
        const auto it = failureCache_.find(deviceId);
        if (it == failureCache_.end() || it->second.first != gen)
            return {};   // never read under this tap set: no honest answer yet
        return it->second.second;
    }

    void RememberFailure(const std::string &deviceId, uint64_t gen,
                         const std::string &reason) const
    {
        const std::lock_guard<std::mutex> lock(failureMutex_);
        failureCache_[deviceId] = std::make_pair(gen, reason);
    }

    // ---- Reaping: HOW A TAP DIES WITHOUT BLOCKING ITS CALLER -------------
    void Reap(std::unique_ptr<PreviewTap> tap)
    {
        if (!tap)
            return;
        {
            const std::lock_guard<std::mutex> lock(reapMutex_);
            ++reaping_;
        }
        // `this` is safe to capture: Clear() does not return until reaping_ is
        // back to 0, and Clear() is the destructor's own path.
        std::thread([this, owned = std::move(tap)]() mutable {
            owned.reset();   // ~PreviewTap -> join the capture worker
            {
                const std::lock_guard<std::mutex> lock(reapMutex_);
                --reaping_;
            }
            reapCv_.notify_all();
        }).detach();
    }

    void Erase(const std::string &deviceId)
    {
        std::unique_ptr<PreviewTap> tap;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            auto it = taps.find(deviceId);
            if (it == taps.end())
                return;
            it->second->shared->running.store(false);
            it->second->shared->cv.notify_all();
            tap = std::move(it->second);
            taps.erase(it);
            // The tap set moved: every cached failure answer is now stale (a
            // reader must not explain a tap that is no longer here).
            generation.fetch_add(1, std::memory_order_acq_rel);
        }
        Reap(std::move(tap));   // join OFF this thread — see Reap
    }

    void Clear()
    {
        std::map<std::string, std::unique_ptr<PreviewTap>> all;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            for (auto &[id, tap] : taps) {
                tap->shared->running.store(false);
                tap->shared->cv.notify_all();
            }
            all.swap(taps);
            generation.fetch_add(1, std::memory_order_acq_rel);   // see PreviewFailure
        }
        for (auto &[id, tap] : all)
            Reap(std::move(tap));
        std::unique_lock<std::mutex> lock(reapMutex_);
        while (reaping_ != 0) {
            if (reapCv_.wait_for(lock, kReapReport) == std::cv_status::timeout) {
                std::printf("[videopreview] teardown: %d capture worker(s) still exiting "
                            "after %lld ms\n",
                            reaping_, static_cast<long long>(kReapReport.count()));
                std::fflush(stdout);
            }
        }
    }

    ~PreviewTable() = default;

private:
    static constexpr std::chrono::milliseconds kReapReport{2000};

    mutable std::mutex failureMutex_;
    mutable std::map<std::string, std::pair<uint64_t, std::string>> failureCache_;

    std::mutex reapMutex_;
    std::condition_variable reapCv_;
    int reaping_ = 0;
};

WindowsVideo::WindowsVideo() : previews_(std::make_shared<PreviewTable>()) {}

WindowsVideo::~WindowsVideo()
{
    if (!previews_)
        return;
    previews_->Clear();
    previews_.reset();
}

namespace {
bool EnsureGdiplusStarted()
{
    // The real GdiplusStartup signature: (token*, input*, output*).
    using GdiplusBoot = LONG (__stdcall *)(ULONG_PTR *, void *, void *);
    static HMODULE gdipModule = nullptr;
    static GdiplusBoot gdipStartup = nullptr;
    static std::once_flag gdipOnce;
    std::call_once(gdipOnce, [] {
        gdipModule = LoadLibraryW(L"gdiplus.dll");
        if (gdipModule)
            gdipStartup = win::ProcAddress<GdiplusBoot>(gdipModule, "GdiplusStartup");
    });
    if (!gdipStartup)
        return false;

    static ULONG_PTR token = 0;
    static std::once_flag tokenOnce;
    std::call_once(tokenOnce, [] {
        Gdiplus::GdiplusStartupInput input;
        if (gdipStartup(&token, &input, nullptr) != Gdiplus::Ok)
            token = 0;   // unavailable — callers stay empty
    });
    return token != 0;
}

std::vector<uint8_t> EncodeJpeg(const uint8_t *bgra, UINT32 w, UINT32 h, UINT32 stride, UINT32 quality)
{
    if (!EnsureGdiplusStarted())
        return {};

    std::vector<uint8_t> out;
    {
        // PixelFormat32bppRGB is the BGRA byte order the reader hands us
        // (GDI+'s "RGB32" is byte-wise B,G,R,X — no channel swap needed).
        Gdiplus::Bitmap bmp(static_cast<INT>(w), static_cast<INT>(h),
                            static_cast<INT>(stride), PixelFormat32bppRGB,
                            const_cast<BYTE *>(bgra));
        CLSID jpgClsid = {};
        CLSIDFromString(L"{557CF401-1A04-11D3-9A73-0000F81EF32E}", &jpgClsid);
        IStream *stream = nullptr;
        if (bmp.GetLastStatus() == Gdiplus::Ok
            && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && stream) {
            // Quality via the encoder parameters block.
            Gdiplus::EncoderParameters params;
            params.Count = 1;
            params.Parameter[0].Guid = Gdiplus::EncoderQuality;
            params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            params.Parameter[0].NumberOfValues = 1;
            const ULONG q = quality;
            params.Parameter[0].Value = const_cast<ULONG *>(&q);
            if (bmp.Save(stream, &jpgClsid, &params) == Gdiplus::Ok) {
                HGLOBAL h = nullptr;
                if (SUCCEEDED(GetHGlobalFromStream(stream, &h)) && h) {
                    const SIZE_T size = GlobalSize(h);
                    const void *mem = GlobalLock(h);
                    if (mem && size > 0) {
                        out.assign(static_cast<const uint8_t *>(mem),
                                   static_cast<const uint8_t *>(mem) + size);
                    }
                    if (mem) GlobalUnlock(h);
                }
            }
            stream->Release();
        }
    }
    // (No GdiplusShutdown — see the start-once note above.)
    return out;
}

struct RawRgba { UINT32 width = 0; UINT32 height = 0; std::vector<uint8_t> rgba; };

RawRgba DecodeJpeg(const uint8_t *jpeg, size_t size)
{
    RawRgba out;
    if (!jpeg || size == 0 || !EnsureGdiplusStarted())
        return out;

    IStream *stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream)
        return out;
    ULONG written = 0;
    stream->Write(jpeg, static_cast<ULONG>(size), &written);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    Gdiplus::Bitmap bmp(stream);
    if (bmp.GetLastStatus() == Gdiplus::Ok) {
        const UINT w = bmp.GetWidth();
        const UINT h = bmp.GetHeight();
        if (w > 0 && h > 0) {
            Gdiplus::BitmapData data{};
            Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
            if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                             PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
                out.width = w;
                out.height = h;
                out.rgba.resize(static_cast<size_t>(w) * h * 4);
                const auto *base = static_cast<const uint8_t *>(data.Scan0);
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t *row = base + static_cast<size_t>(y) * data.Stride;
                    for (UINT x = 0; x < w; ++x) {
                        const uint8_t *px = row + static_cast<size_t>(x) * 4;
                        uint8_t *dst = &out.rgba[(static_cast<size_t>(y) * w + x) * 4];
                        dst[0] = px[2];
                        dst[1] = px[1];
                        dst[2] = px[0];
                        dst[3] = px[3];
                    }
                }
                bmp.UnlockBits(&data);
            }
        }
    }
    stream->Release();
    return out;
}
} // namespace

void WindowsVideo::PreviewThread(PreviewTap &tap)
{
    ScopedComInit com;

    const auto fail = [&tap](HRESULT hr, const char *why) {
        tap.shared->Fail(hr, why);
        std::printf("[videopreview] %s for %s (0x%08lX)\n", why, tap.deviceId.c_str(),
                    static_cast<unsigned long>(hr));
        std::fflush(stdout);
    };

    IMFMediaSource *source = nullptr;
    {
        IMFAttributes *attrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) && attrs) {
            attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                             win::Wide(tap.deviceId).c_str());
            if (FAILED(MFCreateDeviceSource(attrs, &source)) || !source)
                source = nullptr;   // fall through to the manual match below
            attrs->Release();
        }
    }
    IMFActivate *activate = nullptr;
    if (!source) {
        IMFAttributes *attrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) && attrs) {
            attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            IMFActivate **activates = nullptr;
            ULONG32 count = 0;
            if (SUCCEEDED(MFEnumDeviceSources(attrs, &activates, &count)) && count > 0 && activates) {
                const std::string wantLower = LowerAscii(tap.deviceId);
                IMFActivate *bySym = nullptr;
                IMFActivate *byName = nullptr;
                for (ULONG32 i = 0; i < count; ++i) {
                    WCHAR *strW = nullptr;
                    UINT32 strLen = 0;
                    if (!bySym
                        && SUCCEEDED(activates[i]->GetAllocatedString(
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                            &strW, &strLen)) && strW) {
                        if (LowerAscii(win::Utf8(std::wstring(strW, strLen))) == wantLower)
                            bySym = activates[i];
                        CoTaskMemFree(strW);
                    }
                    if (!byName
                        && SUCCEEDED(activates[i]->GetAllocatedString(
                            MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &strW, &strLen)) && strW) {
                        if (win::Utf8(std::wstring(strW, strLen)) == tap.deviceName)
                            byName = activates[i];
                        CoTaskMemFree(strW);
                    }
                }
                activate = bySym ? bySym : byName;   // exact id first, name second
                for (ULONG32 i = 0; i < count; ++i)
                    if (activates[i] != activate)
                        activates[i]->Release();
            }
            if (activates) CoTaskMemFree(activates);
            attrs->Release();
        }
    }
    if (!activate && !source) {
        fail(S_OK, "device activation failed");
        return;
    }

    if (activate) {
        if (FAILED(activate->ActivateObject(IID_IMFMediaSource,
                                            reinterpret_cast<void **>(&source))) || !source) {
            fail(S_OK, "ActivateObject failed");
            activate->Release();
            return;
        }
        activate->Release();
    }

    IMFSourceReader *reader = nullptr;
    SourceReaderNotify *notify = new SourceReaderNotify(tap.shared);
    {
        IMFAttributes *readerAttrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&readerAttrs, 2)) && readerAttrs) {
            readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            readerAttrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, notify);
            if (FAILED(MFCreateSourceReaderFromMediaSource(source, readerAttrs, &reader)) || !reader)
                reader = nullptr;
            readerAttrs->Release();
        }
    }
    if (!reader) {
        IMFAttributes *asyncOnly = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&asyncOnly, 1)) && asyncOnly) {
            asyncOnly->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, notify);
            if (FAILED(MFCreateSourceReaderFromMediaSource(source, asyncOnly, &reader)) || !reader)
                reader = nullptr;
            asyncOnly->Release();
        }
        if (!reader) {
            fail(S_OK, "MFCreateSourceReader failed");
            notify->Release();
            source->Release();
            return;
        }
    }

    UINT32 wantW = 0, wantH = 0;
    double wantFps = 0.0;
    {
        const size_t x = tap.mode.find('x');
        if (x != std::string::npos) {
            wantW = static_cast<UINT32>(std::strtoul(tap.mode.c_str(), nullptr, 10));
            wantH = static_cast<UINT32>(std::strtoul(tap.mode.c_str() + x + 1, nullptr, 10));
        }
        const size_t p = tap.mode.find('p');
        if (p != std::string::npos) {
            char *end = nullptr;
            const double parsed = std::strtod(tap.mode.c_str() + p + 1, &end);
            if (end != tap.mode.c_str() + p + 1 && std::isfinite(parsed) && parsed > 0.0)
                wantFps = parsed;
        }
    }
    // Keep pane/JPEG work in step with the selected camera mode, but cap it at
    // 30 fps: the Qt image provider decodes these frames on the UI thread.
    // Live RGBA frames continue at the camera's native cadence.
    if (wantFps > 0.0) {
        const double previewFps = std::min(wantFps, 30.0);
        tap.shared->frameIntervalMs.store(
            std::max<int64_t>(16, static_cast<int64_t>(std::ceil(1000.0 / previewFps))),
            std::memory_order_relaxed);
    } else {
        tap.shared->frameIntervalMs.store(kDefaultPreviewFrameMs, std::memory_order_relaxed);
    }
    auto decodable = [](const GUID &sub) {
        return sub == MFVideoFormat_RGB32 || sub == MFVideoFormat_RGB24
               || sub == MFVideoFormat_NV12 || sub == MFVideoFormat_YUY2
               || sub == MFVideoFormat_IYUV || sub == MFVideoFormat_MJPG;
    };
    bool typeSet = false;
    bool found = false;   // sentinel-free: "chosen" may legitimately be 0
    DWORD chosen = 0;
    if (wantW > 0 && wantH > 0) {
        HRESULT hrEnum = S_OK;
        double bestFpsDelta = std::numeric_limits<double>::infinity();
        for (DWORD t = 0; hrEnum == S_OK; ++t) {
            IMFMediaType *nat = nullptr;
            hrEnum = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, t, &nat);
            if (SUCCEEDED(hrEnum) && nat) {
                GUID sub{};
                nat->GetGUID(MF_MT_SUBTYPE, &sub);
                UINT64 frameSize = 0;
                if (decodable(sub)
                    && SUCCEEDED(nat->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))
                    && static_cast<UINT32>(frameSize >> 32) == wantW
                    && static_cast<UINT32>(frameSize & 0xFFFFFFFF) == wantH) {
                    double fpsDelta = 0.0;
                    if (wantFps > 0.0) {
                        UINT64 frameRate = 0;
                        if (FAILED(nat->GetUINT64(MF_MT_FRAME_RATE, &frameRate))) {
                            nat->Release();
                            continue;
                        }
                        const UINT32 numerator = static_cast<UINT32>(frameRate >> 32);
                        const UINT32 denominator = static_cast<UINT32>(frameRate & 0xFFFFFFFF);
                        if (denominator == 0) {
                            nat->Release();
                            continue;
                        }
                        fpsDelta = std::abs(static_cast<double>(numerator) / denominator - wantFps);
                    }
                    // The mode string includes FPS (for example 1280x720p30).
                    // Matching only width/height silently selected the first
                    // same-size mode, which could be a much slower capture rate.
                    if (!found || fpsDelta < bestFpsDelta) {
                        chosen = t;
                        found = true;
                        bestFpsDelta = fpsDelta;
                    }
                }
                nat->Release();
            }
            if (found && wantFps <= 0.0 && t >= chosen)
                break;
            if (t > 64)
                break;   // sanity cap — no device has 64 distinct sizes
        }
    }
    // No size pick (or none matched) → first DECODABLE type, not blind type 0.
    if (!found) {
        HRESULT hrEnum = S_OK;
        for (DWORD t = 0; hrEnum == S_OK; ++t) {
            IMFMediaType *nat = nullptr;
            hrEnum = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, t, &nat);
            if (SUCCEEDED(hrEnum) && nat) {
                GUID sub{};
                nat->GetGUID(MF_MT_SUBTYPE, &sub);
                if (decodable(sub)) {
                    chosen = t;
                    found = true;
                    nat->Release();
                    break;
                }
                nat->Release();
            }
            if (t > 64)
                break;
        }
    }
    if (!found) {
        fail(S_OK, "no decodable media type (H264-only virtual cam?)");
        reader->Release();
        notify->Release();
        source->Release();
        return;
    }
    IMFMediaType *native = nullptr;
    if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                             chosen, &native)) && native) {
        typeSet = SUCCEEDED(reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native));
        native->Release();
    }
    if (typeSet) {
        IMFMediaType *nativeType = nullptr;
        if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                 chosen, &nativeType)) && nativeType) {
            IMFMediaType *rgb = nullptr;
            if (SUCCEEDED(MFCreateMediaType(&rgb)) && rgb) {
                nativeType->CopyAllItems(rgb);
                rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                rgb->DeleteItem(MF_MT_DEFAULT_STRIDE);
                rgb->DeleteItem(MF_MT_AVG_BITRATE);   // YUY2's bitrate, same inconsistency
                if (FAILED(reader->SetCurrentMediaType(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb))) {
                    // Conversion refused — frames arrive in the native
                    // format and go through the YUY2 fallback below.
                }
                rgb->Release();
            }
            nativeType->Release();
        }
    }
    if (!typeSet) {
        fail(S_OK, "media type selection failed");
        reader->Release();
        notify->Release();
        source->Release();
        return;
    }

    IMFMediaType *activeType = nullptr;
    if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               &activeType)) && activeType) {
        UINT64 frameSize = 0, frameRate = 0;
        if (SUCCEEDED(activeType->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))
            && SUCCEEDED(activeType->GetUINT64(MF_MT_FRAME_RATE, &frameRate))) {
            const UINT32 width = static_cast<UINT32>(frameSize >> 32);
            const UINT32 height = static_cast<UINT32>(frameSize & 0xFFFFFFFF);
            const UINT32 numerator = static_cast<UINT32>(frameRate >> 32);
            const UINT32 denominator = static_cast<UINT32>(frameRate & 0xFFFFFFFF);
            const double fps = denominator > 0
                ? static_cast<double>(numerator) / denominator : 0.0;
            std::printf("[videopreview] selected %s: %ux%u @ %.2f fps (preview %lld ms)\n",
                        tap.deviceName.c_str(), width, height, fps,
                        static_cast<long long>(tap.shared->frameIntervalMs.load()));
            std::fflush(stdout);
        }
        activeType->Release();
    }

    // ---- the read chain: ASYNC, and therefore INTERRUPTIBLE ----------------
    tap.shared->reader.store(reader);
    {
        const std::lock_guard<std::mutex> lock(tap.shared->mutex);
        if (tap.shared->running.load())
            reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                               nullptr, nullptr, nullptr, nullptr);
    }

    {
        std::unique_lock<std::mutex> lock(tap.shared->mutex);
        while (tap.shared->running.load() && !tap.shared->failed)
            tap.shared->cv.wait_for(lock, std::chrono::milliseconds(250));
    }
    // ---- deterministic shutdown -------------------------------------------
    {
        std::unique_lock<std::mutex> lock(tap.shared->mutex);
        // No further requests: a callback still racing below must not re-arm the
        // chain while we are retiring it.
        tap.shared->reader.store(nullptr);
        lock.unlock();
        reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        lock.lock();
        if (!tap.shared->cv.wait_for(lock, kFlushWait, [&] { return tap.shared->flushDone; })) {
            std::printf("[videopreview] teardown: no OnFlush within %lld ms for %s — the "
                        "device ignored the cancellation\n",
                        static_cast<long long>(kFlushWait.count()), tap.deviceId.c_str());
            std::fflush(stdout);
        }
        tap.shared->stopping = true;
    }
    reader->Release();   // held past the flush on purpose: that is what retires it
    notify->Release();
    source->Release();
}

void WindowsVideo::ScreenPreviewThread(PreviewTap &tap)
{
    const bool isWindow = tap.deviceId.rfind("win:", 0) == 0;
    const auto giveUp = [&tap](const char *why) {
        tap.shared->Fail(S_OK, why);
        std::printf("[screenpreview] %s for %s\n", why, tap.deviceId.c_str());
        std::fflush(stdout);
    };
    HWND hwnd = nullptr;
    if (isWindow)
        hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(
            std::strtoull(tap.deviceId.c_str() + 4, nullptr, 10)));
    const std::wstring monitor = isWindow ? std::wstring() : win::Wide(tap.deviceId);
    int consecutiveFailures = 0;
    bool wasMinimized = false;   // one-shot print, not the give-up counter
    bool wasUnanswering = false; // one-shot print, same discipline as minimized
    while (tap.shared->running.load(std::memory_order_relaxed)) {
        if (isWindow && !IsWindow(hwnd)) {
            giveUp("window closed");
            break;
        }
        if (isWindow && IsIconic(hwnd)) {
            if (!wasMinimized) {
                std::printf("[screenpreview] window minimized, holding last frame: %s\n",
                            tap.deviceId.c_str());
                std::fflush(stdout);
                wasMinimized = true;
            }
            WaitOrStop(tap.shared, 100);
            continue;
        }
        wasMinimized = false;
        if (isWindow && !WindowIsAnswering(hwnd)) {
            if (!wasUnanswering) {
                std::printf("[screenpreview] window not answering, holding last frame: %s\n",
                            tap.deviceId.c_str());
                std::fflush(stdout);
                wasUnanswering = true;
            }
            WaitOrStop(tap.shared, 100);
            continue;
        }
        wasUnanswering = false;
        HDC screenDc = nullptr;
        if (!isWindow) {
            screenDc = CreateDCW(monitor.c_str(), nullptr, nullptr, nullptr);
        }
        HDC memDc = CreateCompatibleDC(screenDc);
        if (!screenDc && !memDc) {
            if (++consecutiveFailures < 20) {
                WaitOrStop(tap.shared, 100);
                continue;
            }
            giveUp("display or window device context unavailable");
            break;
        }
        const int srcW = isWindow
            ? [] (HWND h) { RECT r{}; GetClientRect(h, &r); return (int)(r.right - r.left); }(hwnd)
            : GetDeviceCaps(screenDc, HORZRES);
        const int srcH = isWindow
            ? [] (HWND h) { RECT r{}; GetClientRect(h, &r); return (int)(r.bottom - r.top); }(hwnd)
            : GetDeviceCaps(screenDc, VERTRES);
        if (srcW <= 0 || srcH <= 0) {
            if (memDc) DeleteDC(memDc);
            if (screenDc) DeleteDC(screenDc);
            if (++consecutiveFailures >= 20)
                break;   // window gone zero-sized / monitor vanished — stop hammering
            WaitOrStop(tap.shared, 66);
            continue;
        }
        const bool live = tap.shared->live.load(std::memory_order_relaxed);
        const int dstW = live ? srcW : std::min(srcW, 960);
        const int dstH = live ? srcH : std::max(1, srcH * dstW / srcW);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = dstW;
        bi.bmiHeader.biHeight = -dstH;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP dib = CreateDIBSection(memDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = dib ? SelectObject(memDc, dib) : nullptr;
        bool gotFrame = false;
        if (dib && bits) {
            SetStretchBltMode(memDc, COLORONCOLOR);
            BOOL blitOk = FALSE;
            if (isWindow) {
                BITMAPINFO fbi{};
                fbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                fbi.bmiHeader.biWidth = srcW;
                fbi.bmiHeader.biHeight = -srcH;   // top-down, like the small DIB
                fbi.bmiHeader.biPlanes = 1;
                fbi.bmiHeader.biBitCount = 32;
                fbi.bmiHeader.biCompression = BI_RGB;
                void *fbits = nullptr;
                HDC fullDc = CreateCompatibleDC(nullptr);
                HBITMAP fdib = fullDc ? CreateDIBSection(fullDc, &fbi, DIB_RGB_COLORS,
                                                         &fbits, nullptr, 0) : nullptr;
                HGDIOBJ fold = fdib ? SelectObject(fullDc, fdib) : nullptr;
                if (fdib && fbits) {
                    if (PrintWindow(hwnd, fullDc, PW_CLIENTONLY | PW_RENDERFULLCONTENT))
                        blitOk = StretchBlt(memDc, 0, 0, dstW, dstH, fullDc,
                                            0, 0, srcW, srcH, SRCCOPY);
                    else if (consecutiveFailures == 0)
                        std::printf("[screenpreview] PrintWindow failed for %s\n",
                                    tap.deviceId.c_str()), std::fflush(stdout);
                }
                if (fdib) {
                    SelectObject(fullDc, fold);
                    DeleteObject(fdib);
                }
                if (fullDc) DeleteDC(fullDc);
            } else {
                blitOk = StretchBlt(memDc, 0, 0, dstW, dstH, screenDc,
                                    0, 0, srcW, srcH, SRCCOPY | CAPTUREBLT);
            }
            if (blitOk) {
                const UINT32 stride = static_cast<UINT32>(dstW) * 4;
                const int64_t now = SteadyMs();
                if (live) {
                    std::vector<uint8_t> rgba(static_cast<size_t>(stride) * dstH);
                    const uint8_t *src = static_cast<const uint8_t *>(bits);
                    for (size_t i = 0; i < rgba.size(); i += 4) {
                        rgba[i] = src[i + 2];
                        rgba[i + 1] = src[i + 1];
                        rgba[i + 2] = src[i];
                        rgba[i + 3] = 0xFF;
                    }
                    tap.shared->PublishRgba(static_cast<uint32_t>(dstW),
                                            static_cast<uint32_t>(dstH), std::move(rgba));
                    gotFrame = true;
                }
                bool encodeJpeg = true;
                {
                    const std::lock_guard<std::mutex> frameLock(tap.shared->mutex);
                    encodeJpeg = !live || now - tap.shared->lastEncodeMs >= kLiveJpegMs;
                    if (encodeJpeg)
                        tap.shared->lastEncodeMs = now;
                }
                if (encodeJpeg) {
                    // Quality 85 (was 70) — quality-70 JPEG blocking on fine UI
                    // text is what "rumbled up" chat text actually was.
                    std::vector<uint8_t> jpeg = EncodeJpeg(static_cast<const uint8_t *>(bits),
                                                           static_cast<uint32_t>(dstW),
                                                           static_cast<uint32_t>(dstH), stride, 85);
                    if (!jpeg.empty()) {
                        tap.shared->Publish(std::move(jpeg));
                        gotFrame = true;
                    }
                }
            }
        }
        if (dib) {
            SelectObject(memDc, old);
            DeleteObject(dib);
        }
        if (memDc) DeleteDC(memDc);
        if (screenDc) DeleteDC(screenDc);
        if (!gotFrame) {
            if (++consecutiveFailures >= 20) {
                giveUp("no frame could be grabbed");
                break;   // nothing blits for ~2 s — the source is gone, stop
            }
            WaitOrStop(tap.shared, 30);   // a failed grab retries a bit slower; success loops at ~15 fps
        } else {
            consecutiveFailures = 0;
            // A live tap grabs as fast as GDI will let it (16 ms = the display
            // refresh a bus window is meant to follow); a pane keeps ~15 fps.
            WaitOrStop(tap.shared, live ? 16 : 66);
        }
    }
}

Result<void> WindowsVideo::StartScreenPreview(const std::string &monitorId)
{
    const bool isWindow = monitorId.rfind("win:", 0) == 0;
    if (!isWindow && monitorId.rfind("\\\\.\\DISPLAY", 0) != 0)
        return Error::Make(Err::NotFound, "Video",
                           "not a monitor id: " + monitorId);
    if (isWindow) {
        // Validate the hwnd NOW — a stale id from a closed window must fail
        // with NotFound instead of starting a thread that dies on grab 1.
        const HWND hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(
            std::strtoull(monitorId.c_str() + 4, nullptr, 10)));
        if (!IsWindow(hwnd) || !IsWindowVisible(hwnd))
            return Error::Make(Err::NotFound, "Video",
                               "window no longer exists: " + monitorId);
    }

    std::unique_ptr<PreviewTap> stale;
    {
        const std::lock_guard<std::mutex> lock(previews_->mutex);
        auto &slot = previews_->taps[monitorId];
        if (slot && slot->shared->running.load() && !slot->shared->failed.load())
            return Ok();
        if (slot) {
            slot->shared->running.store(false);
            slot->shared->cv.notify_all();
            stale = std::move(slot);
        }
        auto tap = std::make_unique<PreviewTap>();
        tap->deviceId = monitorId;
        tap->shared->running.store(true);
        tap->worker = std::thread(&WindowsVideo::ScreenPreviewThread, std::ref(*tap));
        slot = std::move(tap);
        previews_->generation.fetch_add(1, std::memory_order_acq_rel);   // see PreviewFailure
    }
    previews_->Reap(std::move(stale));
    return Ok();
}

Result<void> WindowsVideo::StopScreenPreview(const std::string &monitorId)
{
    previews_->Erase(monitorId);
    return Ok();
}

Result<void> WindowsVideo::StartPreview(const std::string &deviceId, const std::string &mode,
                                        const std::string &deviceName)
{
    std::string lowerName = deviceName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowerName.find("ndi") != std::string::npos)
        return Error::Make(Err::Unsupported, "Video",
                           "NDI sources are served by the NDI pipeline, not the camera preview tap");

    std::unique_ptr<PreviewTap> stale;
    {
        const std::lock_guard<std::mutex> lock(previews_->mutex);
        auto &slot = previews_->taps[deviceId];
        if (slot && slot->shared->running.load() && !slot->shared->failed.load()
            && slot->mode == mode)
            return Ok();
        if (slot) {
            slot->shared->running.store(false);
            slot->shared->cv.notify_all();
            stale = std::move(slot);
        }
        auto tap = std::make_unique<PreviewTap>();
        tap->deviceId = deviceId;
        tap->deviceName = deviceName;
        tap->mode = mode;
        tap->shared->running.store(true);
        tap->worker = std::thread(&WindowsVideo::PreviewThread, std::ref(*tap));
        slot = std::move(tap);
        // A reader's cached failure answer is only good for the tap set it was
        // read under (see PreviewTable::generation).
        previews_->generation.fetch_add(1, std::memory_order_acq_rel);
    }
    previews_->Reap(std::move(stale));
    return Ok();
}

Result<void> WindowsVideo::StopPreview(const std::string &deviceId)
{
    previews_->Erase(deviceId);
    return Ok();
}

std::vector<uint8_t> WindowsVideo::PreviewFrame(const std::string &deviceId)
{
    const std::lock_guard<std::mutex> lock(previews_->mutex);
    auto it = previews_->taps.find(deviceId);
    if (it == previews_->taps.end())
        return {};
    return it->second->shared->Frame();
}

IVideo::DecodedFrame WindowsVideo::PreviewFramePixels(const std::string &deviceId)
{
    IVideo::DecodedFrame out;
    std::shared_ptr<const std::vector<uint8_t>> pixels;
    std::vector<uint8_t> jpeg;
    uint32_t w = 0, h = 0;
    {
        const std::lock_guard<std::mutex> lock(previews_->mutex);
        auto it = previews_->taps.find(deviceId);
        if (it == previews_->taps.end())
            return {};
        auto &shared = *it->second->shared;
        const std::lock_guard<std::mutex> frameLock(shared.mutex);
        out.seq = shared.frameSeq.load(std::memory_order_acquire);
        pixels = shared.latestRgba;
        w = shared.rgbaW;
        h = shared.rgbaH;
        if (!pixels)
            jpeg = shared.latestJpeg;
    }
    if (pixels) {
        out.width = w;
        out.height = h;
        static const bool legacyLockedCopy = std::getenv("BPS_TAP_COPY_UNDER_LOCK") != nullptr;
        if (legacyLockedCopy) {
            const std::lock_guard<std::mutex> lock(previews_->mutex);
            auto it = previews_->taps.find(deviceId);
            if (it != previews_->taps.end()) {
                const std::lock_guard<std::mutex> frameLock(it->second->shared->mutex);
                if (it->second->shared->latestRgba)
                    out.rgba = *it->second->shared->latestRgba;
            }
            return out;
        }
        out.rgba = *pixels;   // the only copy, and it holds no lock
        return out;
    }
    if (jpeg.empty())
        return {};
    RawRgba raw = DecodeJpeg(jpeg.data(), jpeg.size());   // decode outside the table's lock
    out.width = raw.width;
    out.height = raw.height;
    out.rgba = std::move(raw.rgba);
    return out;
}

// THE LIVE FRAME, SHARED. A live tap stores its newest RGBA as one immutable buffer (latestRgba); this
// hands that same buffer out instead of copying it, so the consumer's cost per frame is its own one copy.
// A tap still in pane mode holds only a JPEG: that falls back to the base wrapper (decode, one copy).
IVideo::SharedFrame WindowsVideo::PreviewFrameShared(const std::string &deviceId)
{
    {
        const std::lock_guard<std::mutex> lock(previews_->mutex);
        const auto it = previews_->taps.find(deviceId);
        if (it == previews_->taps.end())
            return {};
        auto &shared = *it->second->shared;
        const std::lock_guard<std::mutex> frameLock(shared.mutex);
        if (shared.latestRgba) {
            SharedFrame out;
            out.width = shared.rgbaW;
            out.height = shared.rgbaH;
            out.seq = shared.frameSeq.load(std::memory_order_acquire);
            out.rgba = shared.latestRgba;
            return out;
        }
    }
    return IVideo::PreviewFrameShared(deviceId);   // pane-mode tap: the JPEG path, outside the table's lock
}

uint64_t WindowsVideo::PreviewFrameSeq(const std::string &deviceId)
{
    const std::lock_guard<std::mutex> lock(previews_->mutex);
    auto it = previews_->taps.find(deviceId);
    if (it == previews_->taps.end())
        return 0;
    return it->second->shared->frameSeq.load(std::memory_order_acquire);
}

void WindowsVideo::SetPreviewLive(const std::string &deviceId, bool live)
{
    const std::lock_guard<std::mutex> lock(previews_->mutex);
    auto it = previews_->taps.find(deviceId);
    if (it == previews_->taps.end())
        return;
    auto &shared = *it->second->shared;
    shared.live.store(live, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> frameLock(shared.mutex);
        if (live) {
            // Nothing already held should count against the first live store:
            // the pane gate's timestamp is about the pane rate.
            shared.lastStoreMs = 0;
        } else {
            shared.latestRgba.reset();
            shared.rgbaW = shared.rgbaH = 0;
        }
    }
    std::printf("[videopreview] tap %s -> %s\n", deviceId.c_str(),
                live ? "live (program rate, native size)" : "pane (15 fps JPEG)");
    std::fflush(stdout);
}

std::string WindowsVideo::PreviewFailure(const std::string &deviceId) const
{
    if (!previews_)
        return {};
    const uint64_t gen = previews_->generation.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lock(previews_->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return previews_->LastFailure(deviceId, gen);
    auto it = previews_->taps.find(deviceId);
    if (it == previews_->taps.end())
        return {};
    if (!it->second->shared->failed.load())
        return {};
    const std::string reason = it->second->shared->Failure();
    previews_->RememberFailure(deviceId, gen, reason);
    return reason;
}

std::vector<std::string> WindowsVideo::ActivePreviews() const
{
    std::vector<std::string> ids;
    const std::lock_guard<std::mutex> lock(previews_->mutex);
    for (const auto &[id, tap] : previews_->taps)
        if (tap->shared->running.load()) ids.push_back(id);
    return ids;
}
} // namespace bps::platform
