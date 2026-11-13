// QtRecordingContainer — recording to a file through Qt Multimedia, on every OS.
//
// WHY. The recording ENGINE is portable — frames come from the render engine's on-air buffer, audio from
// the mixer's tap — so the only piece that could differ per OS is the container that turns those into a
// file. That is IRecordingContainer, and this is its ONE implementation, on Windows, Linux and macOS alike:
// a take recorded on one machine is the same kind of file as on any other. (Windows used to have a separate
// Media Foundation writer; it wrote files that would not play and has been removed.)
//
// HOW. QMediaRecorder writes the file; frames and audio are pushed in with QVideoFrameInput and
// QAudioBufferInput (Qt 6.8+) attached to a QMediaCaptureSession. Qt's FFmpeg backend does the encoding
// — hardware encoders (VideoToolbox, VAAPI, NVENC ...) where the machine has one.
//
//   container   MP4 -> MPEG-4, MKV -> Matroska, MOV -> QuickTime, WebM, M4A -> MPEG-4 audio, MP3, FLAC
//   video       the profile's codec (H.264/HEVC/AV1/VP9) when the backend can encode that container with it,
//               else the backend's default for the container
//   audio       the profile's codec likewise; always 48 kHz float on the way in (a block at another rate
//               or channel count is converted first)
//
// THREADS. The Qt objects live on a private QThread with an event loop (QMediaRecorder and the inputs
// are event-driven and thread-affine). The recording pump thread calls WriteVideo/WriteAudio, which
// copy the samples, queue them to that thread and return at once; the thread forwards them whenever
// the encoder says it is ready for more (readyToSend*), so a slow encoder drops frames (counted and
// reported through the error path only if it never recovers) instead of stalling the pump or growing
// memory. Close() asks the recorder to stop and WAITS for the file to be finalized.
//
// No Q_OBJECT here (this file is not run through moc): everything is functor connections and
// queued invocations on a context QObject.

#include "modules/recording/QtRecordingContainer.hpp"

#include "platform/qt/QtRecording.hpp"

