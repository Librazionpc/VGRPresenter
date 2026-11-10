#include "platform/qt/QtMediaDevices.hpp"

#include <QAudioDevice>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QMediaDevices>
#include <QMetaObject>
#include <QSpan>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace bps::platform {
namespace {
constexpr size_t kRingFrames = 16'384;
constexpr uint64_t kProgramStreamId = std::numeric_limits<uint64_t>::max();
constexpr int kMaxChannels = IAudio::kMaxInputMeterChannels;

float ToFloat(float value) noexcept
{
    return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
}
float ToFloat(int16_t value) noexcept { return static_cast<float>(value) / 32768.0f; }
float ToFloat(int32_t value) noexcept { return static_cast<float>(value) / 2147483648.0f; }
float ToFloat(uint8_t value) noexcept { return (static_cast<float>(value) - 128.0f) / 128.0f; }

template <typename Sample>
Sample FromFloat(float value) noexcept;
template <> float FromFloat(float value) noexcept { return ToFloat(value); }
template <> int16_t FromFloat(float value) noexcept
{
    const float bounded = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(bounded <= -1.0f ? -32768.0f : bounded * 32767.0f);
}
template <> int32_t FromFloat(float value) noexcept
{
    const double bounded = std::clamp(static_cast<double>(value), -1.0, 1.0);
    return static_cast<int32_t>(bounded <= -1.0 ? -2147483648.0
                                                  : bounded * 2147483647.0);
}
template <> uint8_t FromFloat(float value) noexcept
{
    const float bounded = std::clamp(value, -1.0f, 1.0f);
    return static_cast<uint8_t>(std::clamp(std::lround(bounded * 127.5f + 127.5f), 0l, 255l));
}

template <typename Sample>
Sample Silence() noexcept { return Sample{}; }
template <>
uint8_t Silence<uint8_t>() noexcept { return 128; }

struct CaptureTap {
    std::atomic<uint32_t> channels{0};
    std::atomic<uint32_t> rate{0};
    std::atomic<bool> running{true};
    std::mutex producerMutex;
    std::vector<float> ring = std::vector<float>(kRingFrames * kMaxChannels);
    std::atomic<uint64_t> written{0};
    std::atomic<uint64_t> read{0};
    std::atomic<uint64_t> frames{0};
    std::atomic<uint8_t> meterChannels{0};
    std::array<std::atomic<float>, kMaxChannels> peaks{};
    std::array<std::atomic<float>, kMaxChannels> rms{};
};

struct RenderTap {
    IAudio::RenderCallback callback;
    std::vector<float> scratch;
    uint32_t deviceId = UINT32_MAX;
    std::atomic<bool> running{false};
    // OUTPUT METER: the level of exactly what this stream hands the device, read after each callback.
    // The program mix is PLAYED by the app through this stream, so its level is known without an
    // operating-system loopback (WASAPI loopback on Windows, a monitor source on PulseAudio, a tap on
    // macOS — three different mechanisms for one number): the same meter on every OS, and it reads what
    // this app plays rather than every other program's sound on the machine.
    std::atomic<uint8_t> meterChannels{0};
    std::atomic<uint32_t> meterRate{0};
    std::atomic<uint64_t> meterFrames{0};
    std::array<std::atomic<float>, kMaxChannels> peaks{};
    std::array<std::atomic<float>, kMaxChannels> rms{};
};

// Publishes the peak and RMS of one rendered block (interleaved floats), per channel.
void MeterRender(RenderTap& tap, const float* data, size_t frames, uint32_t channels, uint32_t rate) noexcept
{
    if (!data || channels == 0 || channels > static_cast<uint32_t>(kMaxChannels)) return;
    std::array<float, kMaxChannels> peak{};
    std::array<double, kMaxChannels> squares{};
    for (size_t frame = 0; frame < frames; ++frame)
        for (size_t channel = 0; channel < channels; ++channel) {
            const float sample = ToFloat(data[frame * channels + channel]);
            peak[channel] = std::max(peak[channel], std::abs(sample));
            squares[channel] += static_cast<double>(sample) * sample;
        }
    for (size_t channel = 0; channel < channels; ++channel) {
        tap.peaks[channel].store(peak[channel], std::memory_order_relaxed);
        tap.rms[channel].store(frames == 0 ? 0.0f
                                           : static_cast<float>(std::sqrt(squares[channel] / static_cast<double>(frames))),
                               std::memory_order_relaxed);
    }
    tap.meterRate.store(rate, std::memory_order_relaxed);
    tap.meterChannels.store(static_cast<uint8_t>(channels), std::memory_order_release);
    tap.meterFrames.fetch_add(frames, std::memory_order_relaxed);
}

template <typename Sample>
void CaptureSamples(const std::shared_ptr<CaptureTap>& tap, QSpan<const Sample> samples) noexcept
{
    if (!tap || !tap->running.load(std::memory_order_acquire)) return;
    const size_t channels = tap->channels.load(std::memory_order_acquire);
    if (channels == 0) return;
    const size_t frameCount = samples.size() / channels;
    uint64_t write = tap->written.load(std::memory_order_relaxed);
    const uint64_t read = tap->read.load(std::memory_order_acquire);
    const size_t available = kRingFrames - static_cast<size_t>(std::min<uint64_t>(
        kRingFrames, write - read));
    const size_t toWrite = std::min(frameCount, available);
    std::array<float, kMaxChannels> peak{};
    std::array<double, kMaxChannels> squares{};
    for (size_t frame = 0; frame < frameCount; ++frame) {
        for (size_t channel = 0; channel < channels; ++channel) {
            const float sample = ToFloat(samples[frame * channels + channel]);
            const float magnitude = std::abs(sample);
            peak[channel] = std::max(peak[channel], magnitude);
            squares[channel] += static_cast<double>(sample) * sample;
            if (frame < toWrite) {
                const size_t ringFrame = static_cast<size_t>((write + frame) % kRingFrames);
                tap->ring[ringFrame * channels + channel] = sample;
            }
        }
    }
    if (toWrite != 0) {
        tap->written.store(write + toWrite, std::memory_order_release);
    }
    for (size_t channel = 0; channel < channels; ++channel) {
        tap->peaks[channel].store(peak[channel], std::memory_order_relaxed);
        tap->rms[channel].store(frameCount == 0 ? 0.0f : static_cast<float>(
            std::sqrt(squares[channel] / static_cast<double>(frameCount))),
            std::memory_order_relaxed);
    }
    tap->meterChannels.store(static_cast<uint8_t>(channels), std::memory_order_release);
    tap->frames.fetch_add(frameCount, std::memory_order_relaxed);
}

QAudioFormat::SampleFormat SupportedSampleFormat(QAudioFormat::SampleFormat format)
{
    switch (format) {
    case QAudioFormat::Float:
    case QAudioFormat::UInt8:
    case QAudioFormat::Int16:
    case QAudioFormat::Int32: return format;
    default: return QAudioFormat::Unknown;
    }
}

} // namespace

