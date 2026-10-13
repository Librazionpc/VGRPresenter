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
#include <chrono>
#include <cmath>
#include <condition_variable>
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

// Is this camera one of the names a targeted walk was asked for? Exact first
// (the roster came from this same walk), then case-insensitive with
// surrounding whitespace ignored — the wanted set is a SAVED roster, and a
// label differing only in case or padding must not read as "camera not
// connected" for a camera that is plugged in.
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
        // The targeted walk's whole point: a camera that was not asked for is
        // dropped here, before ActivateObject + the media-type walk below (and
        // it is not listed either — the caller asked for its own devices only).
        if (!NamedBy(only, info.name))
            continue;
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
    // TOOLWINDOW: palettes/tooltips sized 0×0 or worse, no taskbar presence —
    // capture noise either way.
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
    // CLOAKED — the subtle one. The shell cloaks windows it keeps alive but
    // off-screen: OTHER VIRTUAL DESKTOPS (DWM_CLOAKED_SHELL == 2 — the exact
    // state of the workspace window in the repeat "no display named 'live url
    // product sync gate'" report: probe showed visible=True, toolwin=False,
    // cloaked=2 while the window sat on another virtual desktop) and
    // suspended UWP apps (DWM_CLOAKED_APP, also skipped here — truly ghost
    // pixels). PrintWindow(PW_RENDERFULLCONTENT) still captures a SHELL-cloaked
    // window's real, current content, so those stay IN the roster — they are
    // precisely the "window I left on desktop 2" the user picks as a screen
    // source. Only fully-invisible app-level cloaks are filtered.
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
// One tap's stop flag, frame slot and read handshake. SHARED (shared_ptr) on
// purpose: a camera tap's Media Foundation callback runs on an MF thread and
// may still be in flight — or arrive late — while the tap is torn down, so the
// callback owns live memory of its own and never touches the tap, the table,
// or their mutexes.
struct VideoTapShared {
    std::atomic<bool> running{false};   // the stop flag the table's Erase/Clear sets
    std::mutex mutex;                   // guards latestJpeg + the read handshake below
    std::vector<uint8_t> latestJpeg;    // swapped wholesale, copied out under `mutex`

    void Publish(std::vector<uint8_t> jpeg)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        latestJpeg = std::move(jpeg);
    }
    std::vector<uint8_t> Frame()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return latestJpeg;
    }

    // ---- the ASYNC camera read: the MF callback <-> this tap's worker ----
    std::condition_variable cv;        // stop / flush / stream-death wakeups
    std::atomic<IMFSourceReader *> reader{nullptr};   // borrowed; owned by the worker
    bool flushDone = false;            // OnFlush arrived: nothing is pending any more
    bool stopping = false;             // the worker released the reader — callbacks no-op
    // The tap is dead: the stream died (unplug, driver error) OR the worker's
    // OPEN failed (activation / reader / media type / no decodable type — those
    // used to print to stdout and return, leaving this false, so a tap whose
    // device never opened looked exactly like one warming up: the UI pulled an
    // empty frame forever and showed its connecting ring with no reason
    // anywhere, and StartPreview's idempotency check saw `running` still set and
    // refused to retry the device for the rest of the session).
    // Atomic because StartPreview reads it under the TABLE's mutex while the
    // worker writes it under this state's own.
    std::atomic<bool> failed{false};
    HRESULT failure = S_OK;
    std::string failureText;           // WHY, in the platform's own words (guarded by `mutex`)
    int64_t lastEncodeMs = 0;          // the ~15 fps ceiling the sync loop got from Sleep(66)

    // The ONE way a tap reports it can never produce a frame. Both workers (the
    // camera drain and the screen/window grab) call this instead of breaking or
    // returning silently, so `failed` means the same thing everywhere and the
    // reason survives for the UI to ask for. `running` is cleared because a
    // failed tap is not a running one: StartPreview's idempotency check would
    // otherwise keep returning Ok for the rest of the session and never retry
    // the device.
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
    // NOT const: reading the reason takes the state's own mutex, and the guard
    // does not own the lock — the same two-level hand-off Frame() uses. (A const
    // reader would need a mutable mutex, which would let this one method's
    // requirement leak into every other use of the member.)
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