#include <QAudioBuffer>
#include <QAudioBufferInput>
#include <QAudioFormat>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QMetaObject>
#include <QObject>
#include <QPainter>
#include <QSize>
#include <QThread>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoFrameInput>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bps::recording {
namespace {

constexpr const char* kModule = "QtRecording";
constexpr int kOpenTimeoutMs = 10'000;       // the backend must report "recording" by then
constexpr int kDrainTimeoutMs = 20'000;      // queued pictures / sound the encoder still has to take before the stop
constexpr int kStopTimeoutMs = 30'000;       // a long take can take a while to finalize
constexpr int kMaxQueuedVideo = 8;           // frames waiting for the encoder (more are dropped)
constexpr int kMaxQueuedAudio = 512;         // blocks waiting for the encoder
constexpr uint32_t kRecordRate = 48'000;     // the audio sample rate files are written at

bool IsAudioOnly(ContainerKind k) {
    return k == ContainerKind::WAV || k == ContainerKind::FLAC || k == ContainerKind::M4A || k == ContainerKind::MP3;
}

QMediaFormat::FileFormat FileFormatFor(ContainerKind k) {
    switch (k) {
    case ContainerKind::MP4: return QMediaFormat::MPEG4;
    case ContainerKind::MKV: return QMediaFormat::Matroska;
    case ContainerKind::MOV: return QMediaFormat::QuickTime;
    case ContainerKind::WebM: return QMediaFormat::WebM;
    case ContainerKind::WAV: return QMediaFormat::Wave;
    case ContainerKind::FLAC: return QMediaFormat::FLAC;
    case ContainerKind::M4A: return QMediaFormat::Mpeg4Audio;
    case ContainerKind::MP3: return QMediaFormat::MP3;
    }
    return QMediaFormat::UnspecifiedFormat;
}

QMediaFormat::VideoCodec VideoCodecFor(VideoCodec c) {
    switch (c) {
    case VideoCodec::H264: return QMediaFormat::VideoCodec::H264;
    case VideoCodec::HEVC: return QMediaFormat::VideoCodec::H265;
    case VideoCodec::AV1: return QMediaFormat::VideoCodec::AV1;
    case VideoCodec::VP9: return QMediaFormat::VideoCodec::VP9;
    case VideoCodec::None: break;
    }
    return QMediaFormat::VideoCodec::Unspecified;
}

QMediaFormat::AudioCodec AudioCodecFor(AudioCodec c) {
    switch (c) {
    case AudioCodec::AAC: return QMediaFormat::AudioCodec::AAC;
    case AudioCodec::Opus: return QMediaFormat::AudioCodec::Opus;
    case AudioCodec::FLAC: return QMediaFormat::AudioCodec::FLAC;
    case AudioCodec::MP3: return QMediaFormat::AudioCodec::MP3;
    case AudioCodec::PCM: return QMediaFormat::AudioCodec::Wave;
    case AudioCodec::None: break;
    }
    return QMediaFormat::AudioCodec::Unspecified;
}

// ---------------------------------------------------------------------------------------------
// What this machine's backend can encode (probed once there is a Qt application to ask).
// ---------------------------------------------------------------------------------------------
struct Support {
    std::vector<ContainerKind> containers;
    bool video = false;
};

Support ProbeSupport() {
    Support out;
    QMediaFormat probe;
    const auto formats = probe.supportedFileFormats(QMediaFormat::Encode);
    const auto videoCodecs = probe.supportedVideoCodecs(QMediaFormat::Encode);
    const auto audioCodecs = probe.supportedAudioCodecs(QMediaFormat::Encode);
    const auto hasFormat = [&](QMediaFormat::FileFormat f) { return formats.contains(f); };
    const auto hasVideo = [&](QMediaFormat::VideoCodec c) { return videoCodecs.contains(c); };
    const auto hasAudio = [&](QMediaFormat::AudioCodec c) { return audioCodecs.contains(c); };

    const bool h264 = hasVideo(QMediaFormat::VideoCodec::H264);
    const bool vp = hasVideo(QMediaFormat::VideoCodec::VP9) || hasVideo(QMediaFormat::VideoCodec::VP8);
    auto add = [&](ContainerKind k, bool ok) { if (ok) out.containers.push_back(k); };
    add(ContainerKind::MP4, hasFormat(QMediaFormat::MPEG4) && h264);
    add(ContainerKind::MKV, hasFormat(QMediaFormat::Matroska) && (h264 || vp));
    add(ContainerKind::MOV, hasFormat(QMediaFormat::QuickTime) && h264);
    add(ContainerKind::WebM, hasFormat(QMediaFormat::WebM) && vp);
    add(ContainerKind::M4A, hasFormat(QMediaFormat::Mpeg4Audio) && hasAudio(QMediaFormat::AudioCodec::AAC));
    add(ContainerKind::MP3, hasFormat(QMediaFormat::MP3) && hasAudio(QMediaFormat::AudioCodec::MP3));
    add(ContainerKind::FLAC, hasFormat(QMediaFormat::FLAC) && hasAudio(QMediaFormat::AudioCodec::FLAC));
    out.video = h264 || vp;
    return out;
}

// Cached once a Qt application exists; before that every caller gets an empty answer that is NOT remembered.
const Support& CachedSupport() {
    static std::mutex mutex;
    static std::unique_ptr<Support> cache;
    static const Support empty;
    const std::lock_guard<std::mutex> lock(mutex);
    if (cache) return *cache;
    if (!QCoreApplication::instance()) return empty;
    cache = std::make_unique<Support>(ProbeSupport());
    return *cache;
}

// ---------------------------------------------------------------------------------------------
// Audio: the block the engine hands over -> 48 kHz float at the file's channel count.
// ---------------------------------------------------------------------------------------------
std::vector<float> ConvertAudio(const AudioBlockIn& in, uint32_t dstRate, uint32_t dstChannels) {
    std::vector<float> out;
    if (!in.samples || in.frames == 0 || in.channels == 0 || in.rateHz == 0 || dstChannels == 0) return out;
    const size_t srcFrames = in.frames;
    // 1. channels: same layout copies; to mono averages; from mono duplicates; otherwise the leading
    //    channels map one to one and any extra output channel stays silent.
    std::vector<float> mapped;
    const float* base = in.samples;
    if (in.channels != dstChannels) {
        mapped.resize(srcFrames * dstChannels);
        for (size_t f = 0; f < srcFrames; ++f) {
            const float* s = in.samples + f * in.channels;
            float* d = mapped.data() + f * dstChannels;
            if (dstChannels == 1) {
                float sum = 0.0f;
                for (uint32_t c = 0; c < in.channels; ++c) sum += s[c];
                d[0] = sum / static_cast<float>(in.channels);
            } else {
                for (uint32_t c = 0; c < dstChannels; ++c)
                    d[c] = in.channels == 1 ? s[0] : (c < in.channels ? s[c] : 0.0f);
            }
        }
        base = mapped.data();
    }
    // 2. rate: linear interpolation (the engine mixes at the device rate, normally 48 kHz already).
    if (in.rateHz == dstRate) {
        out.assign(base, base + srcFrames * dstChannels);
        return out;
    }
    const size_t dstFrames = std::max<size_t>(1, static_cast<size_t>(
        std::llround(static_cast<double>(srcFrames) * dstRate / in.rateHz)));
    out.resize(dstFrames * dstChannels);
    const double step = static_cast<double>(in.rateHz) / dstRate;
    for (size_t f = 0; f < dstFrames; ++f) {
        const double pos = static_cast<double>(f) * step;
        const size_t i0 = std::min(srcFrames - 1, static_cast<size_t>(pos));
        const size_t i1 = std::min(srcFrames - 1, i0 + 1);
        const float t = static_cast<float>(pos - static_cast<double>(i0));
        for (uint32_t c = 0; c < dstChannels; ++c)
            out[f * dstChannels + c] = base[i0 * dstChannels + c] * (1.0f - t) + base[i1 * dstChannels + c] * t;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// The container.
// ---------------------------------------------------------------------------------------------
class QtRecordingContainer final : public IRecordingContainer {
public:
    explicit QtRecordingContainer(ContainerKind kind) : kind_(kind) {}
    ~QtRecordingContainer() override { (void)Close(false); }

    const char* Name() const noexcept override {
        switch (kind_) {
        case ContainerKind::MP4: return "mp4";
        case ContainerKind::MKV: return "mkv";
        case ContainerKind::MOV: return "mov";
        case ContainerKind::WebM: return "webm";
        case ContainerKind::WAV: return "wav";
        case ContainerKind::FLAC: return "flac";
        case ContainerKind::M4A: return "m4a";
        case ContainerKind::MP3: return "mp3";
        }
        return "?";
    }

    Result<std::string> Open(const RecordingProfile& profile, std::string_view path) override {
        if (thread_) return Error::Make(Err::Recording_InvalidState, kModule, "container is already open");
        if (!QCoreApplication::instance())
            return Error::Make(Err::Unsupported, kModule, "no Qt application is running to host the media encoder");

        audioOnly_ = IsAudioOnly(kind_);
        width_ = std::max(2, profile.width & ~1);      // encoders want even dimensions
        height_ = std::max(2, profile.height & ~1);
        fps_ = profile.fps > 0.0 ? profile.fps : 30.0;
        channels_ = static_cast<uint32_t>(std::clamp(profile.audioChannels, 1, 8));
        shared_ = std::make_shared<Shared>();
        // An ABSOLUTE path: the backend resolves a relative URL against its own idea of the working directory
        // (and the engine hands over "recordings/<name>.mp4"), which on Windows ends as "cannot open the output
        // location for writing".
        const QFileInfo target(QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size())));
        path_ = target.absoluteFilePath().toStdString();
        QDir().mkpath(target.absolutePath());

        // The file format and codecs, with a fallback to the backend's own defaults for the container.
        QMediaFormat format(FileFormatFor(kind_));
        if (!audioOnly_) format.setVideoCodec(VideoCodecFor(profile.videoCodec));
        format.setAudioCodec(AudioCodecFor(profile.audioCodec));
        if (!format.isSupported(QMediaFormat::Encode)) {
            format = QMediaFormat(FileFormatFor(kind_));
            if (!format.isSupported(QMediaFormat::Encode))
                return Error::Make(Err::Unsupported, kModule,
                                   std::string("this machine's Qt media backend cannot write ") + Name());
        }

        thread_ = std::make_unique<QThread>();
        context_ = new QObject;
        context_->moveToThread(thread_.get());
        QObject::connect(thread_.get(), &QThread::finished, context_, &QObject::deleteLater);
        thread_->start();

        std::string failure;
        const bool invoked = QMetaObject::invokeMethod(context_, [&] { failure = BuildOnThread(profile, format); },
                                                       Qt::BlockingQueuedConnection);
        if (!invoked) failure = "the recording thread did not start";
        if (failure.empty()) {
            // record() is asynchronous: wait until the backend says it is recording, or says why not.
            std::unique_lock<std::mutex> lock(shared_->mutex);
            const bool settled = shared_->cv.wait_for(lock, std::chrono::milliseconds(kOpenTimeoutMs),
                                                      [&] { return shared_->recording || !shared_->error.empty(); });
            if (!settled) failure = "the media backend did not start recording in time";
            else if (!shared_->error.empty()) failure = shared_->error;
        }
        if (!failure.empty()) {
            Teardown(false);
            return Error::Make(Err::Recording_EncoderUnavailable, kModule, failure);
        }
        open_ = true;
        return path_;
    }

    Result<void> Close(bool) override {
        if (!thread_) return Ok();
        if (open_) {
            open_ = false;
            // Let the encoder take everything that is still queued BEFORE stopping: a slow encoder (the MP3 one) is
            // still working through a burst when the stop arrives, and a stop with buffers queued cuts the file short.
            // The recording thread keeps feeding it on every readyToSend signal while this waits.
            for (int waited = 0; waited < kDrainTimeoutMs
                 && (shared_->queuedVideo.load() > 0 || shared_->queuedAudio.load() > 0); waited += 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const std::lock_guard<std::mutex> lock(shared_->mutex);
                if (!shared_->error.empty()) break;
            }
            // Hand over what is still queued, then stop; the file is finalized when the state says stopped.
            (void)QMetaObject::invokeMethod(context_, [this] {
                PumpVideo();
                PumpAudio();
                if (worker_ && worker_->recorder) worker_->recorder->stop();
            }, Qt::BlockingQueuedConnection);
            std::unique_lock<std::mutex> lock(shared_->mutex);
            const bool stopped = shared_->cv.wait_for(lock, std::chrono::milliseconds(kStopTimeoutMs),
                                                      [&] { return shared_->stopped || !shared_->error.empty(); });
            const std::string error = shared_->error;
            lock.unlock();
            Teardown(true);
            if (!stopped) return Error::Make(Err::Recording_EncoderUnavailable, kModule, "the file was not finalized in time");
            if (!error.empty()) return Error::Make(Err::Recording_EncoderUnavailable, kModule, error);
            return Ok();
        }
        Teardown(false);
        return Ok();
    }

    Result<void> WriteVideo(const VideoFrameIn& in) override {
        if (!open_ || audioOnly_) return Ok();
        if (auto e = CheckHealthy(); !e.ok()) return e;
        if (!in.pixels || in.width <= 0 || in.height <= 0) return Ok();
        if (shared_->queuedVideo.load(std::memory_order_relaxed) >= kMaxQueuedVideo) {
            shared_->droppedVideo.fetch_add(1, std::memory_order_relaxed);   // the encoder is behind: skip this one
            return Ok();
        }
        const qint64 startUs = std::max<qint64>(0, static_cast<qint64>(in.timestampMs * 1000.0));
        QVideoFrame frame = MakeFrame(in, startUs);
        if (!frame.isValid()) return Ok();
        shared_->queuedVideo.fetch_add(1, std::memory_order_relaxed);
        (void)QMetaObject::invokeMethod(context_, [this, frame = std::move(frame)]() mutable {
            if (!worker_) return;
            worker_->videoQueue.push_back(std::move(frame));
            PumpVideo();
        }, Qt::QueuedConnection);
        return Ok();
    }

    Result<void> WriteAudio(const AudioBlockIn& in) override {
        if (!open_) return Ok();
        if (auto e = CheckHealthy(); !e.ok()) return e;
        if (shared_->queuedAudio.load(std::memory_order_relaxed) >= kMaxQueuedAudio) return Ok();
        std::vector<float> samples = ConvertAudio(in, kRecordRate, channels_);
        if (samples.empty()) return Ok();
        QAudioFormat format;
        format.setSampleRate(static_cast<int>(kRecordRate));
        format.setChannelCount(static_cast<int>(channels_));
        format.setSampleFormat(QAudioFormat::Float);
        const QByteArray bytes(reinterpret_cast<const char*>(samples.data()),
                               static_cast<qsizetype>(samples.size() * sizeof(float)));
        const qint64 startUs = std::max<qint64>(0, static_cast<qint64>(in.timestampMs * 1000.0));
        QAudioBuffer buffer(bytes, format, startUs);
        shared_->queuedAudio.fetch_add(1, std::memory_order_relaxed);
        (void)QMetaObject::invokeMethod(context_, [this, buffer = std::move(buffer)]() mutable {
            if (!worker_) return;
            worker_->audioQueue.push_back(std::move(buffer));
            PumpAudio();
        }, Qt::QueuedConnection);
        return Ok();
    }

private:
    // Cross-thread state: what the Qt thread reports and the pump thread reads.
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        bool recording = false;
        bool stopped = false;
        std::string error;
        std::atomic<int> queuedVideo{0};
        std::atomic<int> queuedAudio{0};
        std::atomic<uint64_t> droppedVideo{0};
    };

