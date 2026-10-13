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
// Media Foundation: the audio decoder behind a media file's stream tap.
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
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
// Minimal COM smart pointer (no ATL/WRL in this build): the Media Foundation
// audio reader below hands out IMFSourceReader/IMFSample/IMFMediaBuffer.
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;
    ~ComPtr() { if (p_) p_->Release(); }
    T **put() { return &p_; }
    T *get() const { return p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    T *p_ = nullptr;
};

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
// Defined below (and used by the name check in the filtered walk above it).
bool NameMatches(const std::string &winmmName, const std::string &endpointName);

// `only` (optional) limits the mix-format queries to these friendly names —
// the activation + GetMixFormat below is the expensive half of an enumeration,
// and a caller that named its devices (IAudio::EnumerateFor) has no use for
// the rest of them. Names are matched the way the roster matches them
// (NameMatches), with a trimmed compare as the fallback.
std::map<std::string, AudioDeviceInfo> QueryWasapiMixFormats(
    bool input, const std::set<std::string> *only = nullptr)
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
                if (!name.empty() && (!only || only->count(name) > 0 ||
                                      [&] {
                                          for (const std::string &w : *only)
                                              if (NameMatches(name, w)) return true;
                                          return false;
                                      }())) {
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

// Is this device one of the names a targeted walk was asked for? Exact first
// (the common case: the roster was built from this very walk, so the strings
// are identical), then case-insensitive with surrounding whitespace ignored.
// The fallback exists because the wanted set is a SAVED roster: a label that
// differs by case or a stray space would otherwise read as "device not
// connected" and produce a false missing-device notice for a device that is
// sitting right there.
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
    const std::string wanted = trim(name);
    for (const std::string &w : *only) {
        const std::string candidate = trim(w);
        if (candidate.size() != wanted.size()) continue;
        bool same = true;
        for (size_t i = 0; i < wanted.size() && same; ++i)
            same = std::tolower(static_cast<unsigned char>(wanted[i]))
                   == std::tolower(static_cast<unsigned char>(candidate[i]));
        if (same) return true;
    }
    return false;
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
    // STREAM taps (a media file's audio track, an NDI stream's embedded audio)
    // live in the stream table and carry their source instead of a device: the
    // capture thread below is replaced by either the Media Foundation decode
    // thread or nothing at all (an NDI receiver pushes the samples).
    bool isStream = false;
    std::string streamSource;
    std::string streamKind;
    // Analysis state for PUSHED samples (PushStreamTapAudio): the same 50 ms
    // window the capture loop runs, kept per tap so a producer feeding blocks
    // of any size still publishes whole windows.
    float streamPeaks[IAudio::kMaxInputMeterChannels] = {};
    double streamSumSq[IAudio::kMaxInputMeterChannels] = {};
    uint64_t streamFrames = 0;
    uint32_t streamWindow = 0;
    IAudio::InputMeterLevels latest;   // written by the capture thread under the table's mutex

    // ---- Sample-through FIFO (the mixer's REAL carrier) ----------------
    // The capture thread pushes its converted interleaved float32 frames
    // here; AudioMixer (via ReadTapAudio) drains them per render chunk —
    // the device's actual samples, not a level-carried approximation.
    // ~200 ms of headroom at 48 kHz absorbs render-chunk jitter without
    // growing; the mixer drains at the playout rate, so this stays near
    // empty in steady state and a slow pump drops OLDEST samples (voice).
    static constexpr size_t kRingFrames = 9600;
    std::vector<float> ring;            // kRingFrames * channels samples
    size_t ringChannels = 0;
    size_t ringRead = 0, ringWrite = 0; // frame indices, ring.size() wrap
    uint32_t ringRate = 0;
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

    // The sample-through ring: allocate once per tap start (format-fixed).
    tap.ringChannels = channels;
    tap.ringRate = rate;
    tap.ring.assign(static_cast<size_t>(MeterTap::kRingFrames) * channels, 0.0f);
    tap.ringRead = tap.ringWrite = 0;

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
            float converted[IAudio::kMaxInputMeterChannels] = {};
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
                    converted[c] = sample;
                    if (c < IAudio::kMaxInputMeterChannels) {
                        const float a = sample < 0.f ? -sample : sample;
                        if (a > peaks[c]) peaks[c] = a;
                        sumSq[c] += static_cast<double>(sample) * sample;
                    }
                }
            }
            // Sample-through: push the frame into the FIFO (overwrites the
            // oldest when the render side falls behind — drop-old keeps a
            // live input live instead of lagging forever). Under
            // publishMutex — ReadTapAudio (the render thread, via
            // AudioMixer::SumSource) reads/mutates this SAME ring, ringRead
            // and ringWrite under that same lock; this push used to run
            // unlocked, a real data race against that reader (found in
            // review — an uncontended std::mutex lock is tens of ns, and a
            // WASAPI packet is a handful of frames, not the render chunk,
            // so the per-sample cost here is negligible against the
            // correctness it buys).
            {
                const std::lock_guard<std::mutex> lock(publishMutex);
                const size_t ringFrames = tap.ring.size() / (tap.ringChannels ? tap.ringChannels : 1);
                const size_t w = tap.ringWrite % ringFrames;
                for (UINT32 c = 0; c < tap.ringChannels; ++c)
                    tap.ring[w * tap.ringChannels + c] = converted[c];
                tap.ringWrite = (tap.ringWrite + 1) % ringFrames;
                if (tap.ringWrite == tap.ringRead)
                    tap.ringRead = (tap.ringRead + 1) % ringFrames;   // drop oldest
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

Result<void> WindowsAudio::StartInputMeter(uint32_t deviceId, const std::string &deviceName)
{
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
        tap->deviceName = deviceName;
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
    // A stream tap answers here too (disjoint id ranges): the UI's per-label
    // level lookup is then the SAME call for a device and for a media file or
    // NDI stream, which is what lets one card kind meter either.
    if (IAudio::IsStreamTap(deviceId)) {
        if (!streams_)
            return IAudio::InputMeterLevels{};
        const std::lock_guard<std::mutex> lock(streams_->mutex);
        auto it = streams_->taps.find(deviceId);
        return it == streams_->taps.end() ? IAudio::InputMeterLevels{} : it->second->latest;
    }
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

// ---- Tap sample-through (the mixer's real carrier) -------------------------
IAudio::TapAudio WindowsAudio::TapFormat(uint32_t deviceId)
{
    if (IAudio::IsStreamTap(deviceId)) {
        if (!streams_)
            return {};
        const std::lock_guard<std::mutex> lock(streams_->mutex);
        auto it = streams_->taps.find(deviceId);
        if (it == streams_->taps.end() || it->second->ringChannels == 0)
            return {};
        IAudio::TapAudio out;
        out.channels = static_cast<uint32_t>(it->second->ringChannels);
        out.sampleRateHz = it->second->ringRate;
        return out;
    }
    if (!meters_)
        return {};
    const std::lock_guard<std::mutex> lock(meters_->mutex);
    auto it = meters_->taps.find(deviceId);
    if (it == meters_->taps.end() || it->second->loopback || it->second->ringChannels == 0)
        return {};
    IAudio::TapAudio out;
    out.channels = static_cast<uint32_t>(it->second->ringChannels);
    out.sampleRateHz = it->second->ringRate;
    return out;
}

size_t WindowsAudio::ReadTapAudio(uint32_t deviceId, float *dst, size_t frames)
{
    if (!dst || frames == 0)
        return 0;
    // A stream tap's ring is drained exactly like a device tap's — the mixer
    // does not know (or care) which it is bound to.
    if (IAudio::IsStreamTap(deviceId)) {
        if (!streams_)
            return 0;
        const std::lock_guard<std::mutex> lock(streams_->mutex);
        auto it = streams_->taps.find(deviceId);
        if (it == streams_->taps.end() || it->second->ringChannels == 0)
            return 0;
        MeterTap &stream = *it->second;
        const size_t streamRingFrames = stream.ring.size() / stream.ringChannels;
        const size_t streamChans = stream.ringChannels;
        size_t streamGot = 0;
        while (streamGot < frames && stream.ringRead != stream.ringWrite) {
            const size_t r = stream.ringRead % streamRingFrames;
            std::memcpy(dst + streamGot * streamChans,
                        &stream.ring[r * streamChans], streamChans * sizeof(float));
            stream.ringRead = (stream.ringRead + 1) % streamRingFrames;
            ++streamGot;
        }
        return streamGot;
    }
    if (!meters_)
        return 0;
    const std::lock_guard<std::mutex> lock(meters_->mutex);
    auto it = meters_->taps.find(deviceId);
    if (it == meters_->taps.end() || it->second->loopback || it->second->ringChannels == 0)
        return 0;
    MeterTap &tap = *it->second;
    const size_t ringFrames = tap.ring.size() / tap.ringChannels;
    const size_t chans = tap.ringChannels;
    size_t got = 0;
    while (got < frames && tap.ringRead != tap.ringWrite) {
        const size_t r = tap.ringRead % ringFrames;
        std::memcpy(dst + got * chans, &tap.ring[r * chans], chans * sizeof(float));
        tap.ringRead = (tap.ringRead + 1) % ringFrames;
        ++got;
    }
    return got;
}

// ============================================================================
// STREAM taps — audio that is not a capture device.
//
// A video source carries its own audio (a media file's track, an NDI stream's
// embedded audio) that no endpoint tap can reach. A stream tap is the SAME
// MeterTap the device taps use, with two possible producers:
//   * kind "media": StreamThread below decodes the file's audio with Media
//     Foundation and hands each block to PushStreamTapAudio.
//   * kind "ndi": nobody local decodes it — the NDI receiver that owns the
//     stream calls PushStreamTapAudio as frames arrive.
// Both producers land in ONE place, so the levels, the layout snapshot and the
// sample ring the mixer drains are the code the device taps already run: a
// stream is metered, and can be mixed, exactly like a device.
// ============================================================================

Result<void> WindowsAudio::StartStreamTap(uint32_t tapId, const std::string &source,
                                          const std::string &kind)
{
    if (!IAudio::IsStreamTap(tapId))
        return Error::Make(Err::InvalidArgument, "Audio",
                           "a stream tap id must be in the stream range");
    if (source.empty())
        return Error::Make(Err::InvalidArgument, "Audio", "a stream tap needs a source");
    const bool media = kind == "media";
    const bool ndi = kind == "ndi";
    if (!media && !ndi)
        return Error::Make(Err::Unsupported, "Audio",
                           "no stream tap producer for kind '" + kind + "'");
    // A media tap decodes the file itself, so a file that is not there is
    // refused here (NotFound) rather than living on as a tap that never sounds.
    if (media && GetFileAttributesW(win::Wide(source).c_str()) == INVALID_FILE_ATTRIBUTES)
        return Error::Make(Err::NotFound, "Audio", "no media file at '" + source + "'");

    if (!streams_)
        streams_ = std::make_unique<MeterTable>();

    std::unique_ptr<MeterTap> stale;
    {
        const std::lock_guard<std::mutex> lock(streams_->mutex);
        auto &slot = streams_->taps[tapId];
        if (slot && slot->running.load()
            && slot->streamSource == source && slot->streamKind == kind)
            return Ok();   // idempotent — this source is already live on this tap
        if (slot) {
            // A tap on a DIFFERENT source (or a dead one): replace it, joining
            // the old worker OUTSIDE the lock below (it publishes under that lock).
            slot->running.store(false);
            stale = std::move(slot);
        }
        auto tap = std::make_unique<MeterTap>();
        tap->deviceId = tapId;
        tap->isStream = true;
        tap->streamSource = source;
        tap->streamKind = kind;
        tap->running.store(true);
        // "ndi" gets no worker: its samples are pushed by the receiver. The tap
        // still exists from here on, so a push that arrives first is accepted.
        if (media)
            tap->worker = std::thread(&WindowsAudio::StreamThread, std::ref(*tap),
                                      std::ref(*streams_), source);
        slot = std::move(tap);
    }
    stale.reset();   // join the replaced tap with the mutex free
    return Ok();
}

Result<void> WindowsAudio::StopStreamTap(uint32_t tapId)
{
    if (streams_)
        streams_->Erase(tapId);
    return Ok();
}

std::vector<uint32_t> WindowsAudio::ActiveStreamTaps() const
{
    std::vector<uint32_t> ids;
    if (!streams_)
        return ids;
    const std::lock_guard<std::mutex> lock(streams_->mutex);
    for (const auto &[id, tap] : streams_->taps)
        if (tap->running.load()) ids.push_back(id);
    return ids;
}

Result<void> WindowsAudio::PushStreamTapAudio(uint32_t tapId, const float *interleaved,
                                              uint32_t channels, uint32_t frames,
                                              uint32_t rate)
{
    if (!streams_)
        return Ok();   // nobody holds this tap any more: the reader went away
    return PushToTapTable(*streams_, tapId, interleaved, channels, frames, rate);
}

// The one implementation of "samples arrived": both producers (the decode
// thread and an NDI receiver) and the public push call land here, so a stream
// tap's levels/ring/windowing cannot drift between them.
Result<void> WindowsAudio::PushToTapTable(MeterTable &table, uint32_t tapId,
                                          const float *interleaved, uint32_t channels,
                                          uint32_t frames, uint32_t rate)
{
    if (!IAudio::IsStreamTap(tapId))
        return Error::Make(Err::InvalidArgument, "Audio", "not a stream tap id");
    if (!interleaved || frames == 0 || rate == 0 || channels == 0
        || channels > IAudio::kMaxInputMeterChannels)
        return Error::Make(Err::InvalidArgument, "Audio", "malformed stream tap block");

    const std::lock_guard<std::mutex> lock(table.mutex);
    auto it = table.taps.find(tapId);
    if (it == table.taps.end())
        return Ok();
    MeterTap &tap = *it->second;

    // First block (or a format change): size the ring and publish the layout,
    // exactly as a capture tap does when it learns its mix format — the UI's
    // channel rows appear while the first window is still filling.
    if (tap.ringChannels != channels || tap.ringRate != rate) {
        tap.ringChannels = channels;
        tap.ringRate = rate;
        tap.ring.assign(static_cast<size_t>(MeterTap::kRingFrames) * channels, 0.0f);
        tap.ringRead = tap.ringWrite = 0;
    }
    IAudio::InputMeterLevels base = tap.latest;
    base.deviceId = tapId;
    base.channelCount = static_cast<uint8_t>(channels);
    base.sampleRateHz = rate;
    base.layout = IAudio::InputMeterLevels::LayoutFor(static_cast<int>(channels));
    tap.latest = base;

    const size_t ringFrames = tap.ring.size() / channels;
    const uint32_t windowFrames = rate / 20;   // the device taps' ~50 ms window
    for (uint32_t f = 0; f < frames; ++f) {
        const float *frame = interleaved + static_cast<size_t>(f) * channels;
        for (uint32_t c = 0; c < channels; ++c) {
            const float a = frame[c] < 0.f ? -frame[c] : frame[c];
            if (a > tap.streamPeaks[c]) tap.streamPeaks[c] = a;
            tap.streamSumSq[c] += static_cast<double>(frame[c]) * frame[c];
        }
        const size_t w = tap.ringWrite % ringFrames;
        std::memcpy(&tap.ring[w * channels], frame, channels * sizeof(float));
        tap.ringWrite = (tap.ringWrite + 1) % ringFrames;
        if (tap.ringWrite == tap.ringRead)
            tap.ringRead = (tap.ringRead + 1) % ringFrames;   // drop oldest

        ++tap.streamFrames;
        if (++tap.streamWindow >= windowFrames) {
            IAudio::InputMeterLevels snap = base;
            snap.framesCaptured = tap.streamFrames;
            for (int c = 0; c < snap.channelCount; ++c) {
                snap.peaks[c] = tap.streamPeaks[c];
                snap.rms[c] = static_cast<float>(
                    std::sqrt(tap.streamSumSq[c] / static_cast<double>(tap.streamWindow)));
                tap.streamPeaks[c] = 0.f;
                tap.streamSumSq[c] = 0.0;
            }
            tap.latest = snap;
            tap.streamWindow = 0;
        }
    }
    return Ok();
}

// The kind="media" producer: a Media Foundation audio-stream reader pumping the
// file's own track into the tap, paced by the samples' timestamps so the meter
// and the mixer see the file play in real time rather than as fast as the disk
// allows.
void WindowsAudio::StreamThread(MeterTap &tap, MeterTable &table, std::string source)
{
    std::mutex &publishMutex = table.mutex;   // where a snapshot must be published
    // COM for this thread (MF hands out apartment-bound objects). Media
    // Foundation is started ONCE for the process and deliberately not shut
    // down here: MFShutdown is process-wide, and a stream tap ending must never
    // tear the platform out from under the video path still decoding.
    ScopedComInit com;
    static std::once_flag mfOnce;
    std::call_once(mfOnce, []() { (void)MFStartup(MF_VERSION, MFSTARTUP_LITE); });

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(win::Wide(source).c_str(), nullptr, reader.put())))
        return;   // unreadable file: the tap stays silent (no level, no fake)
    // Audio only: the video stream is the frame tap's business, not this one's.
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)))
        return;   // no audio track at all
    ComPtr<IMFMediaType> want;
    if (FAILED(MFCreateMediaType(want.put())))
        return;
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);   // float32 PCM, whatever the codec was
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                           want.get())))
        return;
    ComPtr<IMFMediaType> got;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, got.put())))
        return;
    UINT32 channels = 0, rate = 0;
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (channels == 0 || rate == 0 || channels > IAudio::kMaxInputMeterChannels)
        return;
    {
        // Layout is truth too: publish it before the first window so the UI's
        // channel rows are up while the first block is still decoding.
        const std::lock_guard<std::mutex> lock(publishMutex);
        IAudio::InputMeterLevels base = tap.latest;
        base.deviceId = tap.deviceId;
        base.channelCount = static_cast<uint8_t>(channels);
        base.sampleRateHz = rate;
        base.layout = IAudio::InputMeterLevels::LayoutFor(static_cast<int>(channels));
        tap.latest = base;
    }

    LONGLONG firstTs = 0;
    bool haveFirst = false;
    auto wall0 = std::chrono::steady_clock::now();
    while (tap.running.load(std::memory_order_relaxed)) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                                      &flags, &ts, sample.put())))
            break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            // The file ended (or the tap is on a stale position): publish a
            // zeroed window so the meters read silence instead of freezing on
            // the last thing they caught, then park until stopped.
            //
            // THE PUBLISH IS ONE SHORT LOCKED SCOPE, AND THE PARK IS OUTSIDE
            // IT. The park used to sit inside this lock_guard, so a media tap
            // whose file had simply PLAYED OUT held the table's mutex for as
            // long as the row stayed open — and the UI's meter pump calls
            // InputLevels(tapId) on the GUI THREAD every 50 ms, which takes
            // that same mutex: the moment a media row's audio reached its end
            // the whole interface froze (the lock was never released because
            // the tap is parked, not stopped), which read as "the app hung".
            // A wait — sleeping included — must never happen under the lock
            // the readers of the thing being waited on need (the same rule the
            // modules' lock-scope guard enforces, one layer down).
            {
                const std::lock_guard<std::mutex> lock(publishMutex);
                IAudio::InputMeterLevels quiet = tap.latest;
                for (int c = 0; c < quiet.channelCount; ++c) {
                    quiet.peaks[c] = 0.0f;
                    quiet.rms[c] = 0.0f;
                }
                tap.latest = quiet;
            }
            while (tap.running.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }
        if (!sample)
            continue;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            // The track changed format mid-file (concatenated transports do
            // this): re-read the negotiated type and keep decoding — the tap
            // re-sizes its ring on the next block when the shape differs.
            ComPtr<IMFMediaType> now;
            if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                      now.put()))) {
                UINT32 chans = 0, hz = 0;
                now->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &chans);
                now->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &hz);
                if (chans > 0 && chans <= IAudio::kMaxInputMeterChannels)
                    channels = chans;
                if (hz > 0)
                    rate = hz;
            }
            continue;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer)
            continue;
        BYTE *data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length)) || !data) {
            if (data) buffer->Unlock();
            continue;
        }
        const uint32_t frames = static_cast<uint32_t>(
            length / (static_cast<DWORD>(channels) * sizeof(float)));
        if (frames > 0)
            (void)PushToTapTable(table, tap.deviceId, reinterpret_cast<const float *>(data),
                                 channels, frames, rate);
        buffer->Unlock();

        // Pace to the stream's own clock: without this the whole file is
        // drained in milliseconds and the meter is a single spike.
        if (!haveFirst) {
            firstTs = ts;
            wall0 = std::chrono::steady_clock::now();
            haveFirst = true;
        }
        const double target = static_cast<double>(ts - firstTs) / 10000000.0;
        double ahead = target
            - std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        if (ahead > 0.5) {
            // A long silence in the file: never hold the tap for the whole gap.
            firstTs = ts;
            wall0 = std::chrono::steady_clock::now();
            ahead = 0.0;
        }
        while (ahead > 0.0 && tap.running.load(std::memory_order_relaxed)) {
            const double slice = ahead > 0.005 ? 0.005 : ahead;
            std::this_thread::sleep_for(std::chrono::duration<double>(slice));
            ahead -= slice;
        }
        if (ahead < -0.5) {
            // Fell far behind (a stalled decode): re-anchor rather than
            // accumulating a debt that would make every later block late.
            firstTs = ts;
            wall0 = std::chrono::steady_clock::now();
        }
    }
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
    return EnumerateFiltered(nullptr);
}

