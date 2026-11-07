#pragma once

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
    Result<void> StartOutputAudio(const std::string &outputNodeId, uint32_t deviceId);
    void StopOutputAudio(const std::string &outputNodeId);
    void StopAllOutputAudio();
    bool OutputAudioRunning(const std::string &outputNodeId) const;

    // Whether the Speakers output is switched on in Routing (a read of the published graph snapshot).
    // Other players of sound that do not run through the mixer (the media file on air) obey the same switch.
    bool SpeakersAudible() const;

private:
    AudioEngine() = default;

    void ControlLoop();
    void SoftClockLoop();
    void MaybeFinishStop();
    void EnsureSpeakersOutput();
    void Reconcile();
    // THE SPEAKER DEVICE FOLLOWS THE SPEAKER OUTPUT. The program mix always has a clock
    // (a recording or an NDI feed reads it), but the default playback device is opened only
    // while the Speakers output is on in Routing; otherwise the software clock drives the mix
    // and no device is touched. SyncSpeakerDevice re-reads that on the control thread.
    Result<void> OpenSpeakerDevice();
    void SyncSpeakerDevice();
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
    std::atomic<bool> softClockStop_{false};   // ends the software clock while the program keeps playing
    std::mutex deviceMutex_;                   // guards softClock_ and the program render stream's open/close

    mutable std::mutex outputAudioMutex_;
    std::map<std::string, uint64_t> outputStreams_;
    std::map<std::string, uint32_t> outputDevices_;
    uint64_t nextOutputStreamId_ = 1;
};
} // namespace bps::audio