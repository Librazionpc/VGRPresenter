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

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>

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

// Friendly name of an MMDevAPI endpoint (property store read — the same
// lookup QueryWasapiMixFormats performs; shared so the meter thread matches
// endpoints by the exact convention the roster was built with).
std::string EndpointName(IMMDevice *device)
{
    IPropertyStore *props = nullptr;
    std::string name;
    if (device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT var{};
        props->GetValue(PKEY_Device_FriendlyName, &var);
        if (var.vt == VT_LPWSTR && var.pwszVal)
            name = win::Utf8(std::wstring(var.pwszVal));
        PropVariantClear(&var);
        props->Release();
    }
    return name;
}

} // namespace

// ============================================================================
// Input metering — a REAL WASAPI capture tap per metered device. The GUI
// thread only ever touches the lock-protected snapshot struct; each tap runs
// its own capture thread that owns every COM object it touches (MMDeviceAPI
// objects are apartment-bound and must not cross threads).
//
// Analysis runs on ~50 ms windows: the tap accumulates per-channel running
// peak (max |sample|) and sum-of-squares, then publishes one InputMeterLevels
// snapshot per window under the shared metersMutex_. 16-bit PCM converts
// with the symmetric 1/32768 scaling; float capture formats read directly.
// ============================================================================
struct WindowsAudio::MeterTap {
    std::atomic<bool> running{false};
    uint32_t deviceId = 0;
    std::string deviceName;   // roster name — the reliable endpoint match key
    bool loopback = false;    // false = input capture, true = render loopback
    IAudio::InputMeterLevels latest;   // written by the capture thread under the table's mutex
    // Owned + joined by the tap itself: erasing the tap (Stop, destructor)
    // blocks until the capture thread has fully exited, so no reference into
    // this storage — or into the table's mutex, which the thread takes to
    // publish — can outlive the objects it points at.
    std::thread worker;
    ~MeterTap() { if (worker.joinable()) worker.join(); }
};

// The pimpl: all metering state. unique_ptr<incomplete> in the header is
// safe because EVERY destruction of this object happens inside this .cpp
// (WindowsAudio's out-of-line destructor; the default ctor's cleanup only
// ever sees a null pointer, which never calls the deleter).
struct WindowsAudio::MeterTable {
    std::mutex mutex;
    std::map<uint32_t, std::unique_ptr<MeterTap>> taps;

    // Erase-with-join, the one removal path: signal the tap, move it out,
    // destroy it with the mutex FREE (the worker takes this mutex to publish
    // its snapshots — joining while holding it would deadlock against
    // exactly that publish).
    void Erase(uint32_t deviceId)
    {
        std::unique_ptr<MeterTap> tap;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            auto it = taps.find(deviceId);
            if (it == taps.end())
                return;
            it->second->running.store(false);
            tap = std::move(it->second);
            taps.erase(it);
        }
        tap.reset();   // ~MeterTap joins the worker here
    }

    // Stop every live tap (shutdown path) — same outside-the-lock join.
    void Clear()
    {
        std::map<uint32_t, std::unique_ptr<MeterTap>> all;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            for (auto &[id, tap] : taps)
                tap->running.store(false);
            all.swap(taps);
        }
        all.clear();   // joins, mutex free
    }

    ~MeterTable() { Clear(); }
};

WindowsAudio::WindowsAudio() : meters_(std::make_unique<MeterTable>()) {}
// ~MeterTable joins every live tap before the members die.
WindowsAudio::~WindowsAudio() = default;