// A pane, not a program monitor: one ENCODED frame every ~66 ms (~15 fps) —
// the same ceiling the synchronous loop's Sleep(66) imposed, now a throttle on
// the encode instead of a sleep between reads. The reader therefore always
// holds the newest frame the device delivered while the CPU cost stays put.
constexpr int64_t kPreviewFrameMs = 66;
// How long the worker waits for OnFlush after cancelling the pending read. The
// flush IS the interruptibility: past this backstop the device is ignoring the
// cancellation, and the tap is abandoned (see the table's Clear) instead of
// being left to block teardown forever.
constexpr auto kFlushWait = std::chrono::milliseconds(2000);

int64_t SteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Stop-aware pause: every wait inside a capture worker is interruptible by
// `running == false`, so a stop is honored inside one tick instead of after a
// full Sleep. Returns false when the tap was stopped while waiting. (This is
// the companion of the cancellable capture below: a worker that can leave its
// own waits promptly is what makes an unbounded teardown wait unnecessary.)
bool WaitOrStop(const std::shared_ptr<VideoTapShared> &shared, int ms)
{
    std::unique_lock<std::mutex> lock(shared->mutex);
    if (!shared->running.load())
        return false;
    shared->cv.wait_for(lock, std::chrono::milliseconds(ms));
    return shared->running.load();
}

// THE cancellation a GDI window capture has and PrintWindow does not.
//
// PrintWindow is a synchronous send of WM_PRINT: with no timeout it waits on
// the target's UI thread forever, which is the unbounded call that used to
// wedge teardown (and the reason a wedged tap had to be abandoned).
// SendMessageTimeout with SMTO_ABORTIFHUNG|SMTO_BLOCK is the one primitive GDI
// DOES bound: it gives up after the timeout and reports failure. Probing with
// it before every window grab means PrintWindow is only ever entered for a
// window whose message queue answered within the bound — so the worker always
// returns and `running == false` is always honored. (WM_NULL is the canonical
// "are you answering?" probe and does not disturb the target.)
constexpr UINT kWindowProbeMs = 200;
bool WindowIsAnswering(HWND hwnd)
{
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                               kWindowProbeMs, &result) != 0;
}

namespace {
// Declared ahead: the async frame store below encodes JPEGs, and the encoder
// (with the shared GDI+ boot) is defined further down in THIS unnamed namespace
// — every unnamed-namespace block in a translation unit is the same namespace,
// so this declares that very function. A declaration at bps::platform scope
// instead would be a second, different overload.
std::vector<uint8_t> EncodeJpeg(const uint8_t *bgra, UINT32 w, UINT32 h, UINT32 stride, UINT32 quality);
} // namespace