    // The Qt objects: created, used and destroyed ONLY on the recording thread.
    struct Worker {
        QMediaCaptureSession session;
        std::unique_ptr<QVideoFrameInput> videoIn;
        std::unique_ptr<QAudioBufferInput> audioIn;
        std::unique_ptr<QMediaRecorder> recorder;
        std::deque<QVideoFrame> videoQueue;
        std::deque<QAudioBuffer> audioQueue;
    };

    Result<void> CheckHealthy() {
        const std::lock_guard<std::mutex> lock(shared_->mutex);
        if (!shared_->error.empty())
            return Error::Make(Err::Recording_EncoderUnavailable, kModule, shared_->error);
        return Ok();
    }

    // Runs on the recording thread. Returns "" or why the recorder could not be built.
    std::string BuildOnThread(const RecordingProfile& profile, const QMediaFormat& format) {
        auto worker = std::make_unique<Worker>();
        std::shared_ptr<Shared> shared = shared_;

        // The documented order: recorder on the session first, then the inputs.
        worker->recorder = std::make_unique<QMediaRecorder>();
        worker->session.setRecorder(worker->recorder.get());

        if (!audioOnly_) {
            // Deliberately WITHOUT a format hint: with one, Qt 6.11's FFmpeg backend on Windows never signals
            // readyToSendVideoFrame and the file ends up with no picture track at all (261 bytes). Without it
            // the encoder is set up from the first frame, which works on every OS.
            worker->videoIn = std::make_unique<QVideoFrameInput>();
            worker->session.setVideoFrameInput(worker->videoIn.get());
            QObject::connect(worker->videoIn.get(), &QVideoFrameInput::readyToSendVideoFrame, context_,
                             [this] { PumpVideo(); });
        }
        // No format hint here either (see the picture input): the encoder is set up from the first buffer.
        worker->audioIn = std::make_unique<QAudioBufferInput>();
        worker->session.setAudioBufferInput(worker->audioIn.get());
        QObject::connect(worker->audioIn.get(), &QAudioBufferInput::readyToSendAudioBuffer, context_,
                         [this] { PumpAudio(); });

        QMediaRecorder* recorder = worker->recorder.get();
        QObject::connect(recorder, &QMediaRecorder::errorOccurred, context_,
                         [shared](QMediaRecorder::Error, const QString& text) {
                             const std::lock_guard<std::mutex> lock(shared->mutex);
                             if (shared->error.empty())
                                 shared->error = text.isEmpty() ? "the media encoder reported an error" : text.toStdString();
                             shared->cv.notify_all();
                         });
        QObject::connect(recorder, &QMediaRecorder::recorderStateChanged, context_,
                         [shared](QMediaRecorder::RecorderState state) {
                             const std::lock_guard<std::mutex> lock(shared->mutex);
                             if (state == QMediaRecorder::RecordingState) shared->recording = true;
                             if (state == QMediaRecorder::StoppedState && shared->recording) shared->stopped = true;
                             shared->cv.notify_all();
                         });

        recorder->setOutputLocation(QUrl::fromLocalFile(QString::fromStdString(path_)));
        recorder->setMediaFormat(format);
        recorder->setEncodingMode(QMediaRecorder::AverageBitRateEncoding);
        recorder->setAudioChannelCount(static_cast<int>(channels_));
        recorder->setAudioSampleRate(static_cast<int>(kRecordRate));
        if (profile.audioBitrateKbps > 0) recorder->setAudioBitRate(profile.audioBitrateKbps * 1000);
        if (!audioOnly_) {
            recorder->setVideoResolution(QSize(width_, height_));
            recorder->setVideoFrameRate(fps_);
            if (profile.videoBitrateKbps > 0) recorder->setVideoBitRate(profile.videoBitrateKbps * 1000);
        }
        worker_ = std::move(worker);
        recorder->record();
        return {};
    }