void WindowsAudio::MeterThread(MeterTap &tap, std::mutex &publishMutex, bool loopback)
{
    // This thread's own COM apartment + enumerator: MMDeviceAPI objects are
    // not marshalled across apartments, so nothing here is shared.
    ScopedComInit com;
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) return;

    // wavein:<n> → the eCapture endpoint; waveout:<n> LOOPBACK → the
    // eRender endpoint captured with AUDCLNT_STREAMFLAGS_LOOPBACK (the
    // program mix). PRIMARY match: the device's friendly name (WinMM names
    // are truncated prefixes of the endpoint's — the exact correlation
    // QueryWasapiMixFormats establishes; raw indexes disagree whenever a
    // legacy WinMM device has no ACTIVE MMDevAPI endpoint). Fallback:
    // positional, for name-less exotic devices.
    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(loopback ? eRender : eCapture,
                                        DEVICE_STATE_ACTIVE, &collection);
    IMMDevice *device = nullptr;
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        if (!tap.deviceName.empty()) {
            for (UINT i = 0; i < count && !device; ++i) {
                IMMDevice *candidate = nullptr;
                if (FAILED(collection->Item(i, &candidate)) || !candidate)
                    continue;
                if (NameMatches(tap.deviceName, EndpointName(candidate)))
                    device = candidate;   // ownership passes out of the loop
                else
                    candidate->Release();
            }
        }
        if (!device && tap.deviceId < count)
            collection->Item(static_cast<UINT>(tap.deviceId), &device);
    }
    if (collection) collection->Release();
    if (!device) {
        enumerator->Release();
        return;
    }

    IAudioClient *client = nullptr;
    WAVEFORMATEX *fmt = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void **>(&client)))
        || !client
        || FAILED(client->GetMixFormat(&fmt)) || !fmt
        // Shared-mode capture on the endpoint's mix format: read-only tap of
        // what the OS already mixes — no format negotiation, no exclusive
        // hold, nothing the user's apps can hear. The 100 ms period is a hint;
        // WASAPI snaps it to the endpoint's own quantum. LOOPBACK adds the
        // stream flag that turns a render endpoint into a capture source —
        // the engine's playout IS the machine's playout.
        || FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                     1000000 /* 100 ms */, 0, fmt, nullptr))) {
        if (fmt) CoTaskMemFree(fmt);
        if (client) client->Release();
        device->Release();
        enumerator->Release();
        return;
    }

    // The packet pump lives on the SEPARATE capture-client interface —
    // IAudioClient only exposes GetBufferSize/Start/Stop.
    IAudioCaptureClient *capture = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void **>(&capture)))
        || !capture) {
        client->Release();
        device->Release();
        enumerator->Release();
        return;
    }

    const UINT32 channels = fmt->nChannels;
    const UINT32 rate = fmt->nSamplesPerSec;
    const UINT32 bytesPerFrame = fmt->nBlockAlign;
    // The shared-mode mix format is float32 in practice; extensible formats
    // are classified by their frame size (no ksmedia subtype GUID needed —
    // MinGW's headers don't reliably carry KSDATAFORMAT_SUBTYPE_IEEE_FLOAT).
    const bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
                         || (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                             && bytesPerFrame == channels * 4);
    CoTaskMemFree(fmt);

    if (channels == 0 || rate == 0 || bytesPerFrame == 0
        || FAILED(client->Start())) {
        client->Release();
        device->Release();
        enumerator->Release();
        return;
    }

    IAudio::InputMeterLevels base;   // identity fields, snapshotted before the loop
    base.deviceId = tap.deviceId;
    base.channelCount = static_cast<uint8_t>(
        channels > IAudio::kMaxInputMeterChannels ? IAudio::kMaxInputMeterChannels : channels);
    base.sampleRateHz = rate;
    base.layout = IAudio::InputMeterLevels::LayoutFor(static_cast<int>(channels));
    // Publish the layout BEFORE the first window: the UI renders its channel
    // rows (mono vs stereo vs N) the moment the tap is alive, with the meters
    // dark until real samples arrive — layout is truth too.
    {
        const std::lock_guard<std::mutex> lock(publishMutex);
        tap.latest = base;
    }

    const UINT32 windowFrames = rate / 20;   // ~50 ms of analysis per snapshot
    UINT32 framesSinceSnapshot = 0;
    float peaks[IAudio::kMaxInputMeterChannels] = {};
    double sumSq[IAudio::kMaxInputMeterChannels] = {};
    UINT64 totalFrames = 0;

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    std::vector<uint8_t> data(bufferFrames * bytesPerFrame);

    while (tap.running.load(std::memory_order_relaxed)) {
        // The capture contract: GetNextPacketSize → GetBuffer → ReleaseBuffer,
        // per discrete packet. A drained stream sleeps a short quantum and
        // re-polls (no busy spin, no dependence on timer resolution).
        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) {
            // LOOPBACK SILENCE: a render endpoint delivers NO packets at all
            // while nothing plays (an input tap keeps clocking silence). Keep
            // the meters alive by publishing zeroed windows at the same pace —
            // the UI reads "playing silence", not a dead tap.
            if (loopback) {
                if (++framesSinceSnapshot >= windowFrames) {
                    IAudio::InputMeterLevels snap = base;
                    snap.framesCaptured = totalFrames;
                    {
                        const std::lock_guard<std::mutex> lock(publishMutex);
                        tap.latest = snap;
                    }
                    framesSinceSnapshot = 0;
                }
                totalFrames += windowFrames / 4;
            }
            Sleep(5);
            continue;
        }
        BYTE *p = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        // MinGW's headers carry no 3-arg inline overload — the raw COM
        // method's device/QPC position out-params are mandatory (docs allow
        // nullptr, but locals are simpler than arguing with the vtable).
        UINT64 devicePosition = 0, qpcPosition = 0;
        if (FAILED(capture->GetBuffer(&p, &frames, &flags,
                                      &devicePosition, &qpcPosition))) {
            Sleep(5);
            continue;
        }
        // SILENT-flag packets still advance the clock — the tap keeps
        // publishing zeroed windows so the UI's layout stays live; the data
        // pointer may be null when that flag is set, so only dereference it
        // for real audio.
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        for (UINT32 f = 0; f < frames; ++f) {
            if (!silent) {
                const uint8_t *frame = p + static_cast<size_t>(f) * bytesPerFrame;
                for (UINT32 c = 0; c < channels; ++c) {
                    float sample = 0.f;
                    if (isFloat) {
                        sample = reinterpret_cast<const float *>(frame)[c];
                    } else if (bytesPerFrame == channels * 2) {
                        // 16-bit PCM — the shared-mode mix format's integer case.
                        sample = static_cast<float>(reinterpret_cast<const int16_t *>(frame)[c])
                                 / 32768.f;
                    }
                    if (c < IAudio::kMaxInputMeterChannels) {
                        const float a = sample < 0.f ? -sample : sample;
                        if (a > peaks[c]) peaks[c] = a;
                        sumSq[c] += static_cast<double>(sample) * sample;
                    }
                }
            }
            ++totalFrames;
            if (++framesSinceSnapshot >= windowFrames) {
                IAudio::InputMeterLevels snap = base;
                snap.framesCaptured = totalFrames;
                for (int c = 0; c < snap.channelCount; ++c) {
                    snap.peaks[c] = peaks[c];
                    snap.rms[c] = static_cast<float>(
                        std::sqrt(sumSq[c] / static_cast<double>(framesSinceSnapshot)));
                    peaks[c] = 0.f;
                    sumSq[c] = 0.0;
                }
                {
                    const std::lock_guard<std::mutex> lock(publishMutex);
                    tap.latest = snap;
                }
                framesSinceSnapshot = 0;
            }
        }
        capture->ReleaseBuffer(frames);
    }

    client->Stop();
    capture->Release();
    client->Release();
    device->Release();
    enumerator->Release();
}