struct QtAudioDevices::State {
    std::atomic<bool> programRunning{false};

    class Worker final : public QObject {
    public:
        explicit Worker(State* state) : state_(state) {}
        ~Worker() override { StopAll(); }

        Result<void> StartInput(uint32_t id, const QAudioDevice& device, QAudioFormat format,
                                const std::shared_ptr<CaptureTap>& tap)
        {
            if (inputs_.contains(id)) return Ok();
            if (!device.isFormatSupported(format))
                return Error::Make(Err::Unsupported, "Audio",
                    "the selected input does not support its preferred PCM format");
            auto source = std::make_unique<QAudioSource>(device, format, this);
            QAudioSource* sourcePtr = source.get();
            QObject::connect(sourcePtr, &QAudioSource::stateChanged, sourcePtr,
                [tap, sourcePtr](QtAudio::State current) {
                    if (current == QtAudio::StoppedState && sourcePtr->error() != QtAudio::NoError)
                        tap->running.store(false, std::memory_order_release);
                });
            source->setBufferFrameCount(2048);
            switch (format.sampleFormat()) {
            case QAudioFormat::Float:
                source->start([tap](QSpan<const float> data) { CaptureSamples(tap, data); });
                break;
            case QAudioFormat::Int16:
                source->start([tap](QSpan<const int16_t> data) { CaptureSamples(tap, data); });
                break;
            case QAudioFormat::Int32:
                source->start([tap](QSpan<const int32_t> data) { CaptureSamples(tap, data); });
                break;
            case QAudioFormat::UInt8:
                source->start([tap](QSpan<const uint8_t> data) { CaptureSamples(tap, data); });
                break;
            default:
                return Error::Make(Err::Unsupported, "Audio", "unsupported input sample format");
            }
            if (source->error() != QtAudio::NoError) {
                const auto error = source->error();
                source->stop();
                return Error::Make(Err::InvalidState, "Audio",
                    "Qt could not start audio input (error " + std::to_string(static_cast<int>(error)) + ")");
            }
            inputs_.emplace(id, Input{std::move(source), tap});
            return Ok();
        }

