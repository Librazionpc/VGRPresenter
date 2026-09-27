// Windows IVideo backend — Media Foundation (mfplat/mf/device). REAL
// enumeration: video-capture devices from MF_DEVSOURCE, each device's modes
// from its media source's presentation descriptor media types (frame size +
// frame-rate range per type — the actual sensor/capture-path capabilities,
// not a curated list). Max fps = the fastest rate any mode reports.
//
// MF runs on COM (MTA); every entry point takes the ScopedComInit guard.
// CCOMMJOR error paths degrade to empty lists — enumeration must never
// throw into the kernel watcher.

#define INITGUID
#include "platform/windows/WindowsVideo.hpp"

#include "platform/windows/WinUtil.hpp"

#include <windows.h>
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
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
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

} // namespace

std::vector<VideoDeviceInfo> WindowsVideo::Enumerate() const
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
        info.isCamera = true;

        // Modes: activate the source and walk its media types. A device
        // that fails to activate still lists (with no modes) — the UI
        // treats empty modes as "device present, capabilities unknown".
        IMFMediaSource *source = nullptr;
        if (SUCCEEDED(activates[i]->ActivateObject(IID_IMFMediaSource,
                                                   reinterpret_cast<void **>(&source))) && source) {
            IMFPresentationDescriptor *pd = nullptr;
            if (SUCCEEDED(source->CreatePresentationDescriptor(&pd)) && pd) {
                DWORD sdCount = 0;
                if (SUCCEEDED(pd->GetStreamDescriptorCount(&sdCount))) {
                    // Coalesce media types by frame size; collect each
                    // size's discrete rates (media types repeat a size at
                    // every rate the sensor offers).
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

// Open, visible, capturable top-level windows — the Screen source's
// "window" options (OBS-style Window Capture). Filters: visible, titled,
// not cloaked (UWP ghosts), not toolwindows, not our own process's
// windows. Sorted by title so the picker is stable across opens.
struct EnumWindowsCtx {
    std::vector<WindowInfo> out;
    DWORD selfPid = 0;
};

BOOL CALLBACK WindowEnumProc(HWND hwnd, LPARAM lParam)
{
    auto *ctx = reinterpret_cast<EnumWindowsCtx *>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    // Cloaked (DWM): invisible UWP/ghost windows pass IsWindowVisible but
    // render nothing — skip them.
    BOOL cloaked = FALSE;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked)
        return TRUE;
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
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

std::string WindowsVideo::Fingerprint() const
{
    std::string fp;
    for (const auto &d : Enumerate())
        fp += d.id + ";";
    return fp;
}

// ============================================================================
// Preview taps — a REAL MF Source Reader per previewed device. The drain
// thread owns every COM object it touches; the GUI thread only ever copies
// the newest JPEG out of the tap under the table's mutex (same hand-off
// discipline as WindowsAudio's meter taps).
//
// Scoped deliberately: native media type (no resolution negotiation), RGB32
// output conversion via the reader's built-in converter, JPEG encode by the
// GDI+ encoder that ships with Windows (no new dependency), one frame every
// ~66 ms (≈15 fps — plenty for a dialog pane, far below the sensor's load).
// ============================================================================
struct WindowsVideo::PreviewTap {
    std::atomic<bool> running{false};
    std::string deviceId;      // the symbolic-link id from Enumerate()
    std::string deviceName;    // the roster's friendly name — the reliable open key
    std::string mode;          // the UI's resolution pick ("1920x1080p60"), may be empty
    std::vector<uint8_t> latestJpeg;   // written by the thread under the table's mutex
    std::thread worker;        // joined by ~PreviewTap (same discipline as MeterTap)
    ~PreviewTap() { if (worker.joinable()) worker.join(); }
};

// Screen taps key by the monitor id ("\\\.\DISPLAY1" from IMonitor's
// Enumerate) in the SAME table as camera taps — the id namespaces cannot
// collide ("\\\.\DISPLAY" vs camera symlinks), and PreviewFrame/Stop stay
// single-surface for the bridge.

struct WindowsVideo::PreviewTable {
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<PreviewTap>> taps;

    void Erase(const std::string &deviceId)
    {
        std::unique_ptr<PreviewTap> tap;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            auto it = taps.find(deviceId);
            if (it == taps.end())
                return;
            it->second->running.store(false);
            tap = std::move(it->second);
            taps.erase(it);
        }
        tap.reset();   // join OUTSIDE the lock — the thread publishes under it
    }

    void Clear()
    {
        std::map<std::string, std::unique_ptr<PreviewTap>> all;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            for (auto &[id, tap] : taps)
                tap->running.store(false);
            all.swap(taps);
        }
        all.clear();   // joins, mutex free
    }

    ~PreviewTable() { Clear(); }
};

WindowsVideo::WindowsVideo() : previews_(std::make_unique<PreviewTable>()) {}
WindowsVideo::~WindowsVideo() = default;   // ~PreviewTable joins every tap

namespace {

// Encode raw BGRA pixels to JPEG via GDI+ (shipped with Windows; the same
// encoder Windows' own snipping tools use). Returns empty on any failure —
// the tap then keeps the previous frame instead of publishing garbage.
//
// GDIP BOOT: each DRAIN THREAD resolves GdiplusStartup once at its own
// start (thread_local) — the lazy static inside the encode path raced the
// GUI thread's first requestImage decode, and GDI+ startup itself is not
// guaranteed reentrant. GDI+ IS multi-thread-safe once started, so one
// process-wide init from the first tap thread + per-encode token
// scope (the documented contract) is the safe shape.
std::vector<uint8_t> EncodeJpeg(const uint8_t *bgra, UINT32 w, UINT32 h, UINT32 stride, UINT32 quality)
{
    // The real GdiplusStartup signature: (token*, input*, output*).
    using GdiplusBoot = LONG (__stdcall *)(ULONG_PTR *, void *, void *);
    static HMODULE gdipModule = nullptr;   // written once by the first tap thread
    static GdiplusBoot gdipStartup = nullptr;
    static std::once_flag gdipOnce;
    std::call_once(gdipOnce, [] {
        gdipModule = LoadLibraryW(L"gdiplus.dll");
        if (gdipModule)
            gdipStartup = win::ProcAddress<GdiplusBoot>(gdipModule, "GdiplusStartup");
    });
    if (!gdipStartup)
        return {};

    // START ONCE PER PROCESS, NEVER SHUTDOWN: the earlier per-encode
    // Startup/Shutdown cycle crashed the process seconds into a live
    // stream (GDI+ keeps background worker threads that outlive each
    // cycle). One deliberate init kept for the process lifetime is the
    // standard pattern for long-lived GDI+ hosts.
    static ULONG_PTR token = 0;
    static std::once_flag tokenOnce;
    std::call_once(tokenOnce, [] {
        Gdiplus::GdiplusStartupInput input;
        if (gdipStartup(&token, &input, nullptr) != Gdiplus::Ok)
            token = 0;   // encoder unavailable — publishes stay empty
    });
    if (token == 0)
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

} // namespace

// (A member-function definition must live OUTSIDE the anonymous namespace
// above — GCC rejects it inside. EncodeJpeg stays hidden there; the tap
// thread below is a WindowsVideo member and stays visible.)

// The drain thread: open the device in a Source Reader, request RGB32 at
// the UI's picked frame size when one was given (falls back to the native
// default), loop ReadSample → JPEG → publish. Exits when the tap's running
// flag drops, the device vanishes (MF_E_NO_MORE_TYPES on a hot-unplug
// surfaces as a read failure), or the encoder never engages.
void WindowsVideo::PreviewThread(PreviewTap &tap, std::mutex &publishMutex)
{
    ScopedComInit com;

    // Open the EXACT device — by symbolic link, never by enumeration order.
    // MFEnumDeviceSources IGNORES the symbolic-link/friendly-name attributes
    // as enumeration filters (telemetry: a symlink-filtered query returned
    // ALL 10 cameras, and activates[0] was always the first device), so both
    // "filtered" lookups here previously resolved every id to whatever camera
    // enumerated first and every tap opened the SAME physical camera. The
    // documented per-device open is MFCreateDeviceSource with the symlink
    // attribute set — it matches that symlink alone and touches no other
    // device. (MFCreateDeviceSourceActivate is not declared in MinGW's
    // headers; the IMFMediaSource this thread needs comes straight back.)
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
        // Fallback: enumerate every camera and match the symlink MANUALLY
        // (case-insensitive), friendly name second — a re-plugged device can
        // change its symlink suffix, and the roster name is then the last
        // honest key. Nothing is released until the winner is picked: an
        // earlier shape released non-matches while scanning and the name
        // pass then walked dangling pointers.
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
        std::printf("[videopreview] device activation failed for %s\n", tap.deviceId.c_str());
        std::fflush(stdout);
        return;
    }

    if (activate) {
        if (FAILED(activate->ActivateObject(IID_IMFMediaSource,
                                            reinterpret_cast<void **>(&source))) || !source) {
            std::printf("[videopreview] ActivateObject failed\n");
            std::fflush(stdout);
            activate->Release();
            return;
        }
        activate->Release();
    }

    IMFSourceReader *reader = nullptr;
    // Advanced video processing ON: several camera stacks (the integrated
    // webcam among them) have no native RGB32 type and their built-in
    // converter refused a plain subtype-override, so the reader must be
    // allowed to insert its own video processor (deinterlace/colorspace
    // MFT) for the NV12→RGB32 hop. Without it the frames arrive NV12 and
    // every one fails the RGB32 buffer check ("frame skipped" spam) — the
    // "integrated cam shows nothing" defect.
    {
        IMFAttributes *readerAttrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&readerAttrs, 1)) && readerAttrs) {
            readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            if (FAILED(MFCreateSourceReaderFromMediaSource(source, readerAttrs, &reader)) || !reader)
                reader = nullptr;
            readerAttrs->Release();
        }
    }
    if (!reader) {
        // Attr path failed outright — retry bare; the YUY2 fallback below
        // still covers the native formats this device delivers.
        if (FAILED(MFCreateSourceReaderFromMediaSource(source, nullptr, &reader)) || !reader) {
            std::printf("[videopreview] MFCreateSourceReader failed\n");
            std::fflush(stdout);
            source->Release();
            return;
        }
    }

    // Steer the reader to RGB32 at the UI's picked frame size: walk the
    // device's native types, prefer the first whose size matches the pick
    // (width×height; the fps in the pick only breaks ties between same-size
    // types), else fall back to type 0 (the device default). Sensors hand
    // us NV12/YUY2 — the reader's built-in converter makes RGB32.
    //
    // DECODABLE-TYPES ONLY: some virtual cameras (NDI Webcam) expose ONLY
    // H264/HEVC native types, and driving the reader through their built-in
    // decoder crashes inside that driver. Restricted to formats this tap
    // can actually consume (uncompressed RGB32/NV12/YUY2/IYUV + MJPG), a
    // device with no such type gets its stream rejected EARLY with
    // Unsupported instead of crashing mid-ReadSample. MJPG is kept: the
    // reader decodes it reliably and it is a common capture-card type.
    UINT32 wantW = 0, wantH = 0;
    {
        const size_t x = tap.mode.find('x');
        if (x != std::string::npos) {
            wantW = static_cast<UINT32>(std::strtoul(tap.mode.c_str(), nullptr, 10));
            wantH = static_cast<UINT32>(std::strtoul(tap.mode.c_str() + x + 1, nullptr, 10));
        }
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
                    chosen = t;   // first same-size, decodable native type wins
                    found = true;
                }
                nat->Release();
            }
            if (found && t >= chosen)
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
        // H264/HEVC-only virtual cam (NDI Webcam et al.): this tap cannot
        // consume its stream without the driver's own decoder path, which
        // is what crashes. Refuse cleanly — the UI keeps its glyph and the
        // device stays usable by the graph capture path.
        std::printf("[videopreview] no decodable media type (H264-only virtual cam?) for %s\n",
                    tap.deviceId.c_str());
        std::fflush(stdout);
        reader->Release();
        source->Release();
        return;
    }
    // THE DOCUMENTED TWO-STEP: select a COMPLETE native type first (a fresh
    // reader has NO current type — setting only a partial RGB32 type on top
    // of nothing fails on most cameras, and the tap died there before ever
    // reading a frame), THEN set the partial RGB32 type so the reader's
    // built-in converter kicks in.
    IMFMediaType *native = nullptr;
    if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                             chosen, &native)) && native) {
        typeSet = SUCCEEDED(reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native));
        native->Release();
    }
    if (typeSet) {
        // CLONE-AND-OVERRIDE, not a bare partial type: a fresh RGB32 type
        // with only major+subtype set is refused by many cameras (no frame
        // rate/size attributes) — the reader then silently stays on the
        // NATIVE format (NV12/YUY2), and every downstream frame fails the
        // RGB32 buffer check (skipped forever, pane never lights). Cloning
        // the selected native type and overriding ONLY the subtype keeps
        // the geometry the device agreed to and asks the reader's built-in
        // converter for RGB32 — the documented, always-accepted form.
        IMFMediaType *nativeType = nullptr;
        if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                 chosen, &nativeType)) && nativeType) {
            IMFMediaType *rgb = nullptr;
            if (SUCCEEDED(MFCreateMediaType(&rgb)) && rgb) {
                nativeType->CopyAllItems(rgb);
                rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                // CRITICAL: drop the native format's MF_MT_DEFAULT_STRIDE —
                // it says 2560 (YUY2's 2-byte stride), which contradicts
                // RGB32's 4-byte stride and makes SetCurrentMediaType fail
                // (MF_E_INVALIDMEDIATYPE). Deleting it lets the converter
                // compute the RGB32 stride itself. (Telemetry proved the
                // missing DeleteItem: frames kept arriving YUY2.)
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
        // No Qt logging in the engine's PAL — printf telemetry (the UI keeps
        // its glyph; the honest "no frames" signal).
        std::printf("[videopreview] media type selection failed\n");
        std::fflush(stdout);
        reader->Release();
        source->Release();
        return;
    }

    while (tap.running.load(std::memory_order_relaxed)) {
        DWORD streamFlags = 0;
        IMFSample *sample = nullptr;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                        nullptr, &streamFlags, nullptr, &sample);
        if (FAILED(hr)) {
            std::printf("[videopreview] ReadSample failed: 0x%08lX\n", static_cast<unsigned long>(hr));
            std::fflush(stdout);
            break;   // device gone / stream error — tap dies, UI keeps glyph
        }
        if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) || !sample) {
            if (sample) sample->Release();
            Sleep(10);
            continue;
        }

        IMFMediaBuffer *buf = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
            BYTE *data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
                // Frame geometry + subtype from the CURRENT type (post-
                // conversion) — BOTH read before the release (the earlier
                // shape released first and read the subtype from the
                // dangling pointer: a use-after-free that took the test
                // binary down mid-pal). Then a size cross-check against
                // the buffer the reader actually delivered — never read
                // past a short buffer.
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
                if (w > 0 && h > 0 && subtype == MFVideoFormat_RGB32
                    && curLen >= static_cast<DWORD>(w) * h * 4) {
                    const UINT32 stride = w * 4;   // contiguous RGB32
                    std::vector<uint8_t> jpeg = EncodeJpeg(data, w, h, stride, 70);
                    if (!jpeg.empty()) {
                        const std::lock_guard<std::mutex> lock(publishMutex);
                        tap.latestJpeg = std::move(jpeg);
                    }
                } else if (w > 0 && h > 0 && subtype == MFVideoFormat_YUY2
                           && curLen >= static_cast<DWORD>(w) * h * 2) {
                    // YUY2 packed 4:2:2 — [Y0 U Y1 V] per pixel pair. The
                    // BT.601 limited-range lift used by every capture stack.
                    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 4);
                    for (UINT32 y = 0; y < h; ++y) {
                        const uint8_t *src = data + static_cast<size_t>(y) * w * 2;
                        uint8_t *dst = rgb.data() + static_cast<size_t>(y) * w * 4;
                        for (UINT32 x = 0; x < w; x += 2) {
                            const float y0 = src[0], u = src[1], y1 = src[2], v = src[3];
                            const float cb = u - 128.f, cr = v - 128.f;
                            auto clamp8 = [](float v) {
                                return static_cast<uint8_t>(v < 0.f ? 0 : v > 255.f ? 255 : v);
                            };
                            dst[0] = clamp8(y0 + 1.772f * cb);            // B
                            dst[1] = clamp8(y0 - 0.344136f * cr - 0.714136f * cb);   // G
                            dst[2] = clamp8(y0 + 1.402f * cr);            // R
                            dst[3] = 0xFF;
                            dst[4] = clamp8(y1 + 1.772f * cb);
                            dst[5] = clamp8(y1 - 0.344136f * cr - 0.714136f * cb);
                            dst[6] = clamp8(y1 + 1.402f * cr);
                            dst[7] = 0xFF;
                            src += 4;
                            dst += 8;
                        }
                    }
                    std::vector<uint8_t> jpeg = EncodeJpeg(rgb.data(), w, h, w * 4, 70);
                    if (!jpeg.empty()) {
                        const std::lock_guard<std::mutex> lock(publishMutex);
                        tap.latestJpeg = std::move(jpeg);
                    }
                } else if (w > 0 && h > 0 && subtype == MFVideoFormat_NV12
                           && curLen >= static_cast<DWORD>(w) * h * 3 / 2) {
                    // NV12 planar 4:2:0 — full-res Y plane, then interleaved
                    // UV at half resolution. Same BT.601 lift as YUY2. The
                    // integrated webcam delivers this when its driver refuses
                    // the reader's RGB32 conversion.
                    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 4);
                    const uint8_t *yPlane = data;
                    const uint8_t *uvPlane = data + static_cast<size_t>(w) * h;
                    for (UINT32 y = 0; y < h; ++y) {
                        const uint8_t *yRow = yPlane + static_cast<size_t>(y) * w;
                        const uint8_t *uvRow = uvPlane + static_cast<size_t>(y / 2) * w;
                        uint8_t *dst = rgb.data() + static_cast<size_t>(y) * w * 4;
                        for (UINT32 x = 0; x < w; ++x) {
                            const float cb = uvRow[(x & ~1u)] - 128.f;
                            const float cr = uvRow[(x & ~1u) + 1] - 128.f;
                            const float luma = yRow[x];
                            dst[0] = static_cast<uint8_t>(luma + 1.772f * cb < 0.f ? 0
                                      : luma + 1.772f * cb > 255.f ? 255 : luma + 1.772f * cb);   // B
                            dst[1] = static_cast<uint8_t>(luma - 0.344136f * cr - 0.714136f * cb < 0.f ? 0
                                      : luma - 0.344136f * cr - 0.714136f * cb > 255.f ? 255
                                      : luma - 0.344136f * cr - 0.714136f * cb);                   // G
                            dst[2] = static_cast<uint8_t>(luma + 1.402f * cr < 0.f ? 0
                                      : luma + 1.402f * cr > 255.f ? 255 : luma + 1.402f * cr);     // R
                            dst[3] = 0xFF;
                            dst += 4;
                        }
                    }
                    std::vector<uint8_t> jpeg = EncodeJpeg(rgb.data(), w, h, w * 4, 70);
                    if (!jpeg.empty()) {
                        const std::lock_guard<std::mutex> lock(publishMutex);
                        tap.latestJpeg = std::move(jpeg);
                    }
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
        sample->Release();
        Sleep(66);   // ~15 fps ceiling — a pane, not a program monitor
    }

    reader->Release();
    source->Release();
}

