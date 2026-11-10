#include "platform/qt/QtMediaDevices.hpp"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QBuffer>
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QCoreApplication>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMetaObject>
#include <QPointer>
#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QScreenCapture>
#include <QSize>
#include <QThread>
#include <QVideoFrame>
#include <QVideoSink>

#if defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace bps::platform {

namespace {
// Promotes a weak_ptr to a shared_ptr. Named so, rather than `weak.lock()` written out at each use,
// because the lock-scope guard (tools/check_lock_scope.mjs) cannot tell std::weak_ptr::lock() from a
// mutex lock and would treat every later QObject::connect / screen start in the function as a blocking
// call made under a held lock. Nothing here waits or blocks: it is one atomic reference-count step.
template <class T>
std::shared_ptr<T> Promote(const std::weak_ptr<T>& weak)
{
    return weak.lock();
}

// Defined below: the screen controller (declared with State) needs it before its definition.
QScreen* TargetScreen(QGuiApplication* app, const std::string& monitorId);
} // namespace

struct QtVideoDevices::State {
    struct Frame {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t sequence = 0;
        // The newest picture as ONE immutable buffer, shared with whoever reads it (see PreviewFrameShared):
        // a reader takes a reference under the lock and copies — or does not copy — outside it.
        std::shared_ptr<const std::vector<uint8_t>> rgba;
        std::vector<uint8_t> jpeg;
        std::string failure;
        bool live = false;
        int64_t lastJpegAtMs = 0;
        int64_t jpegIntervalMs = 66;
    };

    struct Worker final : QObject {
        explicit Worker(std::weak_ptr<State> state) : state_(std::move(state)) {}

        void Start(const std::string& id, const std::string& mode)
        {
            StopCapture(id);
            if (auto state = Promote(state_)) {
                std::lock_guard<std::mutex> lock(state->mutex);
                const auto it = state->frames.find(id);
                if (it != state->frames.end()) it->second.jpegIntervalMs = PreviewInterval(mode);
            }
            QCameraDevice device;
            bool found = false;
            for (const QCameraDevice& candidate : QMediaDevices::videoInputs()) {
                if (candidate.id().toStdString() == id) {
                    device = candidate;
                    found = true;
                    break;
                }
            }
            if (!found || device.isNull()) {
                Fail(id, "camera is no longer available");
                return;
            }

            const QCameraFormat format = SelectFormat(device, mode);
            auto* camera = new QCamera(device, this);
            if (!format.isNull()) camera->setCameraFormat(format);
            auto* session = new QMediaCaptureSession(this);
            auto* sink = new QVideoSink(this);
            session->setCamera(camera);
            session->setVideoSink(sink);
            std::weak_ptr<State> weak = state_;
            QObject::connect(sink, &QVideoSink::videoFrameChanged, this,
                [weak, id](const QVideoFrame& videoFrame) {
                    if (auto state = Promote(weak))
                        state->worker->PublishFrame(id, videoFrame);
                });
            QObject::connect(camera, &QCamera::errorOccurred, this,
                [weak, id](QCamera::Error, const QString& message) {
                    if (auto state = Promote(weak)) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        auto it = state->frames.find(id);
                        if (it != state->frames.end()) it->second.failure = message.toStdString();
                        state->active.erase(id);
                    }
                });
            captures_.emplace(id, Capture{camera, session, sink});
            camera->start();
        }