        Result<void> StopInput(uint32_t id)
        {
            const auto it = inputs_.find(id);
            if (it == inputs_.end()) return Ok();
            it->second.source->stop();
            it->second.tap->running.store(false, std::memory_order_release);
            inputs_.erase(it);
            return Ok();
        }

        Result<void> StartMediaStream(uint32_t id, const std::string& source,
                                      const std::shared_ptr<CaptureTap>& tap)
        {
            const auto existing = mediaStreams_.find(id);
            if (existing != mediaStreams_.end()) (void)StopMediaStream(id);
            auto player = std::make_unique<QMediaPlayer>(this);
            if (!player->isAvailable())
                return Error::Make(Err::Unsupported, "Audio", "Qt Multimedia playback is unavailable");

            QAudioFormat format;
            format.setSampleRate(48000);
            format.setChannelCount(2);
            format.setSampleFormat(QAudioFormat::Float);
            tap->rate.store(48000, std::memory_order_relaxed);
            tap->channels.store(2, std::memory_order_release);

            auto* bufferOutput = new QAudioBufferOutput(format, player.get());
            player->setAudioBufferOutput(bufferOutput);
            player->setAudioOutput(nullptr);
            QObject::connect(bufferOutput, &QAudioBufferOutput::audioBufferReceived,
                bufferOutput, [tap](const QAudioBuffer& buffer) {
                    if (!buffer.isValid() || buffer.sampleCount() <= 0) return;
                    const QAudioFormat received = buffer.format();
                    if (received.sampleFormat() != QAudioFormat::Float
                        || received.channelCount() != 2 || received.sampleRate() != 48000) {
                        tap->running.store(false, std::memory_order_release);
                        return;
                    }
                    const float* data = buffer.constData<float>();
                    if (data)
                        CaptureSamples(tap, QSpan<const float>(data, buffer.sampleCount()));
                });
            QObject::connect(player.get(), &QMediaPlayer::errorOccurred, player.get(),
                [tap](QMediaPlayer::Error, const QString&) {
                    tap->running.store(false, std::memory_order_release);
                });
            player->setLoops(QMediaPlayer::Infinite);
            player->setSource(QUrl::fromLocalFile(QString::fromStdString(source)));
            player->play();
            mediaStreams_.emplace(id, MediaStream{std::move(player), tap});
            return Ok();
        }

        Result<void> StopMediaStream(uint32_t id)
        {
            const auto it = mediaStreams_.find(id);
            if (it == mediaStreams_.end()) return Ok();
            it->second.tap->running.store(false, std::memory_order_release);
            it->second.player->stop();
            mediaStreams_.erase(it);
            return Ok();
        }

