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

bool NameMatches(const std::string &winmmName, const std::string &endpointName);

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
// ============================================================================
struct WindowsAudio::MeterTap {
    std::atomic<bool> running{false};
    uint32_t deviceId = 0;
    std::string deviceName;   // roster name — the reliable endpoint match key
    bool loopback = false;    // false = input capture, true = render loopback
    bool isStream = false;
    std::string streamSource;
    std::string streamKind;
    float streamPeaks[IAudio::kMaxInputMeterChannels] = {};
    double streamSumSq[IAudio::kMaxInputMeterChannels] = {};
    uint64_t streamFrames = 0;
    uint32_t streamWindow = 0;
    IAudio::InputMeterLevels latest;   // written by the capture thread under the table's mutex

    // ---- Sample-through FIFO (the mixer's REAL carrier) ----------------
    static constexpr size_t kRingFrames = 9600;
    std::vector<float> ring;            // kRingFrames * channels samples
    size_t ringChannels = 0;
    size_t ringRead = 0, ringWrite = 0; // frame indices, ring.size() wrap
    uint32_t ringRate = 0;
    std::thread worker;
    ~MeterTap() { if (worker.joinable()) worker.join(); }
};

struct WindowsAudio::MeterTable {
    std::mutex mutex;
    std::map<uint32_t, std::unique_ptr<MeterTap>> taps;

    std::atomic<uint64_t> generation{0};

    std::mutex readerMutex;
    std::map<uint32_t, std::pair<uint64_t, IAudio::InputMeterLevels>> readerCache;

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
            generation.fetch_add(1, std::memory_order_release);   // the reader's cache is stale now
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
            generation.fetch_add(1, std::memory_order_release);
        }
        all.clear();   // joins, mutex free
    }

    ~MeterTable() { Clear(); }
};

