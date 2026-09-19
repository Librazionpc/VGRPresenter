#include "platform/linux/LinuxAudio.hpp"

#include "platform/PlatformAccessor.hpp"

#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace bps::platform {

namespace {

// ---------------------------------------------------------------------------
// Runtime-loaded libasound — fills idle sample-rate/channel gaps that
// /proc/asound/pcm cannot see. Mirrors the BroadcastEngine NDI loader: the
// library is resolved through the PAL ILibrary seam and degrades to the
// /proc-based path (rate/channels 0 = unknown) when libasound is absent.
// ---------------------------------------------------------------------------
struct AsoundApi {
    // snd_pcm_open(pcm, name, stream, mode); SND_PCM_STREAM_PLAYBACK=0, CAPTURE=1.
    int (*pcm_open)(void**, const char*, int, int);
    int (*pcm_close)(void*);
    // snd_pcm_hw_params_malloc/any/set_rate_near/get_rate/get_channels/free.
    int (*hw_params_malloc)(void**);
    void (*hw_params_free)(void*);
    int (*hw_params_any)(void*, void*);
    // set_rate_near(pcm, params, &rate, &dir) — clamps a preferred rate to the
    // device's supported range and stores the actual rate back into `rate`.
    int (*hw_params_set_rate_near)(void*, void*, unsigned int*, int*);
    int (*hw_params_get_rate)(const void*, unsigned int*, int*);
    int (*hw_params_get_channels)(const void*, unsigned int*);
};

class AsoundLoader {
public:
    static const AsoundApi& Api() {
        static AsoundLoader loader;
        return loader.api_;
    }

    bool Available() const { return api_.pcm_open != nullptr; }

private:
    AsoundLoader() {
        auto& platform = platform::PlatformAccessor::Get();
        auto& loader = platform.Library();
        auto h = loader.Load("libasound.so.2");
        if (!h.ok()) return;
        handle_ = h.value();
        auto sym = [&](void** out, const char* name) {
            auto s = loader.Symbol(handle_, name);
            if (s.ok()) *out = s.value();
        };
        sym((void**)&api_.pcm_open, "snd_pcm_open");
        sym((void**)&api_.pcm_close, "snd_pcm_close");
        sym((void**)&api_.hw_params_malloc, "snd_pcm_hw_params_malloc");
        sym((void**)&api_.hw_params_free, "snd_pcm_hw_params_free");
        sym((void**)&api_.hw_params_any, "snd_pcm_hw_params_any");
        sym((void**)&api_.hw_params_set_rate_near, "snd_pcm_hw_params_set_rate_near");
        sym((void**)&api_.hw_params_get_rate, "snd_pcm_hw_params_get_rate");
        sym((void**)&api_.hw_params_get_channels, "snd_pcm_hw_params_get_channels");
        if (!api_.pcm_open || !api_.pcm_close || !api_.hw_params_malloc ||
            !api_.hw_params_free || !api_.hw_params_any || !api_.hw_params_set_rate_near ||
            !api_.hw_params_get_rate || !api_.hw_params_get_channels) {
            (void)loader.Unload(handle_);
            handle_ = nullptr;
            api_ = AsoundApi{};
        }
    }

    ~AsoundLoader() {
        if (handle_)
            (void)platform::PlatformAccessor::Get().Library().Unload(handle_);
    }