        Result<void> StartOutput(uint64_t id, const QAudioDevice& device,
                                 IAudio::RenderCallback callback, uint32_t deviceId)
        {
            const auto existing = outputs_.find(id);
            if (existing != outputs_.end()) {
                if (existing->second.tap->running.load(std::memory_order_acquire)
                    && existing->second.tap->deviceId == deviceId)
                    return Ok();
                (void)StopOutput(id);
            }
            QAudioFormat format = device.preferredFormat();
            if (format.sampleRate() <= 0 || format.channelCount() <= 0)
                return Error::Make(Err::Unsupported, "Audio", "output device has no usable sample rate");
            if (format.channelCount() > kMaxChannels) {
                format.setChannelCount(2);
                if (!device.isFormatSupported(format)) format.setChannelCount(1);
            }
            if (!device.isFormatSupported(format))
                return Error::Make(Err::Unsupported, "Audio",
                    "the selected output does not support a usable PCM format");
            const auto sampleFormat = SupportedSampleFormat(format.sampleFormat());
            if (sampleFormat == QAudioFormat::Unknown)
                return Error::Make(Err::Unsupported, "Audio", "unsupported output sample format");

            auto tap = std::make_shared<RenderTap>();
            tap->callback = std::move(callback);
            tap->deviceId = deviceId;
            tap->scratch.resize(32'768u * static_cast<size_t>(format.channelCount()));
            tap->running.store(true, std::memory_order_release);
            auto sink = std::make_unique<QAudioSink>(device, format, this);
            QAudioSink* sinkPtr = sink.get();
            QObject::connect(sinkPtr, &QAudioSink::stateChanged, sinkPtr,
                [tap, sinkPtr, id, state = state_](QtAudio::State current) {
                    if (current == QtAudio::StoppedState && sinkPtr->error() != QtAudio::NoError) {
                        tap->running.store(false, std::memory_order_release);
                        if (id == kProgramStreamId)
                            state->programRunning.store(false, std::memory_order_release);
                    }
                });
            sink->setBufferFrameCount(2048);
            const uint32_t channels = static_cast<uint32_t>(format.channelCount());
            const uint32_t rate = static_cast<uint32_t>(format.sampleRate());
            switch (sampleFormat) {
            case QAudioFormat::Float:
                sink->start([tap, channels, rate](QSpan<float> data) {
                    const size_t count = data.size() / channels;
                    if (data.size() % channels) std::fill(data.end() - (data.size() % channels), data.end(), 0.0f);
                    bool ok = false;
                    try { ok = tap->running.load(std::memory_order_acquire) && tap->callback
                                   && tap->callback(data.data(), static_cast<uint32_t>(count), channels, rate); }
                    catch (...) { ok = false; }
                    if (!ok) std::fill(data.begin(), data.end(), 0.0f);
                    MeterRender(*tap, data.data(), count, channels, rate);
                });
                break;
            case QAudioFormat::Int16:
                sink->start([tap, channels, rate](QSpan<int16_t> data) {
                    ConvertOutput(tap, data, channels, rate);
                });
                break;
            case QAudioFormat::Int32:
                sink->start([tap, channels, rate](QSpan<int32_t> data) {
                    ConvertOutput(tap, data, channels, rate);
                });
                break;
            case QAudioFormat::UInt8:
                sink->start([tap, channels, rate](QSpan<uint8_t> data) {
                    ConvertOutput(tap, data, channels, rate);
                });
                break;
            default:
                return Error::Make(Err::Unsupported, "Audio", "unsupported output sample format");
            }
            if (sink->error() != QtAudio::NoError) {
                const auto error = sink->error();
                tap->running.store(false, std::memory_order_release);
                sink->stop();
                return Error::Make(Err::InvalidState, "Audio",
                    "Qt could not start audio output (error " + std::to_string(static_cast<int>(error)) + ")");
            }
            outputs_.emplace(id, Output{std::move(sink), tap});
            {
                const std::lock_guard<std::mutex> lock(state_->mutex);
                state_->renderTaps[id] = tap;   // OutputLevels reads the meters from any thread
            }
            if (id == kProgramStreamId) state_->programRunning.store(true, std::memory_order_release);
            return Ok();
        }

        Result<void> StopOutput(uint64_t id)
        {
            const auto it = outputs_.find(id);
            if (it == outputs_.end()) return Ok();
            it->second.tap->running.store(false, std::memory_order_release);
            it->second.sink->stop();
            outputs_.erase(it);
            {
                const std::lock_guard<std::mutex> lock(state_->mutex);
                state_->renderTaps.erase(id);
            }
            if (id == kProgramStreamId) state_->programRunning.store(false, std::memory_order_release);
            return Ok();
        }

        void StopAll()
        {
            while (!inputs_.empty()) (void)StopInput(inputs_.begin()->first);
            while (!mediaStreams_.empty()) (void)StopMediaStream(mediaStreams_.begin()->first);
            while (!outputs_.empty()) (void)StopOutput(outputs_.begin()->first);
        }

    private:
        struct Input { std::unique_ptr<QAudioSource> source; std::shared_ptr<CaptureTap> tap; };
        struct Output { std::unique_ptr<QAudioSink> sink; std::shared_ptr<RenderTap> tap; };
        struct MediaStream { std::unique_ptr<QMediaPlayer> player; std::shared_ptr<CaptureTap> tap; };

        template <typename Sample>
        static void ConvertOutput(const std::shared_ptr<RenderTap>& tap, QSpan<Sample> data,
                                  uint32_t channels, uint32_t rate) noexcept
        {
            const size_t samples = data.size();
            const size_t frames = channels == 0 ? 0 : samples / channels;
            const size_t required = frames * channels;
            if (required > tap->scratch.size()) {
                std::fill(data.begin(), data.end(), Silence<Sample>());
                return;
            }
            bool ok = false;
            try { ok = tap->running.load(std::memory_order_acquire) && tap->callback
                           && tap->callback(tap->scratch.data(), static_cast<uint32_t>(frames), channels, rate); }
            catch (...) { ok = false; }
            if (!ok) std::fill(tap->scratch.begin(), tap->scratch.begin() + required, 0.0f);
            MeterRender(*tap, tap->scratch.data(), frames, channels, rate);
            for (size_t i = 0; i < required; ++i) data[i] = FromFloat<Sample>(tap->scratch[i]);
            std::fill(data.begin() + required, data.end(), Silence<Sample>());
        }

        State* state_;
        std::map<uint32_t, Input> inputs_;
        std::map<uint32_t, MediaStream> mediaStreams_;
        std::map<uint64_t, Output> outputs_;
    };