// RGB32 / YUY2 / NV12 -> JPEG — the conversions the synchronous loop used,
// verbatim. CALLER HOLDS `shared.mutex` and passes a sample it is inside: the
// async contract owns the sample only for the callback's own call, so the
// frame is consumed and published before that call returns.
void StoreSampleFrame(VideoTapShared &shared, IMFSample *sample)
{
    IMFSourceReader *reader = shared.reader.load();
    IMFMediaBuffer *buf = nullptr;
    if (!reader || !sample || FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf)
        return;
    BYTE *data = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)) && data) {
        // Frame geometry + subtype from the CURRENT type (post-conversion) —
        // BOTH read before the release (an earlier shape released first and
        // read the subtype from the dangling pointer: a use-after-free that
        // took the test binary down mid-pal). Then a size cross-check against
        // the buffer the reader actually delivered — never read past a short
        // buffer.
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
            if (!jpeg.empty()) shared.latestJpeg = std::move(jpeg);
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
            if (!jpeg.empty()) shared.latestJpeg = std::move(jpeg);
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
            if (!jpeg.empty()) shared.latestJpeg = std::move(jpeg);
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

// The Source Reader callback. ASYNC mode is what makes a read INTERRUPTIBLE:
// a synchronous ReadSample can sit inside the camera driver forever (the
// observed MFPlat!MFCreateMediaEvent hang), whereas Flush cancels a pending
// async request and reports completion through OnFlush — so the tap can always
// be shut down instead of abandoned (see the table's Clear and PreviewThread).
//
// Every method is called from MF's own worker threads, and the object is held
// by the reader for as long as the reader lives, so its shared state — never
// the tap — is the only thing it touches.
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
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        // The worker is past this callback: the reader it owned is released, so
        // there is nothing to do and — critically — nothing to touch.
        if (shared_->stopping) return S_OK;
        const bool stopping = !shared_->running.load();                if (FAILED(hr)) {
                    if (!stopping) {   // a cancelled read is not a dead stream
                        shared_->failed.store(true);
                        shared_->failure = hr;
                        shared_->cv.notify_all();
                    }
                    return S_OK;
                }
        if (sample && !(flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            const int64_t now = SteadyMs();
            if (now - shared_->lastEncodeMs >= kPreviewFrameMs) {
                shared_->lastEncodeMs = now;
                StoreSampleFrame(*shared_, sample);
            }
        }
        if (stopping) {
            shared_->cv.notify_all();   // the worker is waiting to shut this down
            return S_OK;
        }
        IMFSourceReader *reader = shared_->reader.load();
        if (reader)   // the next sample; this is the whole read loop
            reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                               nullptr, nullptr, nullptr, nullptr);
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

// Screen taps key by the monitor id ("\\\.\DISPLAY1" from IMonitor's
// Enumerate) in the SAME table as camera taps — the id namespaces cannot
// collide ("\\\.\DISPLAY" vs camera symlinks), and PreviewFrame/Stop stay
// single-surface for the bridge.

struct WindowsVideo::PreviewTable {
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<PreviewTap>> taps;

    // ---- Reaping: HOW A TAP DIES WITHOUT BLOCKING ITS CALLER -------------
    //
    // A PreviewTap joins its capture worker in its destructor, and that join
    // waits for whatever the worker is inside. Every tap is now CANCELLABLE,
    // so that wait is bounded by one worker iteration: a camera read is
    // cancelled by Flush (see PreviewThread / the ASYNC note on
    // SourceReaderNotify), and a screen/window grab is gated on a bounded
    // probe before any PrintWindow (see WindowIsAnswering) with stop-aware
    // waits between grabs. Reaping still happens off the caller's thread — the
    // join is short now, but the callers of the stop path include the GUI
    // thread and must never wait on capture work at all.
    //
    // The callers of the stop path include the GUI thread:
    // EngineBridge::stopAllVideoPreviews() runs from the Audio & Video screen's
    // Component.onDestruction — i.e. the instant the operator clicks another
    // Settings page — and stopScreenPreview/stopVideoPreview run from the
    // source-edit dialog when a tap is re-pointed. Joining inline there is the
    // observed "clicking Plugins with a LIVE screen source closes the app"
    // crash: the GUI thread stopped pumping and Windows answered with an
    // AppHangB1 kill (the crash record is empty because a hang kill is not an
    // exception: only the CallTrace watchdog saw it, 5 s in, with nothing
    // instrumented in flight).
    //
    // So a dying tap is HANDED TO A REAPER THREAD that owns it, joins it and
    // dies. Erase() never waits; the tap's own destructor still does the join,
    // just not on the thread the operator is waiting on.
    void Reap(std::unique_ptr<PreviewTap> tap)
    {
        if (!tap)
            return;
        {
            // Counted BEFORE the thread starts, and cleared by the reaper
            // itself: Clear() (teardown) waits on this, so no capture worker can
            // outlive the table it publishes into. NOT static — the counter is
            // per table.
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
            // The stop flag lives in the SHARED state (the camera callback reads
            // it off an MF thread), and the notify wakes the worker out of its
            // stop wait instead of leaving it to its polling tick.
            it->second->shared->running.store(false);
            it->second->shared->cv.notify_all();
            tap = std::move(it->second);
            taps.erase(it);
        }
        Reap(std::move(tap));   // join OFF this thread — see Reap
    }

    // The TEARDOWN path (the PAL's own destruction, and nothing else — EngineBridge's
    // shutdown/`Manager().Clear()` never reaches this table). It signals every
    // tap, reaps them, then waits for every capture worker to be gone before the
    // table is destroyed: a worker left publishing into `mutex` after the table
    // dies would be use-after-free, which is the guarantee the inline join
    // provided and the reason this cannot simply detach and forget.
    //
    // THE WAIT IS FULL, and deliberately so. It used to be bounded with a
    // leak-the-table fallback on timeout, because a screen/window tap could be
    // parked in PrintWindow against a wedged window with no way out. Both
    // halves of that are gone: the tap's own waits are
    // stop-aware and its window capture is gated on a bounded probe, and a
    // camera tap's read is cancelled by Flush — so a worker always observes
    // `running == false` and returns, and waiting for it is what makes
    // destroying the table safe. (The old unbounded wait that froze shutdown
    // was waiting on MF's synchronous read; that call no longer exists.)
    // A slow worker is REPORTED rather than silently leaked, so a genuine
    // regression shows up in the log instead of as a mystery leak.
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

    // The ONLY owner (WindowsVideo::previews_) calls Clear() first, which is the
    // whole teardown contract now that every worker is cancellable: no table can
    // outlive the PAL that owns it.
    ~PreviewTable() = default;

private:
    // How often teardown reports a worker that is slow to exit. Healthy taps
    // exit in milliseconds (they see running == false between reads and inside
    // every stop-aware wait); this is a diagnostic interval, not a give-up
    // point, because there is nothing left to give up on — see Clear().
    static constexpr std::chrono::milliseconds kReapReport{2000};

    std::mutex reapMutex_;
    std::condition_variable reapCv_;
    int reaping_ = 0;
};


WindowsVideo::WindowsVideo() : previews_(std::make_shared<PreviewTable>()) {}

WindowsVideo::~WindowsVideo()
{
    if (!previews_)
        return;
    // Clear() cancels and reaps every tap, then waits for the workers to exit —
    // a full wait, because every capture worker is now cancellable (camera:
    // Flush; screen/window: bounded probe + stop-aware waits). There is no
    // keep-alive list any more: nothing can be left parked in an unbounded
    // driver call, so the table is always safely destroyable here.
    previews_->Clear();
    previews_.reset();
}

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
// GDI+ boot-once-per-process, shared by EncodeJpeg and DecodeJpeg below
// (each DRAIN THREAD calls this before its first encode; SceneBuilder's
// worker thread calls it before its first decode — GDI+ is documented
// multi-thread-safe once started, so one shared token serves every caller).
//
// START ONCE PER PROCESS, NEVER SHUTDOWN: an earlier per-call
// Startup/Shutdown cycle crashed the process seconds into a live stream
// (GDI+ keeps background worker threads that outlive each cycle). One
// deliberate init kept for the process lifetime is the standard pattern
// for long-lived GDI+ hosts.
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

// Decode JPEG bytes into tightly-packed top-down RGBA8 via GDI+ — the
// inverse of EncodeJpeg, same shared boot-once. Empty on any failure (a
// corrupt/partial frame, or GDI+ unavailable) — the caller just skips that
// render rather than compositing garbage.
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
                        // GDI+'s ARGB32 is byte order B,G,R,A — the engine's
                        // RGBA8 convention (SceneBuilder's own Picture->
                        // RgbaImage conversion, LoadImageCached above) wants
                        // R,G,B,A.
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

// (A member-function definition must live OUTSIDE the anonymous namespace
// above — GCC rejects it inside. EncodeJpeg stays hidden there; the tap
// thread below is a WindowsVideo member and stays visible.)

// The drain thread: open the device in a Source Reader, request RGB32 at
// the UI's picked frame size when one was given (falls back to the native
// default), loop ReadSample → JPEG → publish. Exits when the tap's running
// flag drops, the device vanishes (MF_E_NO_MORE_TYPES on a hot-unplug
// surfaces as a read failure), or the encoder never engages.
void WindowsVideo::PreviewThread(PreviewTap &tap)
{
    ScopedComInit com;

    // EVERY WAY THIS WORKER CAN DIE SAYS SO. `hr` is S_OK for the failures that
    // are not HRESULTs ("no decodable media type"); the point is not the code but
    // the FLAG: VideoTapShared::Fail is how the UI learns that a preview it
    // started will never produce a frame, and the stdout line stays as the PAL's
    // own telemetry.
    const auto fail = [&tap](HRESULT hr, const char *why) {
        tap.shared->Fail(hr, why);
        std::printf("[videopreview] %s for %s (0x%08lX)\n", why, tap.deviceId.c_str(),
                    static_cast<unsigned long>(hr));
        std::fflush(stdout);
    };

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
    // The callback: one reference is ours (released on every exit below), the
    // reader's attribute store AddRefs it a second time and, if creation fails,
    // releases that one again — so exactly one Release at the end balances it
    // either way.
    SourceReaderNotify *notify = new SourceReaderNotify(tap.shared);
    // Advanced video processing ON: several camera stacks (the integrated
    // webcam among them) have no native RGB32 type and their built-in
    // converter refused a plain subtype-override, so the reader must be
    // allowed to insert its own video processor (deinterlace/colorspace
    // MFT) for the NV12→RGB32 hop. Without it the frames arrive NV12 and
    // every one fails the RGB32 buffer check ("frame skipped" spam) — the
    // "integrated cam shows nothing" defect.
    //
    // And MF_SOURCE_READER_ASYNC_CALLBACK, which is what makes the tap
    // interruptible: in async mode ReadSample returns at once and OnReadSample
    // reports completion, so Flush can cancel a read the driver has stopped
    // answering. A SYNCHRONOUS reader must never be created here — its
    // ReadSample is precisely the call that cannot be cancelled (see
    // SourceReaderNotify). Once created, a reader's mode cannot be switched.
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
        // Attr path failed outright — retry with the async callback alone; the
        // YUY2 fallback below still covers the native formats this device
        // delivers.
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
        // H264/HEVC-only virtual cam (NDI Webcam et al.) — the same honest
        // failure the other early exits now report (this one is not an HRESULT).
        fail(S_OK, "no decodable media type (H264-only virtual cam?)");
        reader->Release();
        notify->Release();
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
        // its glyph; the honest "no frames" signal), plus the flag that lets the
        // bridge SAY so instead of leaving a ring spinning forever.
        fail(S_OK, "media type selection failed");
        reader->Release();
        notify->Release();
        source->Release();
        return;
    }

    // ---- the read chain: ASYNC, and therefore INTERRUPTIBLE ----------------
    // ReadSample returns immediately and OnReadSample drives the chain from
    // MF's own thread (see SourceReaderNotify); the frame conversions run there,
    // inside the callback that owns its sample for exactly one call. Nothing on
    // THIS thread blocks inside the driver any more, which is the whole point: a
    // synchronous ReadSample sat in MFPlat!MFCreateMediaEvent indefinitely when
    // a device stopped answering, and the only way out was to abandon the
    // thread.
    tap.shared->reader.store(reader);
    {
        const std::lock_guard<std::mutex> lock(tap.shared->mutex);
        if (tap.shared->running.load())
            reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                               nullptr, nullptr, nullptr, nullptr);
    }

    // Wait for the stop flag (the table sets it on Erase, Clear and re-point) or
    // for the stream to die. The tick is a backstop for a missed wakeup, not the
    // mechanism — the table and the callback both notify.
    {
        std::unique_lock<std::mutex> lock(tap.shared->mutex);
        while (tap.shared->running.load() && !tap.shared->failed)
            tap.shared->cv.wait_for(lock, std::chrono::milliseconds(250));
    }
    // ---- deterministic shutdown -------------------------------------------
    // Cancel the request that is still out there, wait for OnFlush to report
    // the cancellation, THEN release the reader with every callback already
    // retired. `stopping` is what makes a late OnReadSample harmless: one may
    // still be delivered (MF does not promise the callback order around a
    // flush), but it finds the flag set and touches neither the reader nor
    // anything else. That is the difference between this and the old bounded
    // wait: the tap is shut down, not abandoned.
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