        void Stop(const std::string& id)
        {
            StopCapture(id);
            if (auto state = Promote(state_)) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->active.erase(id);
                state->frames.erase(id);
            }
        }

        void StopAll()
        {
            while (!captures_.empty()) Stop(captures_.begin()->first);
        }

        void PublishFrame(const std::string& id, const QVideoFrame& videoFrame)
        {
            auto state = Promote(state_);
            if (!state || !videoFrame.isValid()) return;
            const QImage image = videoFrame.toImage().convertToFormat(QImage::Format_RGBA8888);
            if (image.isNull()) return;
            Frame next;
            next.width = static_cast<uint32_t>(image.width());
            next.height = static_cast<uint32_t>(image.height());
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            bool encodeJpeg = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto it = state->frames.find(id);
                if (it == state->frames.end()) return;
                next.sequence = it->second.sequence + 1;
                next.live = it->second.live;
                if (next.live)   // one copy out of the QImage; everything after shares this buffer
                    next.rgba = std::make_shared<const std::vector<uint8_t>>(
                        image.constBits(), image.constBits() + image.sizeInBytes());
                if (!next.live && now - it->second.lastJpegAtMs >= it->second.jpegIntervalMs) {
                    encodeJpeg = true;
                    it->second.lastJpegAtMs = now;
                }
            }
            if (encodeJpeg) {
                QByteArray jpeg;
                QBuffer buffer(&jpeg);
                if (buffer.open(QIODevice::WriteOnly)) image.save(&buffer, "JPEG", 85);
                next.jpeg.assign(jpeg.begin(), jpeg.end());
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            auto it = state->frames.find(id);
            if (it == state->frames.end()) return;
            it->second.width = next.width;
            it->second.height = next.height;
            it->second.sequence = next.sequence;
            if (it->second.live) {
                if (next.rgba && !next.rgba->empty()) it->second.rgba = std::move(next.rgba);
                it->second.jpeg.clear();
            }
            else if (!next.jpeg.empty()) it->second.jpeg = std::move(next.jpeg);
        }

    private:
        struct Capture { QCamera* camera; QMediaCaptureSession* session; QVideoSink* sink; };

        void StopCapture(const std::string& id)
        {
            auto it = captures_.find(id);
            if (it != captures_.end()) {
                it->second.camera->stop();
                it->second.session->setVideoSink(nullptr);
                it->second.session->setCamera(nullptr);
                delete it->second.session;
                delete it->second.sink;
                delete it->second.camera;
                captures_.erase(it);
            }
        }

        static QCameraFormat SelectFormat(const QCameraDevice& device, const std::string& mode)
        {
            static const std::regex pattern(R"((\d+)x(\d+)p([0-9]+(?:\.[0-9]+)?))");
            std::smatch match;
            int width = 0, height = 0;
            double fps = 0.0;
            if (std::regex_search(mode, match, pattern)) {
                try {
                    width = std::stoi(match[1].str());
                    height = std::stoi(match[2].str());
                    fps = std::stod(match[3].str());
                } catch (const std::exception&) {
                    width = height = 0;
                    fps = 0.0;
                }
            }
            QCameraFormat best;
            double bestScore = std::numeric_limits<double>::max();
            for (const QCameraFormat& candidate : device.videoFormats()) {
                const QSize size = candidate.resolution();
                if (width > 0 && (size.width() != width || size.height() != height)) continue;
                if (fps > 0.0 && (fps < candidate.minFrameRate() || fps > candidate.maxFrameRate())) continue;
                const double score = fps > 0.0
                    ? std::abs(static_cast<double>(candidate.maxFrameRate()) - fps) : 0.0;
                if (score < bestScore) { best = candidate; bestScore = score; }
            }
            return best;
        }

        static int64_t PreviewInterval(const std::string& mode)
        {
            static const std::regex pattern(R"((\d+)x(\d+)p([0-9]+(?:\.[0-9]+)?))");
            std::smatch match;
            if (!std::regex_search(mode, match, pattern)) return 66;
            double fps = 0.0;
            try {
                fps = std::stod(match[3].str());
            } catch (const std::exception&) {
                return 66;
            }
            if (!(fps > 0.0) || !std::isfinite(fps)) return 66;
            const double previewFps = std::min(fps, 30.0);
            return std::max<int64_t>(16, static_cast<int64_t>(std::ceil(1000.0 / previewFps)));
        }

        void Fail(const std::string& id, std::string message)
        {
            if (auto state = Promote(state_)) {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto it = state->frames.find(id);
                if (it != state->frames.end()) it->second.failure = std::move(message);
                state->active.erase(id);
            }
        }

        std::weak_ptr<State> state_;
        std::unordered_map<std::string, Capture> captures_;
    };

    struct ScreenController final : QObject {
        explicit ScreenController(std::weak_ptr<State> state, QGuiApplication* app)
            : QObject(app), state_(std::move(state)), app_(app) {}

        Result<void> Start(const std::string& monitorId)
        {
            QScreen* screen = TargetScreen(app_.data(), monitorId);
            if (!screen) {
                Fail(monitorId, "display id does not match a Qt screen on this session");
                return Error::Make(Err::NotFound, "Video", "display is no longer available: " + monitorId);
            }
            const auto existing = captures_.find(monitorId);
            if (existing != captures_.end() && existing->second.capture->screen() == screen
                && existing->second.capture->isActive())
                return Ok();
            StopCapture(monitorId);
            auto state = Promote(state_);
            if (!state) return Error::Make(Err::InvalidState, "Video", "screen preview service is closing");
            auto* capture = new QScreenCapture(this);
            auto* session = new QMediaCaptureSession(this);
            auto* sink = new QVideoSink(this);
            session->setScreenCapture(capture);
            session->setVideoSink(sink);
            std::weak_ptr<State> weak = state_;
            QObject::connect(sink, &QVideoSink::videoFrameChanged, this,
                [weak, monitorId](const QVideoFrame& frame) {
                    if (auto shared = Promote(weak); shared && shared->worker) {
                        QPointer<Worker> worker = shared->worker;
                        const QVideoFrame copied = frame;
                        QMetaObject::invokeMethod(worker.data(), [weak, worker, monitorId, copied] {
                            if (auto live = Promote(weak); live && worker)
                                worker->PublishFrame(monitorId, copied);
                        }, Qt::QueuedConnection);
                    }
                });
            QObject::connect(capture, &QScreenCapture::errorOccurred, this,
                [weak, monitorId](QScreenCapture::Error, const QString& message) {
                    if (auto shared = Promote(weak)) {
                        std::lock_guard<std::mutex> lock(shared->mutex);
                        auto it = shared->frames.find(monitorId);
                        if (it != shared->frames.end()) it->second.failure = message.toStdString();
                        shared->active.erase(monitorId);
                    }
                });
            capture->setScreen(screen);
            capture->start();
            if (capture->error() != QScreenCapture::NoError) {
                const QString why = capture->errorString();
                session->setScreenCapture(nullptr);
                session->setVideoSink(nullptr);
                delete session;
                delete sink;
                delete capture;
                Fail(monitorId, why.toStdString());
                return Error::Make(Err::Unsupported, "Video", why.toStdString());
            }
            captures_.emplace(monitorId, Capture{capture, session, sink});
            return Ok();
        }

        void Stop(const std::string& monitorId)
        {
            StopCapture(monitorId);
            if (auto state = Promote(state_)) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->active.erase(monitorId);
                state->frames.erase(monitorId);
            }
        }

        void StopAll()
        {
            while (!captures_.empty()) StopCapture(captures_.begin()->first);
        }

    private:
        struct Capture { QScreenCapture* capture; QMediaCaptureSession* session; QVideoSink* sink; };

        void StopCapture(const std::string& id)
        {
            const auto it = captures_.find(id);
            if (it == captures_.end()) return;
            it->second.capture->stop();
            it->second.session->setScreenCapture(nullptr);
            it->second.session->setVideoSink(nullptr);
            delete it->second.session;
            delete it->second.sink;
            delete it->second.capture;
            captures_.erase(it);
        }

        void Fail(const std::string& id, const std::string& message)
        {
            if (auto state = Promote(state_)) {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto it = state->frames.find(id);
                if (it != state->frames.end()) it->second.failure = message;
                state->active.erase(id);
            }
        }

        std::weak_ptr<State> state_;
        QPointer<QGuiApplication> app_;
        std::unordered_map<std::string, Capture> captures_;
    };

    mutable std::mutex mutex;
    std::unordered_map<std::string, Frame> frames;
    std::unordered_set<std::string> active;
    QThread thread;
    Worker* worker = nullptr;
    QPointer<ScreenController> screenController;
};

