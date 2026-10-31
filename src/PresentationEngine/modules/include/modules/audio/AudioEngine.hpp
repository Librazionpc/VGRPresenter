#pragma once

// AudioEngine — the playout engine. It plays the production graph's audio buses
// through the default output device, and it opens capture only for the graph
// sources that something is actually PLAYING. While the program is on the air
// that means any enabled audio bus; with the program stopped it means only the
// buses an output's own endpoint is playing (see StartOutputAudio), because
// those streams are the only thing left that can be heard. A source nobody
// hears is never captured.
//
// Ownership: this service owns the AudioMixer that sums the graph into the render
// stream. The graph is read through its published snapshot (see
// ProductionGraph::PublishedSnapshot), so the render thread never touches the
// maps the GUI thread edits.
//
// A source is bound to a capture device by its "device" meta (the device's roster
// name, written by the source's owner). Until that meta is set, a source
// contributes nothing.

#include "core/common/Common.hpp"
#include "interfaces/IService.hpp"
#include "modules/production/AudioMixer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bps::audio {

class AudioEngine final : public IService {
public:
    static AudioEngine& Instance();

    Result<void> Initialize() override;
    Result<void> Start() override;
    // Stops the PROGRAM: the default endpoint's render stream and, when it never
    // had a device, the software clock that paced it. An output playing on its
    // own endpoint keeps playing — that stream runs on its own device thread and
    // reads the graph directly (AudioMixer::RenderOutput), and its lifetime
    // follows the ROUTING, not playout (see WindowsAudio's renderStreams_). The
    // control thread and the capture bindings stay up with it, because the buses
    // those endpoints carry are exactly what must stay captured. Shutdown() is
    // what takes everything down.
    Result<void> Stop() override;
    Result<void> Shutdown() override;

    const char* ServiceName() const noexcept override { return "AudioEngine"; }
    std::vector<std::string> ServiceDependencies() const override { return {"ProductionEngine"}; }

    // True while the PROGRAM's render stream is running (an output's own endpoint
    // is separate: see OutputAudioRunning).
    bool Playing() const;
    // The program's current levels, as the mixer published them.
    production::AudioMixer::ProgramLevels ProgramLevels() const;
    // The format the mix runs at (zero until playout has rendered a chunk).
    production::AudioMixer::MixFormat MixFormat() const { return mixer_.Format(); }
    // Listeners on the mix of one graph output (see AudioMixer::AddMixTap). Recording uses
    // one per session, on the output node the session writes to.
    uint64_t AddMixTap(std::string outputNodeId, production::AudioMixer::MixTap tap)
    {
        return mixer_.AddMixTap(std::move(outputNodeId), std::move(tap));
    }
    void RemoveMixTap(uint64_t id) { mixer_.RemoveMixTap(id); }
    // Frames the mix has rendered since the engine was built: the sample clock.
    uint64_t FramesRendered() const { return mixer_.FramesRendered(); }
    // Names the output device a source is captured from (its roster name). Empty
    // removes the binding. Safe to call from any thread.
    void SetSourceDevice(const std::string &sourceId, const std::string &deviceName);
    // True while the mix runs on a software clock because no output device is open.
    bool SoftwareClock() const { return softClockOn_.load(); }

    // ---- an OUTPUT's own endpoint --------------------------------------------
    // Opens/closes a render stream for ONE output node on `deviceId` (the audio
    // roster's waveout:<n> number; UINT32_MAX = the default endpoint). The mixer
    // renders that node's own mix at the DEVICE's rate, so an HDMI monitor's
    // speakers get a feed of their own instead of riding the program stream.
    // Idempotent: starting a node that already plays on that device is Ok.
    Result<void> StartOutputAudio(const std::string &outputNodeId, uint32_t deviceId);
    void StopOutputAudio(const std::string &outputNodeId);
    void StopAllOutputAudio();
    bool OutputAudioRunning(const std::string &outputNodeId) const;

private:
    AudioEngine() = default;

    void ControlLoop();
    void SoftClockLoop();
    // The other end of a Stop() that left an output playing: when the last of
    // those streams closes and the program is not on the air either, nothing can
    // hear a source any more, so the capture devices are released here.
    void MaybeFinishStop();
    void EnsureSpeakersOutput();
    void Reconcile();
    void Unbind(const std::string &sourceId);
    void ReleaseAll();

    production::AudioMixer mixer_;

    std::mutex lifecycleMutex_;     // serializes Start/Stop/Shutdown
    bool initialized_ = false;      // guarded by lifecycleMutex_
    bool starting_ = false;         // guarded by lifecycleMutex_
    bool stopping_ = false;         // guarded by lifecycleMutex_
    // The control loop (capture reconciliation) and the graph stay live while
    // either the program or an output's own endpoint is playing — see Stop.
    std::atomic<bool> running_{false};
    // The program's render stream alone (the default endpoint / software clock).
    std::atomic<bool> programPlaying_{false};

    std::thread control_;
    // Set when control_ starts, so MaybeFinishStop can never join itself
    // (guarded by lifecycleMutex_).
    std::thread::id controlId_{};
    std::mutex wakeMutex_;
    std::condition_variable wake_;

    // Touched only by the control thread (Reconcile/Unbind/ReleaseAll), and by Stop
    // after that thread has joined.
    std::map<std::string, std::string> boundLabel_;     // source id → device name
    std::map<std::string, uint32_t> boundDevice_;       // source id → waveIn id
    std::map<uint32_t, std::set<std::string>> tapUsers_; // waveIn id → sources on it
    std::chrono::steady_clock::time_point nextRetry_{};

    // Set by SetSourceDevice and read by the control thread; it takes precedence over
    // the graph source's "device" meta.
    std::mutex namesMutex_;
    std::map<std::string, std::string> deviceNames_;

    // The software clock: drives the mix in real time when no output device is open.
    std::thread softClock_;
    std::atomic<bool> softClockOn_{false};

    // Per-output render streams (see StartOutputAudio): node id → the PAL's stream
    // id, and the device each one was opened on (so "start again on the same
    // device" is a no-op while a device CHANGE re-opens it). Guarded by the mutex;
    // the render callback never touches either map.
    mutable std::mutex outputAudioMutex_;
    std::map<std::string, uint64_t> outputStreams_;
    std::map<std::string, uint32_t> outputDevices_;
    uint64_t nextOutputStreamId_ = 1;
};

} // namespace bps::audio