    State()
    {
        worker = new Worker(this);
        worker->moveToThread(&thread);
        thread.start();
    }

    ~State()
    {
        if (worker) {
            Worker* dying = worker;
            (void)Run([dying] { dying->StopAll(); delete dying; return Ok(); });
            worker = nullptr;
            thread.quit();
            thread.wait();
        }
    }

    template <typename Fn>
    auto Run(Fn&& fn) const -> decltype(fn())
    {
        using Return = decltype(fn());
        if (QThread::currentThread() == &thread) return fn();
        std::optional<Return> result;
        const bool queued = QMetaObject::invokeMethod(worker,
            [&] { result.emplace(fn()); }, Qt::BlockingQueuedConnection);
        if (!queued)
            return Error::Make(Err::InvalidState, "Audio", "audio worker is unavailable");
        if (!result)
            return Error::Make(Err::InvalidState, "Audio", "audio worker returned no result");
        return std::move(*result);
    }

    QThread thread;
    Worker* worker = nullptr;
    std::mutex controlMutex;
    mutable std::mutex mutex;
    std::map<uint32_t, std::shared_ptr<CaptureTap>> inputs;
    std::map<uint32_t, std::shared_ptr<CaptureTap>> streams;
    std::map<uint32_t, std::pair<std::string, std::string>> streamSpecs;
    std::map<uint64_t, std::shared_ptr<RenderTap>> renderTaps;   // guarded by `mutex`; the output meters
};

namespace {
Result<QAudioDevice> FindDevice(bool input, uint32_t requested, const std::string& name = {})
{
    const auto devices = input ? QMediaDevices::audioInputs() : QMediaDevices::audioOutputs();
    if (requested == UINT32_MAX) {
        const QAudioDevice device = input ? QMediaDevices::defaultAudioInput()
                                          : QMediaDevices::defaultAudioOutput();
        if (device.isNull())
            return Error::Make(Err::NotFound, "Audio", input
                ? "no default audio input device is available"
                : "no default audio output device is available");
        return device;
    }
    QAudioDevice match;
    for (const QAudioDevice& device : devices) {
        const uint32_t number = IAudio::StableDeviceNumber(device.id().toStdString());
        if (number != requested) continue;
        if (!name.empty() && device.description().toStdString() != name) continue;
        if (!match.isNull())
            return Error::Make(Err::InvalidArgument, "Audio", "audio device id hash collision");
        match = device;
    }
    if (match.isNull())
        return Error::Make(Err::NotFound, "Audio", "selected audio device is no longer available");
    return match;
}
} // namespace

QtAudioDevices::QtAudioDevices() : state_(std::make_shared<State>()) {}
QtAudioDevices::~QtAudioDevices() = default;

Result<void> QtAudioDevices::StartInputMeter(uint32_t deviceId, const std::string& deviceName)
{
    const std::lock_guard<std::mutex> controlLock(state_->controlMutex);
    bool replaceStoppedTap = false;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        const auto existing = state_->inputs.find(deviceId);
        if (existing != state_->inputs.end()) {
            if (existing->second->running.load(std::memory_order_acquire)) return Ok();
            state_->inputs.erase(existing);
            replaceStoppedTap = true;
        }
    }
    if (replaceStoppedTap)
        (void)state_->Run([&] { return state_->worker->StopInput(deviceId); });
    auto resolved = FindDevice(true, deviceId, deviceName);
    if (!resolved.ok()) return resolved.error();
    const QAudioDevice device = resolved.value();
    QAudioFormat format = device.preferredFormat();
    if (format.channelCount() <= 0 || format.channelCount() > kMaxChannels
        || format.sampleRate() <= 0 || SupportedSampleFormat(format.sampleFormat()) == QAudioFormat::Unknown)
        return Error::Make(Err::Unsupported, "Audio", "input device has no supported PCM mix format");
    auto tap = std::make_shared<CaptureTap>();
            tap->channels.store(static_cast<uint32_t>(format.channelCount()), std::memory_order_relaxed);
            tap->rate.store(static_cast<uint32_t>(format.sampleRate()), std::memory_order_relaxed);
    auto started = state_->Run([&] { return state_->worker->StartInput(deviceId, device, format, tap); });
    if (!started.ok()) return started;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->inputs[deviceId] = std::move(tap);
    return Ok();
}