// The screen tap: BitBlt the monitor into a DIB section, downscale to the
// pane budget, JPEG, publish — every ~66 ms, same shape as the camera drain.
// The thread owns its own HDCs (created and released on that thread). Each
// grab re-resolves the monitor by NAME so a display-settings change or
// unplug surfaces as a missing HDC instead of garbage pixels.
//
// WINDOW branch: a "win:<hwnd>" id PrintWindows that window instead. The
// per-grab IsWindow check ends the tap cleanly when the window closes
// (the UI re-enumerates on dialog open, so stale ids self-heal).
void WindowsVideo::ScreenPreviewThread(PreviewTap &tap)
{
    const bool isWindow = tap.deviceId.rfind("win:", 0) == 0;
    // The screen tap's reason, in the same shape as the camera tap's: a window
    // that closed, a monitor that vanished and a source that stopped blitting
    // are all "this preview can never produce another frame", so the UI can say
    // so (and a later StartScreenPreview may retry the source) instead of
    // showing a connecting ring over a dead tap.
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
        // A minimized window isn't composited by DWM at all — PrintWindow
        // (even with PW_RENDERFULLCONTENT) doesn't fail, it just returns
        // TRUE with a black/blank bitmap, which used to publish silently as
        // a real frame (a live capture that's actually just solid black,
        // indistinguishable from a genuine capture bug). Wait it out
        // OUTSIDE the consecutiveFailures/give-up accounting below — a
        // minimized source can stay that way for minutes and come back;
        // treating it as a "failed grab" would kill the tap after ~20
        // strikes (under a second), same mistake the give-up threshold
        // exists to avoid for a real transient failure.
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
        // A window that is not answering its message queue is HELD, not
        // captured: PrintWindow would block until it answers — the unbounded
        // call this tap can no longer make — while the probe itself is bounded
        // (see WindowIsAnswering). Like the minimized branch above, this is not
        // counted as a failed grab: a heavy page can stop answering for seconds
        // and come back, and a window that stays wedged ends the tap through
        // the same IsWindow/give-up paths instead of being captured mid-wedge.
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
        // Downscale to ~960 wide (was 640) — a text-heavy capture (a chat/
        // meeting window, small UI text) turned to mush after 640px + a
        // quality-70 JPEG: fine text needs more source pixels before
        // compression, not just a higher quality setting, or the encoder
        // is starting from data that's already lost the detail.
        const int dstW = std::min(srcW, 960);
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
                // Quality 85 (was 70) — quality-70 JPEG blocking on fine UI
                // text is what "rumbled up" chat text actually was.
                std::vector<uint8_t> jpeg = EncodeJpeg(static_cast<const uint8_t *>(bits),
                                                       static_cast<UINT32>(dstW),
                                                       static_cast<UINT32>(dstH), stride, 85);
                if (!jpeg.empty()) {
                    tap.shared->Publish(std::move(jpeg));
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
            if (++consecutiveFailures >= 20) {
                giveUp("no frame could be grabbed");
                break;   // nothing blits for ~2 s — the source is gone, stop
            }
            WaitOrStop(tap.shared, 30);   // a failed grab retries a bit slower; success loops at ~15 fps
        } else {
            consecutiveFailures = 0;
            WaitOrStop(tap.shared, 66);
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
        // Idempotent for a tap that is actually WORKING; a tap whose worker died
        // (see PreviewThread's `fail`) is replaced, so a monitor previewed after
        // a failure starts again instead of returning Ok for a dead tap.
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
    }
    // Re-pointed source: the replaced tap leaves through the REAPER, not an
    // inline join — its capture worker may be parked in PrintWindow right now,
    // and this call comes from the UI (the source-edit dialog).
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
    // EngineBridge supplies the friendly name from its cached roster. This
    // call only records the request and starts the capture worker; all Media
    // Foundation discovery and device opening stays off the GUI thread.

    // NDI virtual cameras crash their own driver DLL in-process during
    // activation (before any media-type negotiation). Their feed belongs
    // to the dedicated NDI pipeline anyway — refuse BEFORE any activation
    // instead of taking the whole process down.
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
        // Idempotent for a tap that is actually WORKING; a tap whose open failed
        // is replaced here, so re-picking a camera that was unplugged or busy
        // opens it again instead of returning Ok for a tap that can never
        // produce a frame (see PreviewThread's `fail`).
        if (slot && slot->shared->running.load() && !slot->shared->failed.load())
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
    }
    // Replaced tap: reaped, not joined in place — same reason as
    // StartScreenPreview's identical hand-off (a camera tap can be inside a
    // Media Foundation read for seconds).
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
    // The frame slot is the tap's SHARED state (the camera's frame is published
    // by an MF callback): the copy is under that state's own mutex, so it stays
    // correct even while the table itself is being torn down.
    return it->second->shared->Frame();
}

