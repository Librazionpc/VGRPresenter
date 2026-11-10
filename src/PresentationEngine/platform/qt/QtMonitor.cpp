#include "platform/qt/QtMonitor.hpp"

#include "core/logging/Logger.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QPointer>
#include <QScreen>
#include <QThread>

#include <atomic>
#include <format>
#include <mutex>

namespace bps::platform {

// What the watcher publishes and Enumerate() reads. Held by shared_ptr from both sides: the
// service can be destroyed (a test swapping backends) while the watcher still lives on the
// interface thread, and the reverse at application teardown.
struct QtMonitor::State {
    std::mutex mutex;
    std::vector<MonitorInfo> screens;   // guarded by mutex
    bool published = false;             // guarded by mutex: the watcher has written at least once
    std::atomic<bool> watcherRequested{false};
};

namespace {
MonitorInfo Describe(const QScreen* screen, const QScreen* primary) {
    MonitorInfo m;
    m.id = screen->name().toStdString();   // "\\.\DISPLAY1" on Windows, "eDP-1" on Linux
    m.name = m.id;
    // Qt keeps a screen's ORIGIN in device pixels and scales only its size, so the real pixel
    // rectangle is the origin as given and the size multiplied back by the device pixel ratio.
    const QRect g = screen->geometry();
    const qreal dpr = screen->devicePixelRatio();
    m.x = g.x();
    m.y = g.y();
    m.widthPx = qRound(g.width() * dpr);
    m.heightPx = qRound(g.height() * dpr);
    m.refreshRateHz = qRound(screen->refreshRate());
    m.dpi = qRound(96.0 * dpr);
    m.orientation = screen->angleBetween(screen->nativeOrientation(), screen->orientation());
    m.primary = screen == primary;
    m.connected = true;
    m.hdrSupported = false;   // Qt reports no HDR capability; best effort stays "no"
    return m;
}

// Lives on the interface thread. Every signal that can change what Enumerate() should answer
// lands in Publish(), which is the only writer of the snapshot.
class ScreenWatcher final : public QObject {
public:
    ScreenWatcher(std::shared_ptr<QtMonitor::State> state, QGuiApplication* app)
        : QObject(app), state_(std::move(state)), app_(app) {
        connect(app, &QGuiApplication::screenAdded, this, [this](QScreen* s) { Watch(s); Publish("screen added"); });
        connect(app, &QGuiApplication::screenRemoved, this, [this](QScreen*) { Publish("screen removed"); });
        connect(app, &QGuiApplication::primaryScreenChanged, this, [this](QScreen*) { Publish("primary changed"); });
        for (QScreen* s : app->screens()) Watch(s);
        Publish("first read");
    }

private:
    void Watch(QScreen* s) {
        connect(s, &QScreen::geometryChanged, this, [this](const QRect&) { Publish("geometry changed"); });
        connect(s, &QScreen::refreshRateChanged, this, [this](qreal) { Publish("refresh rate changed"); });
        connect(s, &QScreen::orientationChanged, this, [this](Qt::ScreenOrientation) { Publish("orientation changed"); });
        connect(s, &QScreen::physicalDotsPerInchChanged, this, [this](qreal) { Publish("scale changed"); });
    }

    void Publish(const char* why) {
        std::vector<MonitorInfo> next;
        const QScreen* primary = app_->primaryScreen();
        for (const QScreen* s : app_->screens())
            next.push_back(Describe(s, primary));
        std::string summary;
        for (const MonitorInfo& m : next)
            summary += std::format("{}{} {}x{}@{}Hz at {},{}{}", summary.empty() ? "" : "; ", m.id, m.widthPx,
                                   m.heightPx, m.refreshRateHz, m.x, m.y, m.primary ? " primary" : "");
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->screens = std::move(next);
            state_->published = true;
        }
        Logger::Instance().Info(std::format("displays ({}): {}", why, summary.empty() ? "none" : summary),
                                "QtMonitor");
    }

    std::shared_ptr<QtMonitor::State> state_;
    QGuiApplication* app_;
};
} // namespace

QtMonitor::QtMonitor(const IMonitor* native) : native_(native), state_(std::make_shared<State>()) {}

QtMonitor::~QtMonitor() = default;

// Starts the watcher once a QGuiApplication exists. Never blocks: from another thread the creation
// is queued to the interface thread and this call answers from the native adapter meanwhile.
void QtMonitor::EnsureWatcher() const {
    auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!app || QCoreApplication::closingDown())
        return;
    if (state_->watcherRequested.exchange(true))
        return;
    std::shared_ptr<State> state = state_;
    const auto create = [state, app = QPointer<QGuiApplication>(app)] {
        if (app && !QCoreApplication::closingDown())
            new ScreenWatcher(state, app);   // owned by the application object
    };
    // isCurrentThread() compares thread ids: it does not adopt a thread Qt did not start.
    if (app->thread()->isCurrentThread())
        create();
    else
        QMetaObject::invokeMethod(app, create, Qt::QueuedConnection);
}

bool QtMonitor::WatchingScreens() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->published;
}

std::vector<MonitorInfo> QtMonitor::Enumerate() const {
    EnsureWatcher();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->published)
            return state_->screens;
    }
    return native_ ? native_->Enumerate() : std::vector<MonitorInfo>{};
}

Result<MonitorInfo> QtMonitor::Primary() const {
    const std::vector<MonitorInfo> monitors = Enumerate();
    for (const MonitorInfo& m : monitors)
        if (m.primary) return m;
    if (!monitors.empty()) return monitors.front();
    return Error::Make(Err::NotFound, "Monitor", "no monitors connected");
}

std::string QtMonitor::DefaultMonitorId() const {
    const std::vector<MonitorInfo> monitors = Enumerate();
    for (const MonitorInfo& m : monitors)
        if (m.primary) return m.id;
    return monitors.empty() ? std::string() : monitors.front().id;
}

} // namespace bps::platform
