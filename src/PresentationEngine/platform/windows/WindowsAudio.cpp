// INITGUID first: MinGW's libuuid doesn't carry the storage for
// PKEY_Device_FriendlyName (DEFINE_PROPERTYKEY is extern otherwise) — this
// instantiates the GUID in this translation unit.
#define INITGUID
#include "platform/windows/WindowsAudio.hpp"
#include "platform/windows/WinUtil.hpp"

#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <map>
#include <string>

namespace bps::platform {

namespace {

// The WinMM wave* device APIs are implemented on top of MMDevAPI (COM): a
// calling thread without initialized COM makes them fail with
// CO_E_NOTINITIALIZED (0x800401F0) — observed as floods of "MMDevAPI.DLL:
// CoInitialize has not been called" from the kernel's platform-watcher
// threads, whose failures can race the DLL's lazy per-thread init and take
// the process down. Scoped per-call guard: initializes COM for THIS call if
// the thread has none and releases it on scope exit; when the thread already
// initialized COM under a different model (RPC_E_CHANGED_MODE) or already has
// our model (S_FALSE), we leave the existing init untouched and only balance
// the reference WE successfully took.
class ScopedComInit {
public:
    ScopedComInit() { ok_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ScopedComInit() { if (ok_) CoUninitialize(); }
    ScopedComInit(const ScopedComInit&) = delete;
    ScopedComInit &operator=(const ScopedComInit&) = delete;
private:
    bool ok_ = false;
};

// True per-device channel count / mix rate, straight from WASAPI: each
// endpoint's mix format is what the OS actually configured it for — mono
// mics report 1, stereo line-ins 2, 7.1 outputs 8. WinMM's WAVEINCAPSW
// carries only a bitmask of *supported* legacy formats, which is why the
// channels field used to be hardcoded 2.
//// Matching WinMM devices to MMDevAPI endpoints: both derive their friendly
// name from the same underlying device, but WinMM truncates at 31 chars
// (szPname) while MMDevAPI's PKEY_Device_FriendlyName does not — so a
// captured prefix match (WinMM name ⊆ endpoint name, case-insensitive).
// If two endpoints share a prefix, the first claims it and later duplicates
// fall back to caps defaults (rare; virtual device farms).
std::map<std::string, AudioDeviceInfo> QueryWasapiMixFormats(bool input)
{
    std::map<std::string, AudioDeviceInfo> byName;
    ScopedComInit com;

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) return byName;

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(
        input ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice *device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) continue;

            // Friendly name via the property store.
            IPropertyStore *props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT var{};
                props->GetValue(PKEY_Device_FriendlyName, &var);
                std::string name = (var.vt == VT_LPWSTR && var.pwszVal)
                                       ? win::Utf8(std::wstring(var.pwszVal)) : std::string();
                PropVariantClear(&var);
                props->Release();

                // The mix format — the actual answer.
                if (!name.empty()) {
                    IAudioClient *client = nullptr;
                    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                                   nullptr, reinterpret_cast<void **>(&client)))
                        && client) {
                        WAVEFORMATEX *fmt = nullptr;
                        if (SUCCEEDED(client->GetMixFormat(&fmt)) && fmt) {
                            AudioDeviceInfo info;
                            info.name = name;
                            info.isInput = input;
                            info.channels = fmt->nChannels;
                            info.sampleRateHz = fmt->nSamplesPerSec;
                            byName[name] = std::move(info);
                            CoTaskMemFree(fmt);
                        }
                        client->Release();
                    }
                }
            }
            device->Release();
        }
    }
    if (collection) collection->Release();
    enumerator->Release();
    return byName;
}