Result<void> QtAudioDevices::StopInputMeter(uint32_t deviceId)
{
    const std::lock_guard<std::mutex> controlLock(state_->controlMutex);
    auto stopped = state_->Run([&] { return state_->worker->StopInput(deviceId); });
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->inputs.erase(deviceId);
    return stopped;
}

IAudio::InputMeterLevels QtAudioDevices::InputLevels(uint32_t deviceId)
{
    std::shared_ptr<CaptureTap> tap;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        auto& table = IAudio::IsStreamTap(deviceId) ? state_->streams : state_->inputs;
        const auto it = table.find(deviceId);
        if (it == table.end()) return {};
        tap = it->second;
    }
    InputMeterLevels levels;
    levels.deviceId = deviceId;
    if (!tap->running.load(std::memory_order_acquire)) return levels;
    levels.channelCount = tap->meterChannels.load(std::memory_order_acquire);
    levels.sampleRateHz = tap->rate.load(std::memory_order_acquire);
    levels.framesCaptured = tap->frames.load(std::memory_order_relaxed);
    levels.layout = InputMeterLevels::LayoutFor(levels.channelCount);
    for (size_t i = 0; i < levels.channelCount; ++i) {
        levels.peaks[i] = tap->peaks[i].load(std::memory_order_relaxed);
        levels.rms[i] = tap->rms[i].load(std::memory_order_relaxed);
    }
    return levels;
}

std::vector<uint32_t> QtAudioDevices::ActiveInputMeters() const
{
    std::vector<uint32_t> ids;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    for (const auto& [id, tap] : state_->inputs)
        if (tap && tap->running.load(std::memory_order_acquire)) ids.push_back(id);
    return ids;
}

IAudio::TapAudio QtAudioDevices::TapFormat(uint32_t deviceId)
{
    std::shared_ptr<CaptureTap> tap;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        auto& table = IAudio::IsStreamTap(deviceId) ? state_->streams : state_->inputs;
        const auto it = table.find(deviceId);
        if (it == table.end()) return {};
        tap = it->second;
    }
    return tap->running.load(std::memory_order_acquire)
        ? TapAudio{tap->channels.load(std::memory_order_acquire),
                   tap->rate.load(std::memory_order_acquire)} : TapAudio{};
}

size_t QtAudioDevices::ReadTapAudio(uint32_t deviceId, float* dst, size_t frames)
{
    if (!dst || frames == 0) return 0;
    std::shared_ptr<CaptureTap> tap;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        auto& table = IAudio::IsStreamTap(deviceId) ? state_->streams : state_->inputs;
        const auto it = table.find(deviceId);
        if (it == table.end()) return 0;
        tap = it->second;
    }
    if (!tap->running.load(std::memory_order_acquire)) return 0;
    const uint32_t channels = tap->channels.load(std::memory_order_acquire);
    if (channels == 0) return 0;
    const uint64_t read = tap->read.load(std::memory_order_relaxed);
    const uint64_t written = tap->written.load(std::memory_order_acquire);
    const size_t count = std::min<size_t>(frames, static_cast<size_t>(written - read));
    for (size_t frame = 0; frame < count; ++frame) {
        const size_t ringFrame = static_cast<size_t>((read + frame) % kRingFrames);
        std::copy_n(tap->ring.data() + ringFrame * channels, channels,
                    dst + frame * channels);
    }
    tap->read.store(read + count, std::memory_order_release);
    return count;
}