std::vector<AudioDeviceInfo> WindowsAudio::EnumerateFor(
    const std::vector<std::string> &names) const {
    // Nothing named ⇒ nothing wanted: this is the "no audio in this setup"
    // answer, and it costs no COM at all.
    if (names.empty())
        return {};
    const std::set<std::string> only(names.begin(), names.end());
    return EnumerateFiltered(&only);
}

// The one walk both entry points share (see the header): `only` narrows the
// WASAPI truth query AND the WinMM caps loops to the named endpoints, so a
// caller that wants two devices never activates the other six.
std::vector<AudioDeviceInfo> WindowsAudio::EnumerateFiltered(
    const std::set<std::string> *only) const {
    ScopedComInit com;
    std::vector<AudioDeviceInfo> out;

    // WASAPI mix-format truth per endpoint, name-matched against the WinMM
    // roster below (see QueryWasapiMixFormats — mono mics report 1, stereo
    // 2, 7.1 outputs 8; caps alone can't tell).
    const std::map<std::string, AudioDeviceInfo> inTruth = QueryWasapiMixFormats(true, only);
    const std::map<std::string, AudioDeviceInfo> outTruth = QueryWasapiMixFormats(false, only);

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
        if (!NamedBy(only, d.name)) continue;
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
        if (only && !only->count(d.name)) continue;
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

// ---------------------------------------------------------------------------
// RENDER — the engine's playout. A shared-mode WASAPI render client: the
// first output path in this codebase that makes sound. One stream; the
// callback pulls the mixer's frames from the engine's production graph.
// ---------------------------------------------------------------------------
struct WindowsAudio::RenderStream
{
    std::thread thread;
    std::atomic<bool> running{false};
    IAudio::RenderCallback callback;
    uint32_t deviceId = UINT32_MAX;   // UINT32_MAX = the default output
};

// The render thread body: device matching (the meter thread's PRIMARY-name /
// positional-fallback scheme), shared-mode IAudioClient on the endpoint mix
// format, then the padded-frames pump writing the mixer's frames to the
// speaker. Exits when stream.running goes false (clean stop) — the device
// is released by the thread itself, so the stream never touches COM after
// the join.
void WindowsAudio::RenderThread(RenderStream &stream)
{
    ScopedComInit com;
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator)
        return;

    // Default output unless a roster device was named (same correlation as
    // the meter thread: friendly name first, positional fallback).
    IMMDevice *device = nullptr;
    if (stream.deviceId == UINT32_MAX) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)))
            device = nullptr;
    } else {
        IMMDeviceCollection *collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr) && collection) {
            UINT count = 0;
            collection->GetCount(&count);
            // Name matching needs the WinMM device name; the roster passes
            // the number, so positional is the honest match here (the meter
            // thread's PRIMARY-name path needs the name string, which the
            // render start doesn't carry).
            if (stream.deviceId < count)
                collection->Item(static_cast<UINT>(stream.deviceId), &device);
            collection->Release();
        }
    }
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
        // SHARED mode: coexists with every app; the endpoint's own mix format
        // (float32 in practice) means no negotiation, no exclusive hold. The
        // 100 ms buffer rides the same hint the meter taps use; WASAPI snaps
        // it to the endpoint's quantum.
        || FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                     1000000 /* 100 ms */, 0, fmt, nullptr))) {
        if (fmt) CoTaskMemFree(fmt);
        if (client) client->Release();
        device->Release();
        enumerator->Release();
        return;
    }

    IAudioRenderClient *render = nullptr;
    if (FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void **>(&render))) || !render) {
        CoTaskMemFree(fmt);
        client->Release();
        device->Release();
        enumerator->Release();
        return;
    }

    const UINT32 channels = fmt->nChannels;
    const UINT32 rate = fmt->nSamplesPerSec;
    const UINT32 bytesPerFrame = fmt->nBlockAlign;
    const bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT
                         || (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE
                             && bytesPerFrame == channels * 4);
    CoTaskMemFree(fmt);

    if (channels == 0 || rate == 0 || bytesPerFrame == 0
        || FAILED(client->Start())) {
        render->Release();
        client->Release();
        device->Release();
        enumerator->Release();
        return;
    }

    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);

    while (stream.running.load(std::memory_order_relaxed)) {
        // The render contract: how much room is there? Fill that much.
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            Sleep(5);
            continue;
        }
        UINT32 available = bufferFrames > padding ? bufferFrames - padding : 0;
        if (available == 0) {
            Sleep(5);
            continue;
        }
        // One scratch staging buffer per stream (allocated ONCE per chunk-
        // size change, outside the mixer call), reused every pump: the mixer
        // writes into it, the writer converts into WASAPI's own buffer.
        static thread_local std::vector<float> staging;
        const size_t want = static_cast<size_t>(available) * channels;
        if (staging.size() < want)
            staging.resize(want);

        bool got = stream.callback ? stream.callback(staging.data(), available, channels, rate)
                                   : false;

        BYTE *dst = nullptr;
        if (FAILED(render->GetBuffer(available, &dst)) || !dst) {
            Sleep(5);
            continue;
        }
        if (!got) {
            // No mixer output this chunk → real silence, not a repeat of the
            // last buffer (a stuck loop is how engines howl).
            memset(dst, 0, static_cast<size_t>(available) * bytesPerFrame);
        } else if (isFloat) {
            memcpy(dst, staging.data(), want * sizeof(float));
        } else {
            // Integer endpoint format (rare): clamp-convert the float mix.
            auto *out = reinterpret_cast<int16_t *>(dst);
            for (size_t i = 0; i < want; ++i) {
                float s = staging[i];
                s = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
                out[i] = static_cast<int16_t>(s * 32767.f);
            }
        }
        render->ReleaseBuffer(available, 0);
    }

    client->Stop();
    render->Release();
    client->Release();
    device->Release();
    enumerator->Release();
}

Result<void> WindowsAudio::StartRender(RenderCallback callback, uint32_t deviceId)
{
    if (!callback)
        return Error::Make(Err::InvalidArgument, "Audio", "render callback is empty");
    // Idempotent start: a running stream keeps its mixer (one playout path;
    // a second Start is the UI asking twice, not a new mix).
    if (render_ && render_->running.load())
        return Ok();
    StopRender();   // a dead stream's thread may need joining before reuse

    auto s = std::make_unique<RenderStream>();
    s->callback = std::move(callback);
    s->deviceId = deviceId;
    s->running.store(true);
    s->thread = std::thread([&sref = *s]() { RenderThread(sref); });
    render_ = std::move(s);
    return Ok();
}

Result<void> WindowsAudio::StopRender()
{
    if (!render_)
        return Ok();
    render_->running.store(false);
    if (render_->thread.joinable())
        render_->thread.join();
    render_.reset();
    return Ok();
}

bool WindowsAudio::Rendering() const
{
    return render_ && render_->running.load();
}

} // namespace bps::platform