    platform::ILibrary::Handle handle_ = nullptr;
    AsoundApi api_{};
};

// Probes an idle device through libasound for its default rate + channels.
// Returns false (keeping caller defaults) on any failure; never throws.
bool AlsaProbeIdle(int card, int device, bool input, uint32_t& rate, uint32_t& channels) {
    const AsoundApi& api = AsoundLoader::Api();
    if (!api.pcm_open) return false;
    std::string name = "hw:" + std::to_string(card) + "," + std::to_string(device);
    void* pcm = nullptr;
    int stream = input ? 1 : 0;   // SND_PCM_STREAM_CAPTURE / PLAYBACK
    if (api.pcm_open(&pcm, name.c_str(), stream, 0) != 0 || !pcm) return false;
    void* params = nullptr;
    if (api.hw_params_malloc(&params) != 0 || !params) {
        (void)api.pcm_close(pcm);
        return false;
    }
    bool ok = false;
    if (api.hw_params_any(pcm, params) == 0) {
        unsigned int r = 48000;   // preferred; clamped to the device's range
        int dir = 0;
        // set_rate_near stores the actual (clamped) rate back into r — on
        // "any" params get_rate alone returns a range minimum (e.g. 4000 Hz),
        // which would be misleading; the near-clamp gives a representative one.
        if (api.hw_params_set_rate_near(pcm, params, &r, &dir) == 0 && r > 0) {
            rate = r;
            ok = true;
        }
        unsigned int ch = 0;
        if (api.hw_params_get_channels(params, &ch) == 0 && ch > 0) {
            channels = ch;
            ok = true;
        }
    }
    api.hw_params_free(params);
    (void)api.pcm_close(pcm);
    return ok;
}


// card index -> friendly card name, from /proc/asound/cards:
//   ' 0 [PCH            ]: HDA-Intel - HDA Intel PCH'
std::map<int, std::string> CardNames() {
    std::map<int, std::string> out;
    std::ifstream cards("/proc/asound/cards");
    std::string line;
    static const std::regex re(R"(^\s*(\d+)\s+\[([^\]]+)\])");
    while (std::getline(cards, line)) {
        std::smatch m;
        if (std::regex_match(line, m, re))
            out[std::stoi(m[1].str())] = m[2].str();
    }
    return out;
}

// PCM line: ' 00-00: ALC892 Analog : ALC892 Analog : playback 1 : capture 1'
bool ParsePcmLine(const std::string& line, int& card, int& device,
                  std::string& pcmName, bool& playback, bool& capture) {
    std::smatch m;
    static const std::regex re(R"(^\s*(\d+)-(\d+):\s*([^:]+))");
    if (!std::regex_match(line, m, re)) return false;
    card = std::stoi(m[1].str());
    device = std::stoi(m[2].str());
    pcmName = m[3].str();
    // trim trailing space of the name segment
    while (!pcmName.empty() && pcmName.back() == ' ') pcmName.pop_back();
    playback = line.find(": playback") != std::string::npos;
    capture = line.find(": capture") != std::string::npos;
    return true;
}

// Active-stream rates/channels from hw_params (only present while a stream is
// open): 'rate: 48000' / 'channels: 2'.
void ActiveParams(int card, int device, bool input, uint32_t& rate, uint32_t& channels) {
    std::string path = "/proc/asound/card" + std::to_string(card) + "/pcm" +
                       std::to_string(device) + (input ? "c" : "p") + "/sub0/hw_params";
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("rate:", 0) == 0) rate = static_cast<uint32_t>(std::stoul(line.substr(5)));
        else if (line.rfind("channels:", 0) == 0) channels = static_cast<uint32_t>(std::stoul(line.substr(9)));
    }
}

// Default card index from /proc/asound/default ('0 [PCH ...' or 'card0'), or -1.
int DefaultCard() {
    std::ifstream f("/proc/asound/default");
    std::string line;
    if (!std::getline(f, line)) return -1;
    std::smatch m;
    static const std::regex re(R"(^(\d+))");
    if (std::regex_search(line, m, re)) return std::stoi(m[1].str());
    return -1;
}

} // namespace

std::vector<AudioDeviceInfo> LinuxAudio::Enumerate() const {
    std::vector<AudioDeviceInfo> out;
    std::map<int, std::string> cards = CardNames();
    std::ifstream pcm("/proc/asound/pcm");
    if (!pcm) return out;

    int defCard = DefaultCard();
    std::string line;
    while (std::getline(pcm, line)) {
        int card = -1, device = -1;
        std::string name;
        bool playback = false, capture = false;
        if (!ParsePcmLine(line, card, device, name, playback, capture)) continue;
        if (!playback && !capture) continue;
        std::string cardName = cards.count(card) ? cards[card] : ("card" + std::to_string(card));

        if (playback) {
            AudioDeviceInfo d;
            d.id = "hw:" + std::to_string(card) + "," + std::to_string(device);
            d.name = cardName + " " + name + " (output)";
            d.isInput = false;
            d.isDefault = (card == defCard);
            ActiveParams(card, device, false, d.sampleRateHz, d.channels);
            // Idle devices report 0 from /proc; probe the true capability via
            // runtime-loaded libasound (documented follow-up, now implemented).
            if (d.sampleRateHz == 0)
                (void)AlsaProbeIdle(card, device, false, d.sampleRateHz, d.channels);
            out.push_back(std::move(d));
        }
        if (capture) {
            AudioDeviceInfo d;
            d.id = "hw:" + std::to_string(card) + "," + std::to_string(device);
            d.name = cardName + " " + name + " (input)";
            d.isInput = true;
            d.isDefault = (card == defCard);
            ActiveParams(card, device, true, d.sampleRateHz, d.channels);
            if (d.sampleRateHz == 0)
                (void)AlsaProbeIdle(card, device, true, d.sampleRateHz, d.channels);
            out.push_back(std::move(d));
        }
    }
    return out;
}

Result<AudioDeviceInfo> LinuxAudio::DefaultOutput() const {
    auto devs = Enumerate();
    for (const auto& d : devs)
        if (!d.isInput && d.isDefault) return d;
    for (const auto& d : devs)
        if (!d.isInput) return d;
    return Error::Make(Err::NotFound, "Audio", "no output devices found");
}

Result<AudioDeviceInfo> LinuxAudio::DefaultInput() const {
    auto devs = Enumerate();
    for (const auto& d : devs)
        if (d.isInput && d.isDefault) return d;
    for (const auto& d : devs)
        if (d.isInput) return d;
    return Error::Make(Err::NotFound, "Audio", "no input devices found");
}

std::string LinuxAudio::Fingerprint() const {
    std::string fp;
    for (const auto& d : Enumerate())
        fp += d.id + (d.isInput ? ":in;" : ":out;");
    return fp;
}

} // namespace bps::platform