Result<void> QtAudioDevices::StartStreamTap(uint32_t tapId, const std::string& source,
                                            const std::string& kind)
{
    if (!IAudio::IsStreamTap(tapId))
        return Error::Make(Err::InvalidArgument, "Audio", "a stream tap id must be in the stream range");
    if (source.empty())
        return Error::Make(Err::InvalidArgument, "Audio", "a stream tap needs a source");
    const bool media = kind == "media";
    const bool ndi = kind == "ndi";
    if (!media && !ndi)
        return Error::Make(Err::Unsupported, "Audio", "no stream tap producer for kind '" + kind + "'");
    if (media && !QFileInfo(QString::fromStdString(source)).isFile())
        return Error::Make(Err::NotFound, "Audio", "no media file at '" + source + "'");

    const std::lock_guard<std::mutex> controlLock(state_->controlMutex);
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        const auto spec = state_->streamSpecs.find(tapId);
        const auto tap = state_->streams.find(tapId);
        if (spec != state_->streamSpecs.end() && tap != state_->streams.end()
            && spec->second.first == source && spec->second.second == kind
            && tap->second->running.load(std::memory_order_acquire))
            return Ok();
        if (tap != state_->streams.end()) {
            tap->second->running.store(false, std::memory_order_release);
            state_->streams.erase(tap);
        }
        state_->streamSpecs.erase(tapId);
    }
    auto stopped = state_->Run([&] { return state_->worker->StopMediaStream(tapId); });
    if (!stopped.ok()) return stopped;

    auto tap = std::make_shared<CaptureTap>();
    if (media) {
        tap->channels.store(2, std::memory_order_relaxed);
        tap->rate.store(48000, std::memory_order_relaxed);
        auto started = state_->Run([&] { return state_->worker->StartMediaStream(tapId, source, tap); });
        if (!started.ok()) return started;
    }
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->streams[tapId] = tap;
        state_->streamSpecs[tapId] = {source, kind};
    }
    return Ok();
}

Result<void> QtAudioDevices::StopStreamTap(uint32_t tapId)
{
    if (!IAudio::IsStreamTap(tapId))
        return Error::Make(Err::InvalidArgument, "Audio", "not a stream tap id");
    const std::lock_guard<std::mutex> controlLock(state_->controlMutex);
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        const auto it = state_->streams.find(tapId);
        if (it != state_->streams.end()) {
            it->second->running.store(false, std::memory_order_release);
            state_->streams.erase(it);
        }
        state_->streamSpecs.erase(tapId);
    }
    return state_->Run([&] { return state_->worker->StopMediaStream(tapId); });
}

std::vector<uint32_t> QtAudioDevices::ActiveStreamTaps() const
{
    std::vector<uint32_t> ids;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    for (const auto& [id, tap] : state_->streams)
        if (tap && tap->running.load(std::memory_order_acquire)) ids.push_back(id);
    return ids;
}

Result<void> QtAudioDevices::PushStreamTapAudio(uint32_t tapId, const float* interleaved,
                                               uint32_t channels, uint32_t frames, uint32_t rate)
{
    if (!IAudio::IsStreamTap(tapId))
        return Error::Make(Err::InvalidArgument, "Audio", "not a stream tap id");
    std::shared_ptr<CaptureTap> tap;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        const auto it = state_->streams.find(tapId);
        if (it == state_->streams.end()) return Ok();
        tap = it->second;
    }
    if (!interleaved || frames == 0 || rate == 0 || channels == 0 || channels > kMaxChannels)
        return Error::Make(Err::InvalidArgument, "Audio", "malformed stream tap block");
    if (!tap->running.load(std::memory_order_acquire)) return Ok();
    const std::lock_guard<std::mutex> producerLock(tap->producerMutex);
    if (!tap->running.load(std::memory_order_acquire)) return Ok();
    uint32_t currentChannels = tap->channels.load(std::memory_order_acquire);
    if (currentChannels == 0) {
        tap->rate.store(rate, std::memory_order_relaxed);
        tap->channels.store(channels, std::memory_order_release);
        currentChannels = channels;
    }
    if (currentChannels != channels || tap->rate.load(std::memory_order_acquire) != rate)
        return Error::Make(Err::InvalidArgument, "Audio", "stream tap format changed while active");
    const size_t sampleCount = static_cast<size_t>(channels) * frames;
    CaptureSamples(tap, QSpan<const float>(interleaved, static_cast<qsizetype>(sampleCount)));
    return Ok();
}