// The screen tap: BitBlt the monitor into a DIB section, downscale to the
// pane budget, JPEG, publish — every ~66 ms, same shape as the camera drain.
// The thread owns its own HDCs (created and released on that thread). Each
// grab re-resolves the monitor by NAME so a display-settings change or
// unplug surfaces as a missing HDC instead of garbage pixels.
//
// WINDOW branch: a "win:<hwnd>" id PrintWindows that window instead. The
// per-grab IsWindow check ends the tap cleanly when the window closes
// (the UI re-enumerates on dialog open, so stale ids self-heal).
void WindowsVideo::ScreenPreviewThread(PreviewTap &tap, std::mutex &publishMutex)
{
    const bool isWindow = tap.deviceId.rfind("win:", 0) == 0;
    HWND hwnd = nullptr;
    if (isWindow)
        hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(
            std::strtoull(tap.deviceId.c_str() + 4, nullptr, 10)));
    const std::wstring monitor = isWindow ? std::wstring() : win::Wide(tap.deviceId);
    int consecutiveFailures = 0;
    while (tap.running.load(std::memory_order_relaxed)) {
        if (isWindow && !IsWindow(hwnd)) {
            std::printf("[screenpreview] window closed: %s\n", tap.deviceId.c_str());
            std::fflush(stdout);
            break;
        }
        HDC screenDc = nullptr;
        if (!isWindow) {
            // A per-monitor DC: CreateDCW on the GDI device name captures
            // THAT display only (its HORZRES/VERTRES are the monitor's own
            // size), so multi-monitor rigs never bleed into each other and a
            // gone monitor just fails DC creation. Re-created each grab so
            // resolution changes are picked up without a tap restart.
            screenDc = CreateDCW(monitor.c_str(), nullptr, nullptr, nullptr);
        }
        // Memory DC in BOTH branches: PrintWindow wants a compatible DC, and
        // the previous GetDC(nullptr) display DC leaked one GDI object per
        // grab (it pairs with ReleaseDC, never the DeleteDC below) — 15 fps of
        // those exhausted the process GDI quota in minutes and hung the UI.
        HDC memDc = CreateCompatibleDC(screenDc);
        if (!screenDc && !memDc) {
            // Transient (display-mode change, sleeping panel) vs gone: ride out
            // a short failure streak before ending the tap — a mode change must
            // not kill a tap whose comment promises it survives one.
            if (++consecutiveFailures < 20) {
                Sleep(100);
                continue;
            }
            std::printf("[screenpreview] DC creation failed for %s\n", tap.deviceId.c_str());
            std::fflush(stdout);
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
            Sleep(66);
            continue;
        }
        // Downscale to ~640 wide — pane-sized; the dialog Image scales the
        // rest of the way and GDI+ encodes a fraction of the pixels.
        const int dstW = std::min(srcW, 640);
        const int dstH = std::max(1, srcH * dstW / srcW);
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
                // PW_RENDERFULLCONTENT (Win 8.1+) includes DWM-composited
                // content — Chromium/Qt clients render correctly — but it
                // renders the window 1:1 and CANNOT scale: pointing it at
                // the pane-sized DIB captured only the window's top-left
                // corner, magnified (the "zoomed in, not the full window"
                // report). Render at FULL client size into a transient DIB,
                // then one GDI StretchBlt down into the pane-sized one.
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
                std::vector<uint8_t> jpeg = EncodeJpeg(static_cast<const uint8_t *>(bits),
                                                       static_cast<UINT32>(dstW),
                                                       static_cast<UINT32>(dstH), stride, 70);
                if (!jpeg.empty()) {
                    const std::lock_guard<std::mutex> lock(publishMutex);
                    tap.latestJpeg = std::move(jpeg);
                    gotFrame = true;
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
            if (++consecutiveFailures >= 20)
                break;   // nothing blits for ~2 s — the source is gone, stop
            Sleep(30);   // a failed grab retries a bit slower; success loops at ~15 fps
        } else {
            consecutiveFailures = 0;
            Sleep(66);
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
        if (slot && slot->running.load())
            return Ok();   // idempotent — already running
        if (slot) {
            slot->running.store(false);
            stale = std::move(slot);
        }
        auto tap = std::make_unique<PreviewTap>();
        tap->deviceId = monitorId;
        tap->running.store(true);
        tap->worker = std::thread(&WindowsVideo::ScreenPreviewThread, std::ref(*tap),
                                  std::ref(previews_->mutex));
        slot = std::move(tap);
    }
    stale.reset();
    return Ok();
}