    // Recording thread: hand queued frames to the encoder for as long as it accepts them.
    void PumpVideo() {
        if (!worker_ || !worker_->videoIn) return;
        while (!worker_->videoQueue.empty()) {
            if (!worker_->videoIn->sendVideoFrame(worker_->videoQueue.front())) break;   // not ready: wait for the signal
            worker_->videoQueue.pop_front();
            shared_->queuedVideo.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    void PumpAudio() {
        if (!worker_ || !worker_->audioIn) return;
        while (!worker_->audioQueue.empty()) {
            if (!worker_->audioIn->sendAudioBuffer(worker_->audioQueue.front())) break;
            worker_->audioQueue.pop_front();
            shared_->queuedAudio.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // One picture for the encoder: the profile's size (scaled to fit with black bars if the output differs).
    QVideoFrame MakeFrame(const VideoFrameIn& in, qint64 startUs) const {
        QVideoFrame frame{QVideoFrameFormat(QSize(width_, height_), QVideoFrameFormat::Format_RGBA8888)};
        if (!frame.map(QVideoFrame::WriteOnly)) return {};
        uchar* dst = frame.bits(0);
        const int stride = frame.bytesPerLine(0);
        if (in.width == width_ && in.height == height_) {
            const size_t rowBytes = static_cast<size_t>(width_) * 4;
            for (int y = 0; y < height_; ++y)
                std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(stride),
                            in.pixels + static_cast<size_t>(y) * static_cast<size_t>(in.width), rowBytes);
        } else {
            const QImage src(reinterpret_cast<const uchar*>(in.pixels), in.width, in.height, in.width * 4,
                             QImage::Format_RGBA8888);
            QImage target(dst, width_, height_, stride, QImage::Format_RGBA8888);
            target.fill(Qt::black);
            const QSize fitted = QSize(in.width, in.height).scaled(width_, height_, Qt::KeepAspectRatio);
            QPainter painter(&target);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(QRect((width_ - fitted.width()) / 2, (height_ - fitted.height()) / 2,
                                    fitted.width(), fitted.height()), src);
        }
        frame.unmap();
        frame.setStartTime(startUs);
        frame.setEndTime(startUs + static_cast<qint64>(1'000'000.0 / fps_));
        return frame;
    }

    // Destroys the Qt objects on their own thread, stops the thread, and forgets it.
    void Teardown(bool) {
        if (!thread_) return;
        if (worker_) {
            (void)QMetaObject::invokeMethod(context_, [this] { worker_.reset(); }, Qt::BlockingQueuedConnection);
        }
        thread_->quit();
        thread_->wait();
        thread_.reset();   // context_ deleted itself when the thread finished
        context_ = nullptr;
        open_ = false;
    }

    ContainerKind kind_;
    bool audioOnly_ = false;
    bool open_ = false;
    int width_ = 1920;
    int height_ = 1080;
    double fps_ = 30.0;
    uint32_t channels_ = 2;
    std::string path_;
    std::shared_ptr<Shared> shared_;
    std::unique_ptr<QThread> thread_;
    QObject* context_ = nullptr;
    std::unique_ptr<Worker> worker_;
};

} // namespace

std::vector<ContainerKind> QtSupportedContainers() {
    return CachedSupport().containers;
}

std::shared_ptr<IRecordingContainer> MakeQtRecordingContainer(ContainerKind kind) {
    return std::make_shared<QtRecordingContainer>(kind);
}

} // namespace bps::recording

namespace bps::platform {
bool QtVideoRecordingSupported() {
    return bps::recording::CachedSupport().video;
}
} // namespace bps::platform