// Case-insensitive prefix compare for the WinMM↔WASAPI name match.
bool NameMatches(const std::string &winmmName, const std::string &endpointName)
{
    if (winmmName.empty() || endpointName.empty()) return false;
    if (endpointName.size() < winmmName.size()) return false;
    for (size_t i = 0; i < winmmName.size(); ++i) {
        const char a = winmmName[i], b = endpointName[i];
        if (std::tolower(static_cast<unsigned char>(a))
            != std::tolower(static_cast<unsigned char>(b))) return false;
    }
    return true;
}

} // namespace

std::vector<AudioDeviceInfo> WindowsAudio::Enumerate() const {
    ScopedComInit com;
    std::vector<AudioDeviceInfo> out;

    // WASAPI mix-format truth per endpoint, name-matched against the WinMM
    // roster below (see QueryWasapiMixFormats — mono mics report 1, stereo
    // 2, 7.1 outputs 8; caps alone can't tell).
    const std::map<std::string, AudioDeviceInfo> inTruth = QueryWasapiMixFormats(true);
    const std::map<std::string, AudioDeviceInfo> outTruth = QueryWasapiMixFormats(false);

    // Output devices (winmm). dwFormats is a bitmask; 44.1k/48k are near
    // universal, so those are reported as the nominal rate.
    UINT outputs = waveOutGetNumDevs();
    for (UINT i = 0; i < outputs; ++i) {
        // Explicit *W struct, not the TCHAR-generic WAVEOUTCAPS: this file
        // isn't compiled with UNICODE defined, so the generic name resolves
        // to the ANSI (szPname: CHAR[]) variant, which doesn't match the
        // explicit waveOutGetDevCapsW() call below (LPWAVEOUTCAPSW).
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        AudioDeviceInfo d;
        d.id = "waveout:" + std::to_string(i);
        d.name = win::Utf8(caps.szPname);
        d.isInput = false;
        d.isDefault = (i == 0);
        d.sampleRateHz = 48000;   // nominal; caps are a bitmask of supported rates
        d.channels = 2;
        // WASAPI truth overrides the nominal defaults when the endpoint is
        // found (its name is a non-truncated superset of WinMM's).
        for (const auto &[name, truth] : outTruth) {
            if (NameMatches(d.name, name)) {
                d.channels = truth.channels;
                d.sampleRateHz = truth.sampleRateHz;
                break;
            }
        }
        out.push_back(std::move(d));
    }

    // Input devices.
    UINT inputs = waveInGetNumDevs();
    for (UINT i = 0; i < inputs; ++i) {
        WAVEINCAPSW caps{};   // see the WAVEOUTCAPSW note above
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        AudioDeviceInfo d;
        d.id = "wavein:" + std::to_string(i);
        d.name = win::Utf8(caps.szPname);
        d.isInput = true;
        d.isDefault = (i == 0);
        d.sampleRateHz = 48000;
        d.channels = 2;
        // WASAPI truth overrides the nominal defaults when the endpoint is
        // found (see the output branch above).
        for (const auto &[name, truth] : inTruth) {
            if (NameMatches(d.name, name)) {
                d.channels = truth.channels;
                d.sampleRateHz = truth.sampleRateHz;
                break;
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

Result<AudioDeviceInfo> WindowsAudio::DefaultOutput() const {
    auto devs = Enumerate();
    for (const auto& d : devs)
        if (!d.isInput && d.isDefault) return d;
    for (const auto& d : devs)
        if (!d.isInput) return d;
    return Error::Make(Err::NotFound, "Audio", "no output devices found");
}

Result<AudioDeviceInfo> WindowsAudio::DefaultInput() const {
    auto devs = Enumerate();
    for (const auto& d : devs)
        if (d.isInput && d.isDefault) return d;
    for (const auto& d : devs)
        if (d.isInput) return d;
    return Error::Make(Err::NotFound, "Audio", "no input devices found");
}

std::string WindowsAudio::Fingerprint() const {
    std::string fp;
    for (const auto& d : Enumerate())
        fp += d.id + (d.isInput ? ":in;" : ":out;");
    return fp;
}

} // namespace bps::platform