namespace {

AudioDeviceInfo Describe(const QAudioDevice& device, bool input, bool isDefault)
{
    const QAudioFormat format = device.preferredFormat();
    return {device.id().toStdString(), device.description().toStdString(), input,
            isDefault, static_cast<uint32_t>(std::max(0, format.sampleRate())),
            static_cast<uint32_t>(std::max(0, format.channelCount()))};
}

Result<AudioDeviceInfo> DefaultAudio(bool input)
{
    const QAudioDevice device = input ? QMediaDevices::defaultAudioInput()
                                      : QMediaDevices::defaultAudioOutput();
    if (device.isNull())
        return Error::Make(Err::NotFound, "Audio", input
            ? "no default audio input device is available"
            : "no default audio output device is available");
    return Describe(device, input, true);
}

#if defined(Q_OS_MACOS)
std::string MacDisplayUuid(CGDirectDisplayID display)
{
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(display);
    if (!uuid) return {};
    CFStringRef text = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    std::array<char, 128> utf8{};
    const bool converted = text && CFStringGetCString(text, utf8.data(),
        static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8);
    if (text) CFRelease(text);
    CFRelease(uuid);
    return converted ? std::string(utf8.data()) : std::string{};
}
#endif

QScreen* TargetScreen(QGuiApplication* app, const std::string& monitorId)
{
#if defined(Q_OS_MACOS)
    // The PAL's stable id is the CoreGraphics UUID. QScreen::name() is a
    // user-facing label on macOS, so resolve through matching logical bounds.
    uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) == kCGErrorSuccess && count > 0) {
        std::vector<CGDirectDisplayID> displays(count);
        if (CGGetActiveDisplayList(count, displays.data(), &count) == kCGErrorSuccess) {
            for (uint32_t i = 0; i < count; ++i) {
                const CGDirectDisplayID display = displays[i];
                if (MacDisplayUuid(display) != monitorId) continue;
                const CGRect bounds = CGDisplayBounds(display);
                const QRect geometry(static_cast<int>(std::lround(bounds.origin.x)),
                                     static_cast<int>(std::lround(bounds.origin.y)),
                                     static_cast<int>(std::lround(bounds.size.width)),
                                     static_cast<int>(std::lround(bounds.size.height)));
                for (QScreen* screen : app->screens())
                    if (screen->geometry() == geometry) return screen;
                return nullptr;
            }
        }
    }
