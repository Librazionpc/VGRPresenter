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
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <gdiplus.h>
#include <objidl.h>   // IStream for the JPEG encoder
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
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
    std::vector<uint8_t> latestJpeg;   // written by the thread under the table's mutex
    std::thread worker;        // joined by ~PreviewTap (same discipline as MeterTap)
    ~PreviewTap() { if (worker.joinable()) worker.join(); }
};

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
std::vector<uint8_t> EncodeJpeg(const uint8_t *bgra, UINT32 w, UINT32 h, UINT32 stride, UINT32 quality)
{
    using GdiplusBoot = ULONG (__stdcall *)(ULONG);
    static HMODULE gdipModule = nullptr;
    static GdiplusBoot gdipStartup = nullptr;
    static bool gdipTried = false;
    if (!gdipTried) {
        gdipTried = true;
        gdipModule = LoadLibraryW(L"gdiplus.dll");
        if (gdipModule)
            gdipStartup = reinterpret_cast<GdiplusBoot>(
                reinterpret_cast<void *>(GetProcAddress(gdipModule, "GdiplusStartup")));
    }
    if (!gdipStartup)
        return {};

    // GDI+ must stay initialized for the whole encode; scope the token.
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    if (gdipStartup(&token, &input, nullptr) != Gdiplus::Ok)
        return {};

    std::vector<uint8_t> out;
    {
        // PixelFormat32bppRGB is the BGRA byte order the reader hands us
        // (GDI+'s "RGB32" is byte-wise B,G,R,X — no channel swap needed).
        Gdiplus::Bitmap bmp(static_cast<INT>(w), static_cast<INT>(h),
                            static_cast<INT>(stride), PixelFormat32bppRGB, bgra);
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
            params.Parameter[0].Value = &q;
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
    Gdiplus::GdiplusShutdown(token);
    return out;
}

// The drain thread: open the device in a Source Reader, request RGB32 at
// the native resolution, loop ReadSample → JPEG → publish. Exits when the
// tap's running flag drops, the device vanishes (MF_E_NO_MORE_TYPES on a
// hot-unplug surfaces as a read failure), or the encoder never engages.
void WindowsVideo::PreviewThread(PreviewTap &tap, std::mutex &publishMutex)
{
    ScopedComInit com;

    IMFAttributes *attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 1)) || !attrs)
        return;
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    // Match by symbolic link — the same stable id Enumerate() reports.
    const std::wstring symLink = win::Wide(tap.deviceId);
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                     symLink.c_str());
    IMFActivate *activate = nullptr;
    if (FAILED(MFEnumDeviceSources(attrs, &activate, nullptr)) || !activate) {
        attrs->Release();
        return;
    }
    attrs->Release();

    IMFMediaSource *source = nullptr;
    if (FAILED(activate->ActivateObject(IID_IMFMediaSource,
                                        reinterpret_cast<void **>(&source))) || !source) {
        activate->Release();
        return;
    }
    activate->Release();

    IMFSourceReader *reader = nullptr;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source, nullptr, &reader)) || !reader) {
        source->Release();
        return;
    }

    // Native type first (the reader demands a type before reading), then
    // steer the converter to RGB32 — sensors hand us NV12/YUY2, the reader
    // converts. Full native resolution; the JPEG size is fine for a pane.
    IMFMediaType *native = nullptr;
    if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
        native = nullptr;
    }
    IMFMediaType *rgb = nullptr;
    bool typeSet = false;
    if (SUCCEEDED(MFCreateMediaType(&rgb)) && rgb) {
        rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        typeSet = SUCCEEDED(reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb));
        rgb->Release();
    }
    if (native) native->Release();
    if (!typeSet) {
        reader->Release();
        source->Release();
        return;
    }

    while (tap.running.load(std::memory_order_relaxed)) {
        DWORD streamFlags = 0;
        IMFSample *sample = nullptr;
        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                        nullptr, &streamFlags, nullptr, &sample);
        if (FAILED(hr))
            break;   // device gone / stream error — tap dies, UI keeps glyph
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
                // Frame geometry from the CURRENT type (post-conversion).
                IMFMediaType *cur = nullptr;
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(reader->GetCurrentMediaType(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) && cur) {
                    UINT64 frameSize = 0;
                    if (SUCCEEDED(cur->GetUINT64(MF_MT_FRAME_SIZE, &frameSize))) {
                        w = static_cast<UINT32>(frameSize >> 32);
                        h = static_cast<UINT32>(frameSize & 0xFFFFFFFF);
                    }
                    cur->Release();
                }
                if (w > 0 && h > 0) {
                    const UINT32 stride = w * 4;   // contiguous RGB32
                    std::vector<uint8_t> jpeg = EncodeJpeg(data, w, h, stride, 70);
                    if (!jpeg.empty()) {
                        const std::lock_guard<std::mutex> lock(publishMutex);
                        tap.latestJpeg = std::move(jpeg);
                    }
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

} // namespace

Result<void> WindowsVideo::StartPreview(const std::string &deviceId)
{
    // Validate against the roster: a bogus id must fail with NotFound rather
    // than spawn a thread that dies of a missing device.
    bool known = false;
    for (const auto &d : Enumerate())
        if (d.id == deviceId) { known = true; break; }
    if (!known)
        return Error::Make(Err::NotFound, "Video", "no capture device " + deviceId);

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