WindowsAudio::WindowsAudio() : meters_(std::make_unique<MeterTable>()) {}
// ~MeterTable joins every live tap before the members die.
WindowsAudio::~WindowsAudio()
{
    // A RenderStream owns a std::thread, and destroying a JOINABLE thread calls
    // terminate — so every stream is stopped (which joins) before the maps go.
    (void)StopRender();
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(renderStreamsMutex_);
        for (const auto &entry : renderStreams_)
            ids.push_back(entry.first);
    }
    for (const uint64_t id : ids)
        (void)StopRenderStream(id);
}

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
        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet)) || packet == 0) {
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
        UINT64 devicePosition = 0, qpcPosition = 0;
        if (FAILED(capture->GetBuffer(&p, &frames, &flags,
                                      &devicePosition, &qpcPosition))) {
            Sleep(5);
            continue;
        }
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        std::vector<float> staged;
        staged.reserve(static_cast<size_t>(frames) * channels);
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
            for (UINT32 c = 0; c < channels; ++c)
                staged.push_back(converted[c]);
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
        if (!staged.empty() && tap.ringChannels > 0 && !tap.ring.empty()) {
            const std::lock_guard<std::mutex> lock(publishMutex);
            const size_t ringFrames = tap.ring.size() / tap.ringChannels;
            for (size_t i = 0; i + tap.ringChannels <= staged.size(); i += tap.ringChannels) {
                const size_t w = tap.ringWrite % ringFrames;
                for (UINT32 c = 0; c < tap.ringChannels; ++c)
                    tap.ring[w * tap.ringChannels + c] = staged[i + c];
                tap.ringWrite = (tap.ringWrite + 1) % ringFrames;
                if (tap.ringWrite == tap.ringRead)
                    tap.ringRead = (tap.ringRead + 1) % ringFrames;   // drop oldest
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
        meters_->generation.fetch_add(1, std::memory_order_release);   // a new tap id may be read now
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
    MeterTable *table = IAudio::IsStreamTap(deviceId) ? streams_.get() : meters_.get();
    if (!table)
        return IAudio::InputMeterLevels{};

    // ---- THE INTERFACE THREAD MUST NOT WAIT HERE -----------------------------
    const uint64_t gen = table->generation.load(std::memory_order_relaxed);
    IAudio::InputMeterLevels cached;
    bool haveCached = false;
    {
        const std::lock_guard<std::mutex> cacheLock(table->readerMutex);
        auto c = table->readerCache.find(deviceId);
        if (c != table->readerCache.end() && c->second.first == gen) {
            cached = c->second.second;
            haveCached = true;
        }
    }
    std::unique_lock<std::mutex> lock(table->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return haveCached ? cached : IAudio::InputMeterLevels{};

    auto it = table->taps.find(deviceId);
    const IAudio::InputMeterLevels value =
        it == table->taps.end() ? IAudio::InputMeterLevels{} : it->second->latest;
    {
        const std::lock_guard<std::mutex> cacheLock(table->readerMutex);
        table->readerCache[deviceId] = { gen, value };
    }
    return value;
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
        streams_->generation.fetch_add(1, std::memory_order_release);
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

void WindowsAudio::StreamThread(MeterTap &tap, MeterTable &table, std::string source)
{
    std::mutex &publishMutex = table.mutex;   // where a snapshot must be published
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
        meters_->generation.fetch_add(1, std::memory_order_release);   // a new tap id may be read now
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

std::vector<AudioDeviceInfo> WindowsAudio::EnumerateFiltered(
    const std::set<std::string> *only) const {
    ScopedComInit com;
    std::vector<AudioDeviceInfo> out;

    const std::map<std::string, AudioDeviceInfo> inTruth = QueryWasapiMixFormats(true, only);
    const std::map<std::string, AudioDeviceInfo> outTruth = QueryWasapiMixFormats(false, only);

    // Output devices (winmm). dwFormats is a bitmask; 44.1k/48k are near
    // universal, so those are reported as the nominal rate.
    UINT outputs = waveOutGetNumDevs();
    for (UINT i = 0; i < outputs; ++i) {
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
// ---------------------------------------------------------------------------
struct WindowsAudio::RenderStream
{
    std::thread thread;
    std::atomic<bool> running{false};
    IAudio::RenderCallback callback;
    uint32_t deviceId = UINT32_MAX;   // UINT32_MAX = the default output
};

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
    (void)StopRender();   // a dead stream's thread may need joining before reuse

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

Result<void> WindowsAudio::StartRenderStream(uint64_t streamId, RenderCallback callback,
                                             uint32_t deviceId)
{
    if (!callback)
        return Error::Make(Err::InvalidArgument, "Audio", "render callback is empty");

    std::unique_ptr<RenderStream> dying;
    {
        std::lock_guard<std::mutex> lock(renderStreamsMutex_);
        auto it = renderStreams_.find(streamId);
        if (it != renderStreams_.end()) {
            if (it->second && it->second->running.load() && it->second->deviceId == deviceId)
                return Ok();
            dying = std::move(it->second);
            renderStreams_.erase(it);
        }
    }
    // Joined OUTSIDE the lock: RenderThread never touches the map, and holding
    // the mutex across a join would stall every other output's start/stop.
    if (dying) {
        dying->running.store(false);
        if (dying->thread.joinable())
            dying->thread.join();
        dying.reset();
    }

    auto s = std::make_unique<RenderStream>();
    s->callback = std::move(callback);
    s->deviceId = deviceId;
    s->running.store(true);
    s->thread = std::thread([&sref = *s]() { RenderThread(sref); });
    {
        std::lock_guard<std::mutex> lock(renderStreamsMutex_);
        renderStreams_[streamId] = std::move(s);
    }
    return Ok();
}

Result<void> WindowsAudio::StopRenderStream(uint64_t streamId)
{
    std::unique_ptr<RenderStream> dying;
    {
        std::lock_guard<std::mutex> lock(renderStreamsMutex_);
        const auto it = renderStreams_.find(streamId);
        if (it == renderStreams_.end())
            return Ok();
        dying = std::move(it->second);
        renderStreams_.erase(it);
    }
    dying->running.store(false);
    if (dying->thread.joinable())
        dying->thread.join();
    return Ok();
}
} // namespace bps::platform