#endif
    for (QScreen* screen : app->screens())
        if (screen->name().toStdString() == monitorId) return screen;
    return nullptr;
}

} // namespace

std::vector<AudioDeviceInfo> QtAudioDevices::Enumerate() const
{
    std::vector<AudioDeviceInfo> out;
    const QAudioDevice defaultInput = QMediaDevices::defaultAudioInput();
    const QAudioDevice defaultOutput = QMediaDevices::defaultAudioOutput();
    for (const QAudioDevice& device : QMediaDevices::audioInputs())
        out.push_back(Describe(device, true, device.id() == defaultInput.id()));
    for (const QAudioDevice& device : QMediaDevices::audioOutputs())
        out.push_back(Describe(device, false, device.id() == defaultOutput.id()));
    return out;
}

Result<AudioDeviceInfo> QtAudioDevices::DefaultOutput() const
{
    return DefaultAudio(false);
}

Result<AudioDeviceInfo> QtAudioDevices::DefaultInput() const
{
    return DefaultAudio(true);
}

std::string QtAudioDevices::Fingerprint() const
{
    std::vector<std::string> ids;
    for (const AudioDeviceInfo& device : Enumerate())
        ids.push_back(std::string(device.isInput ? "in:" : "out:")
                      + (device.isDefault ? "default:" : "device:") + device.id);
    std::sort(ids.begin(), ids.end());
    std::string result;
    for (const std::string& id : ids) {
        if (!result.empty()) result.push_back('|');
        result += id;
    }
    return result;
}

std::vector<VideoDeviceInfo> QtVideoDevices::Enumerate() const
{
    std::vector<VideoDeviceInfo> out;
    for (const QCameraDevice& device : QMediaDevices::videoInputs()) {
        VideoDeviceInfo info;
        info.id = device.id().toStdString();
        info.name = device.description().toStdString();
        info.isCamera = true;
        for (const QCameraFormat& format : device.videoFormats()) {
            const QSize size = format.resolution();
            if (size.width() <= 0 || size.height() <= 0) continue;
            const auto dimensions = std::pair{static_cast<uint32_t>(size.width()),
                                              static_cast<uint32_t>(size.height())};
            auto it = std::find_if(info.modes.begin(), info.modes.end(),
                [&](const VideoModeInfo& mode) {
                    return mode.width == dimensions.first && mode.height == dimensions.second;
                });
            if (it == info.modes.end()) {
                info.modes.push_back({dimensions.first, dimensions.second, {}});
                it = std::prev(info.modes.end());
            }
            const double reportedMaxRate = static_cast<double>(format.maxFrameRate());
            const uint32_t maxRate = std::isfinite(reportedMaxRate) && reportedMaxRate > 0.0
                && reportedMaxRate <= static_cast<double>(std::numeric_limits<uint32_t>::max())
                ? static_cast<uint32_t>(reportedMaxRate) : 0u;
            if (maxRate > 0 && std::find(it->fpsRates.begin(), it->fpsRates.end(), maxRate)
                                   == it->fpsRates.end())
                it->fpsRates.push_back(maxRate);
            info.maxFps = std::max(info.maxFps, maxRate);
        }
        std::sort(info.modes.begin(), info.modes.end(), [](const auto& a, const auto& b) {
            return a.width == b.width ? a.height < b.height : a.width < b.width;
        });
        out.push_back(std::move(info));
    }
    return out;
}