Result<void> WindowsVideo::StopScreenPreview(const std::string &monitorId)
{
    previews_->Erase(monitorId);
    return Ok();
}

Result<void> WindowsVideo::StartPreview(const std::string &deviceId, const std::string &mode)
{
    // Resolve the device's friendly name — WITHOUT the full Enumerate() and
    // WITHOUT trusting enumeration order. MFEnumDeviceSources IGNORES the
    // symbolic-link attribute as an enumeration filter (telemetry: 10 of 10
    // cameras returned for every "filtered" query), so the previous
    // activates[0] pick resolved EVERY id to the first enumerated camera —
    // the same-camera bug, and why the NDI refusal never fired in the test
    // run (the resolved name was always "Iriun Webcam"). Match the symlink
    // EXPLICITLY; a miss means the device is genuinely gone.
    std::string rosterName;
    {
        ScopedComInit com;
        IMFAttributes *attrs = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) && attrs) {
            attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            IMFActivate **activates = nullptr;
            UINT32 count = 0;
            if (SUCCEEDED(MFEnumDeviceSources(attrs, &activates, &count)) && count > 0 && activates) {
                const std::string want = LowerAscii(deviceId);
                for (UINT32 i = 0; i < count && rosterName.empty(); ++i) {
                    WCHAR *symW = nullptr;
                    UINT32 symLen = 0;
                    if (SUCCEEDED(activates[i]->GetAllocatedString(
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                            &symW, &symLen)) && symW) {
                        if (LowerAscii(win::Utf8(std::wstring(symW, symLen))) == want) {
                            WCHAR *nameW = nullptr;
                            UINT32 nameLen = 0;
                            if (SUCCEEDED(activates[i]->GetAllocatedString(
                                    MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &nameW, &nameLen)) && nameW) {
                                rosterName = win::Utf8(std::wstring(nameW, nameLen));
                                CoTaskMemFree(nameW);
                            }
                        }
                        CoTaskMemFree(symW);
                    }
                    activates[i]->Release();   // never ActivateObject here
                }
            }
            if (activates) CoTaskMemFree(activates);
            attrs->Release();
        }
    }
    if (rosterName.empty())
        return Error::Make(Err::NotFound, "Video", "no capture device " + deviceId);

    // NDI virtual cameras crash their own driver DLL in-process during
    // activation (before any media-type negotiation). Their feed belongs
    // to the dedicated NDI pipeline anyway — refuse BEFORE any activation
    // instead of taking the whole process down.
    std::string lowerName = rosterName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowerName.find("ndi") != std::string::npos)
        return Error::Make(Err::Unsupported, "Video",
                           "NDI sources are served by the NDI pipeline, not the camera preview tap");

    std::unique_ptr<PreviewTap> stale;
    {
        const std::lock_guard<std::mutex> lock(previews_->mutex);
        auto &slot = previews_->taps[deviceId];
        if (slot && slot->running.load())
            return Ok();   // idempotent — already running
        if (slot) {
            slot->running.store(false);
            stale = std::move(slot);
        }
        auto tap = std::make_unique<PreviewTap>();
        tap->deviceId = deviceId;
        tap->deviceName = rosterName;
        tap->mode = mode;
        tap->running.store(true);
        tap->worker = std::thread(&WindowsVideo::PreviewThread, std::ref(*tap),
                                  std::ref(previews_->mutex));
        slot = std::move(tap);
    }
    stale.reset();   // join the replaced tap with the mutex free
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
    return it->second->latestJpeg;   // copy under the lock — the worker swaps wholesale
}

std::vector<std::string> WindowsVideo::ActivePreviews() const
{
    std::vector<std::string> ids;
    const std::lock_guard<std::mutex> lock(previews_->mutex);
    for (const auto &[id, tap] : previews_->taps)
        if (tap->running.load()) ids.push_back(id);
    return ids;
}

} // namespace bps::platform
