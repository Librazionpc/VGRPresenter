#pragma once

// The QML face of the engine's live render loop (bps::live::LiveOutputController).
//
// GoLive()/Stop() start/stop the engine's 60Hz pipeline loop: the open show's
// current slide is compiled, prepared and rendered continuously, frames are
// distributed to every enabled output (OutputManager feeds the Telemetry
// output meter; RenderEngine's frame events feed the rendering meter) and the
// preview feed below serves the distributed frame to QML.
//
// The preview feed is a QQuickImageProvider ("image://livepreview?v=<n>"):
// QML re-requests on `frameRev` changes (polling at the frame cadence would
// fight the GUI thread; a Rev-driven Image.source re-fetch naturally paces to
// ~the UI's update rate). Pixels come from the engine-side Preview output's
// last frame — the same frame the real outputs received.

#include <QImage>
#include <QQuickImageProvider>
#include <QObject>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>
#include <memory>

class LivePreviewProvider;
class QTimer;

class LiveOutputService : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // On air? (the engine loop is running)
    Q_PROPERTY(bool live READ live NOTIFY liveChanged)
    // Bumped on every new preview frame; QML Image source appends it.
    Q_PROPERTY(qulonglong frameRev READ frameRev NOTIFY frameRevChanged)
    // Frames the loop has rendered this run (diagnostics).
    Q_PROPERTY(qulonglong framesSent READ framesSent NOTIFY frameRevChanged)
    // Slide title currently on air ("" when not live).
    Q_PROPERTY(QString onAirTitle READ onAirTitle NOTIFY onAirChanged)
    // Slide index currently on air (-1 when not live).
    Q_PROPERTY(int onAirIndex READ onAirIndex NOTIFY onAirChanged)
    // Total slides in the live show.
    Q_PROPERTY(int onAirTotal READ onAirTotal NOTIFY onAirChanged)

public:
    static LiveOutputService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    static LiveOutputService &instance();

    bool live() const { return live_; }
    qulonglong frameRev() const { return frameRev_; }
    qulonglong framesSent() const { return framesSent_; }
    QString onAirTitle() const { return onAirTitle_; }
    int onAirIndex() const { return onAirIndex_; }
    int onAirTotal() const { return onAirTotal_; }

    Q_INVOKABLE void goLive();
    Q_INVOKABLE void stop();
    // The runtime's Next()/Previous() while live.
    Q_INVOKABLE bool next();
    Q_INVOKABLE bool previous();
    Q_INVOKABLE bool jumpTo(int index);

    // The last preview frame for the image provider (scaled to the request).
    QImage previewFrame(const QSize &requested);

signals:
    void liveChanged();
    void frameRevChanged();
    void onAirChanged();

private:
    explicit LiveOutputService(QObject *parent = nullptr);

    static LiveOutputService *s_instance;

    void pollTick();   // refresh onAir* + framesSent from the engine

    bool live_ = false;
    qulonglong frameRev_ = 0;
    qulonglong framesSent_ = 0;
    QString onAirTitle_;
    int onAirIndex_ = -1;
    int onAirTotal_ = 0;
    QTimer *poll_ = nullptr;   // while live: onAir/frames refresh at 4Hz
    std::unique_ptr<LivePreviewProvider> provider_;
};

// QQuickImageProvider over the engine preview output's last frame:
//   image://livepreview?v=<frameRev>
class LivePreviewProvider final : public QQuickImageProvider {
public:
    LivePreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