Result<void> WindowsAudio::StartInputMeter(uint32_t deviceId)
{
    // One roster pass validates the id AND captures the device's friendly
    // name: a bogus id must fail with NotFound (not spawn a thread that
    // immediately dies of a missing endpoint), and the name is what the tap
    // thread matches the WASAPI endpoint by (see MeterThread).
    bool known = false;
    std::string rosterName;
    for (const auto &d : Enumerate()) {
        if (d.isInput && d.id == "wavein:" + std::to_string(deviceId)) {
            known = true;
            rosterName = d.name;
            break;
        }
    }
    if (!known)
        return Error::Make(Err::NotFound, "Audio",
                           "no input device wavein:" + std::to_string(deviceId));

    if (!meters_)
        meters_ = std::make_unique<MeterTable>();

    std::unique_ptr<MeterTap> stale;
    {
        const std::lock_guard<std::mutex> lock(meters_->mutex);
        auto &slot = meters_->taps[deviceId];
        if (slot && slot->running.load())
            return Ok();   // idempotent — already running
        if (slot) {
            // A stopped tap lingers here (its device failed mid-stream);
            // replace it — and join the old worker OUTSIDE the lock below.
            slot->running.store(false);
            stale = std::move(slot);
        }
        auto tap = std::make_unique<MeterTap>();
        tap->deviceId = deviceId;
        tap->deviceName = rosterName;
        tap->running.store(true);
        // The worker references this tap's storage and the table's mutex;
        // both outlive it because every removal path joins via ~MeterTap.
        tap->worker = std::thread(&WindowsAudio::MeterThread, std::ref(*tap),
                                  std::ref(meters_->mutex), false);
        slot = std::move(tap);
    }
    stale.reset();   // join the replaced tap with the mutex free
    return Ok();
}

