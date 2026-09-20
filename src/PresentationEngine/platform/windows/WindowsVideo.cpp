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
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

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

} // namespace bps::platform