std::string QtVideoDevices::Fingerprint() const
{
    std::vector<std::string> ids;
    for (const VideoDeviceInfo& device : Enumerate()) ids.push_back(device.id);
    std::sort(ids.begin(), ids.end());
    std::string result;
    for (const std::string& id : ids) {
        if (!result.empty()) result.push_back('|');
        result += id;
    }
    return result;
}

QtVideoDevices::QtVideoDevices() : state_(std::make_shared<State>())
{
    state_->worker = new State::Worker(state_);
    state_->worker->moveToThread(&state_->thread);
    state_->thread.start();
}

QtVideoDevices::~QtVideoDevices()
{
    if (!state_) return;
    auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (app && !QCoreApplication::closingDown()) {
        const auto cleanupScreens = [state = state_] {
            if (state->screenController) {
                state->screenController->StopAll();
                delete state->screenController;
                state->screenController = nullptr;
            }
        };
        if (app->thread()->isCurrentThread()) cleanupScreens();
        else (void)QMetaObject::invokeMethod(app, cleanupScreens, Qt::BlockingQueuedConnection);
    }
    if (state_->worker) {
        State::Worker* worker = state_->worker;
        if (QThread::currentThread() == &state_->thread) {
            worker->StopAll();
            delete worker;
        } else {
            (void)QMetaObject::invokeMethod(worker, [worker] {
                worker->StopAll();
                delete worker;
            }, Qt::BlockingQueuedConnection);
        }
        state_->worker = nullptr;
        state_->thread.quit();
        if (QThread::currentThread() != &state_->thread) state_->thread.wait();
    }
}

Result<void> QtVideoDevices::StartPreview(const std::string& deviceId, const std::string& mode,
                                           const std::string& deviceName)
{
    (void)deviceName;
    if (!QCoreApplication::instance())
        return Error::Make(Err::Unsupported, "Video", "camera capture requires a Qt application");
    const auto devices = QMediaDevices::videoInputs();
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const QCameraDevice& device) {
        return device.id().toStdString() == deviceId;
    });
    if (found == devices.end())
        return Error::Make(Err::NotFound, "Video", "camera is no longer available: " + deviceId);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->frames[deviceId] = State::Frame{};
        state_->active.insert(deviceId);
    }
    const auto state = state_;
    if (!QMetaObject::invokeMethod(state_->worker, [state, deviceId, mode] {
            if (state->worker) state->worker->Start(deviceId, mode);
        }, Qt::QueuedConnection)) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->active.erase(deviceId);
        state_->frames.erase(deviceId);
        return Error::Make(Err::InvalidState, "Video", "camera worker is not available");
    }
    return Ok();
}

Result<void> QtVideoDevices::StopPreview(const std::string& deviceId)
{
    if (!state_ || !state_->worker) return Ok();
    const auto state = state_;
    if (!QMetaObject::invokeMethod(state_->worker, [state, deviceId] {
            if (state->worker) state->worker->Stop(deviceId);
        }, Qt::QueuedConnection))
        return Error::Make(Err::InvalidState, "Video", "camera worker is not available");
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->active.erase(deviceId);
        state_->frames.erase(deviceId);
    }
    return Ok();
}

std::vector<uint8_t> QtVideoDevices::PreviewFrame(const std::string& deviceId)
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->frames.find(deviceId);
    return it == state_->frames.end() ? std::vector<uint8_t>{} : it->second.jpeg;
}

std::string QtVideoDevices::PreviewFailure(const std::string& deviceId) const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->frames.find(deviceId);
    return it == state_->frames.end() ? std::string{} : it->second.failure;
}