IVideo::DecodedFrame WindowsVideo::PreviewFramePixels(const std::string &deviceId)
{
    std::vector<uint8_t> jpeg;
    {
        const std::lock_guard<std::mutex> lock(previews_->mutex);
        auto it = previews_->taps.find(deviceId);
        if (it == previews_->taps.end())
            return {};
        jpeg = it->second->shared->Frame();   // copy under the slot's lock, decode outside it
    }
    if (jpeg.empty())
        return {};
    RawRgba raw = DecodeJpeg(jpeg.data(), jpeg.size());
    IVideo::DecodedFrame out;
    out.width = raw.width;
    out.height = raw.height;
    out.rgba = std::move(raw.rgba);
    return out;
}

std::string WindowsVideo::PreviewFailure(const std::string &deviceId) const
{
    const std::lock_guard<std::mutex> lock(previews_->mutex);
    auto it = previews_->taps.find(deviceId);
    if (it == previews_->taps.end())
        return {};
    // Only a DEAD tap has a reason to give: a tap that is merely warming up (or
    // working) must keep an empty answer, or the UI would cry wolf on every
    // start. The reason text is copied out under the state's own mutex (the
    // same two-level hand-off as Frame()).
    if (!it->second->shared->failed.load())
        return {};
    return it->second->shared->Failure();
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