Result<void> WindowsAudio::StopInputMeter(uint32_t deviceId)
{
    if (meters_)
        meters_->Erase(deviceId);
    return Ok();
}

IAudio::InputMeterLevels WindowsAudio::InputLevels(uint32_t deviceId)
{
    if (!meters_)
        return IAudio::InputMeterLevels{};
    const std::lock_guard<std::mutex> lock(meters_->mutex);
    auto it = meters_->taps.find(deviceId);
    if (it == meters_->taps.end())
        return IAudio::InputMeterLevels{};
    return it->second->latest;
}

std::vector<uint32_t> WindowsAudio::ActiveInputMeters() const
{
    std::vector<uint32_t> ids;
    if (!meters_)
        return ids;
    const std::lock_guard<std::mutex> lock(meters_->mutex);
    for (const auto &[id, tap] : meters_->taps)
        if (tap->running.load() && !tap->loopback) ids.push_back(id);
    return ids;
}

// ---- OUTPUT metering (the program mix, via WASAPI loopback) ----------------
// Mirrors StartInputMeter: one roster pass validates the waveout id and
// captures the friendly name the tap thread matches the RENDER endpoint by;
// the loopback flag routes MeterThread through AUDCLNT_STREAMFLAGS_LOOPBACK.
// Input and output taps share the table — the id namespaces (wavein:/
// waveout:) never collide because both sides validate their own prefix.
Result<void> WindowsAudio::StartOutputMeter(uint32_t deviceId)
{
    bool known = false;
    std::string rosterName;
    for (const auto &d : Enumerate()) {
        if (!d.isInput && d.id == "waveout:" + std::to_string(deviceId)) {
            known = true;
            rosterName = d.name;
            break;
        }
    }
    if (!known)
        return Error::Make(Err::NotFound, "Audio",
                           "no output device waveout:" + std::to_string(deviceId));

    if (!meters_)
        meters_ = std::make_unique<MeterTable>();

    std::unique_ptr<MeterTap> stale;
    {
        const std::lock_guard<std::mutex> lock(meters_->mutex);
        auto &slot = meters_->taps[deviceId];
        if (slot && slot->running.load() && slot->loopback)
            return Ok();   // idempotent — already running
        if (slot) {
            slot->running.store(false);
            stale = std::move(slot);
        }
        auto tap = std::make_unique<MeterTap>();
        tap->deviceId = deviceId;
        tap->deviceName = rosterName;
        tap->loopback = true;
        tap->running.store(true);
        tap->worker = std::thread(&WindowsAudio::MeterThread, std::ref(*tap),
                                  std::ref(meters_->mutex), true);
        slot = std::move(tap);
    }
    stale.reset();   // join the replaced tap with the mutex free
    return Ok();
}

Result<void> WindowsAudio::StopOutputMeter(uint32_t deviceId)
{
    if (!meters_)
        return Ok();
    // Only erase when the tap IS a loopback one — an input tap may share the
    // id slot is impossible (namespaces differ), but stay precise anyway.
    {
        const std::lock_guard<std::mutex> lock(meters_->mutex);
        auto it = meters_->taps.find(deviceId);
        if (it == meters_->taps.end() || !it->second->loopback)
            return Ok();
    }
    meters_->Erase(deviceId);
    return Ok();
}

IAudio::InputMeterLevels WindowsAudio::OutputLevels(uint32_t deviceId)
{
    if (!meters_)
        return IAudio::InputMeterLevels{};
    const std::lock_guard<std::mutex> lock(meters_->mutex);
    auto it = meters_->taps.find(deviceId);
    if (it == meters_->taps.end() || !it->second->loopback)
        return IAudio::InputMeterLevels{};
    return it->second->latest;
}

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