Result<void> QtVideoDevices::StartScreenPreview(const std::string& monitorId)
{
    auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!app || QCoreApplication::closingDown())
        return Error::Make(Err::Unsupported, "Video", "screen capture requires a running GUI application");
    // X11 captures the targeted monitor itself. On Wayland there is no way to name a monitor: Qt's FFmpeg
    // backend asks the xdg-desktop-portal, which shows its own "choose what to share" dialog (the desktop
    // decides, as for every Wayland app) and streams the choice over PipeWire; if the session has no portal
    // the start fails and the error reaches the UI through PreviewFailure.
    bool backendSupported = QGuiApplication::platformName() == QStringLiteral("xcb")
        || QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
#if defined(Q_OS_MACOS)
    const QString mediaBackend = qEnvironmentVariable("QT_MEDIA_BACKEND").toLower();
    backendSupported = QGuiApplication::platformName() == QStringLiteral("cocoa")
        && (mediaBackend.isEmpty() || mediaBackend == QStringLiteral("ffmpeg"));
#endif
    if (!backendSupported)
        return Error::Make(Err::Unsupported, "Video",
            "targeted screen capture is unavailable with the active Qt platform/media backend");
    if (monitorId.empty())
        return Error::Make(Err::InvalidArgument, "Video", "a monitor id is required for screen capture");
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->frames[monitorId] = State::Frame{};
        state_->active.insert(monitorId);
    }
    const std::weak_ptr<State> weak = state_;
    const bool queued = QMetaObject::invokeMethod(app, [weak, app, monitorId] {
        auto state = Promote(weak);
        if (!state || QCoreApplication::closingDown()) return;
        if (!state->screenController)
            state->screenController = new State::ScreenController(state, app);
        const auto started = state->screenController->Start(monitorId);
        if (!started.ok()) {
            // Start() normally publishes the detailed failure itself. Keep a
            // fallback for failures that occur before it can identify a screen.
            std::lock_guard<std::mutex> lock(state->mutex);
            auto it = state->frames.find(monitorId);
            if (it != state->frames.end() && it->second.failure.empty())
                it->second.failure = started.error().message;
            state->active.erase(monitorId);
        }
    }, Qt::QueuedConnection);
    if (!queued) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->frames.erase(monitorId);
        state_->active.erase(monitorId);
        return Error::Make(Err::InvalidState, "Video", "screen capture could not be queued");
    }
    return Ok();
}

Result<void> QtVideoDevices::StopScreenPreview(const std::string& monitorId)
{
    if (!state_) return Ok();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->active.erase(monitorId);
        state_->frames.erase(monitorId);
    }
    auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!app || QCoreApplication::closingDown()) return Ok();
    const std::weak_ptr<State> weak = state_;
    if (!QMetaObject::invokeMethod(app, [weak, monitorId] {
            if (auto state = Promote(weak); state && state->screenController)
                state->screenController->Stop(monitorId);
        }, Qt::QueuedConnection))
        return Error::Make(Err::InvalidState, "Video", "screen capture stop could not be queued");
    return Ok();
}

std::vector<std::string> QtVideoDevices::ActivePreviews() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->active.begin(), state_->active.end()};
}

IVideo::SharedFrame QtVideoDevices::PreviewFrameShared(const std::string& deviceId)
{
    // A reference, taken under the lock — the 8 MB buffer itself is never copied here.
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->frames.find(deviceId);
    if (it == state_->frames.end() || !it->second.rgba) return {};
    SharedFrame out;
    out.width = it->second.width;
    out.height = it->second.height;
    out.seq = it->second.sequence;
    out.rgba = it->second.rgba;
    return out;
}

IVideo::DecodedFrame QtVideoDevices::PreviewFramePixels(const std::string& deviceId)
{
    // The copy this contract promises happens OUTSIDE the lock: the capture worker publishes the next
    // frame under the same mutex, and copying 8 MB while holding it stalled the camera.
    const SharedFrame shared = PreviewFrameShared(deviceId);
    if (shared.empty()) return {};
    return {shared.width, shared.height, *shared.rgba, shared.seq};
}

uint64_t QtVideoDevices::PreviewFrameSeq(const std::string& deviceId)
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = state_->frames.find(deviceId);
    return it == state_->frames.end() ? 0 : it->second.sequence;
}

void QtVideoDevices::SetPreviewLive(const std::string& deviceId, bool live)
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto it = state_->frames.find(deviceId);
    if (it == state_->frames.end()) return;
    it->second.live = live;
    if (live) it->second.jpeg.clear();
    else {
        it->second.rgba.reset();
        it->second.width = it->second.height = 0;
    }
}

} // namespace bps::platform