Result<void> QtAudioDevices::StartRender(RenderCallback callback, uint32_t deviceId)
{
    if (!callback)
        return Error::Make(Err::InvalidArgument, "Audio", "render callback is empty");
    auto device = FindDevice(false, deviceId);
    if (!device.ok()) return device.error();
    return state_->Run([&] { return state_->worker->StartOutput(
        kProgramStreamId, device.value(), std::move(callback), deviceId); });
}

Result<void> QtAudioDevices::StopRender()
{
    return state_->Run([&] { return state_->worker->StopOutput(kProgramStreamId); });
}

bool QtAudioDevices::Rendering() const
{
    return state_->programRunning.load(std::memory_order_acquire);
}

Result<void> QtAudioDevices::StartRenderStream(uint64_t streamId, RenderCallback callback,
                                               uint32_t deviceId)
{
    if (!callback)
        return Error::Make(Err::InvalidArgument, "Audio", "render callback is empty");
    if (streamId == kProgramStreamId)
        return Error::Make(Err::InvalidArgument, "Audio", "reserved output stream id");
    auto device = FindDevice(false, deviceId);
    if (!device.ok()) return device.error();
    return state_->Run([&] { return state_->worker->StartOutput(
        streamId, device.value(), std::move(callback), deviceId); });
}

Result<void> QtAudioDevices::StopRenderStream(uint64_t streamId)
{
    if (streamId == kProgramStreamId)
        return Error::Make(Err::InvalidArgument, "Audio", "reserved output stream id");
    return state_->Run([&] { return state_->worker->StopOutput(streamId); });
}

// ---- OUTPUT metering: the level of what this app plays --------------------------------------------
// The meter lives in the render stream itself (see RenderTap), so there is nothing to start: it reads
// zero ("no signal") until a stream is rendering to the device, exactly like the Windows loopback tap
// that was not running. Start/Stop only validate the request and stay Ok so callers need no OS branch.
Result<void> QtAudioDevices::StartOutputMeter(uint32_t deviceId)
{
    if (deviceId != UINT32_MAX) {
        auto device = FindDevice(false, deviceId);
        if (!device.ok()) return device.error();
    }
    return Ok();
}

Result<void> QtAudioDevices::StopOutputMeter(uint32_t) { return Ok(); }

IAudio::InputMeterLevels QtAudioDevices::OutputLevels(uint32_t deviceId)
{
    std::shared_ptr<RenderTap> tap;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        // The program stream is the one the user hears; a device id asks for whichever stream renders to it.
        if (deviceId == UINT32_MAX) {
            const auto program = state_->renderTaps.find(kProgramStreamId);
            if (program != state_->renderTaps.end()) tap = program->second;
        }
        if (!tap)
            for (const auto& [id, candidate] : state_->renderTaps)
                if (candidate->deviceId == deviceId && candidate->running.load(std::memory_order_acquire)) {
                    tap = candidate;
                    break;
                }
    }
    InputMeterLevels levels;
    if (!tap) return levels;
    const uint8_t channels = tap->meterChannels.load(std::memory_order_acquire);
    if (channels == 0) return levels;
    levels.deviceId = deviceId;
    levels.channelCount = channels;
    levels.sampleRateHz = tap->meterRate.load(std::memory_order_relaxed);
    levels.framesCaptured = tap->meterFrames.load(std::memory_order_relaxed);
    levels.layout = InputMeterLevels::LayoutFor(channels);
    for (int i = 0; i < channels && i < kMaxChannels; ++i) {
        levels.peaks[i] = tap->peaks[static_cast<size_t>(i)].load(std::memory_order_relaxed);
        levels.rms[i] = tap->rms[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    }
    return levels;
}

} // namespace bps::platform
