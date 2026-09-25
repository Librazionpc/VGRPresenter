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
#include <QVariantMap>
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
    // The on-air slide AS DESIGN BLOCKS — the same { blocks, background }
    // shape the preview pane renders: { valid, title, blocks, background }.
    // Empty map when not live. Lets QML previews (the monitor wall's tiles)
    // draw the on-air content with DesignPreview — the same renderer as the
    // ReferencePane preview — instead of re-guessing content from a title.
    Q_PROPERTY(QVariantMap onAirSlide READ onAirSlide NOTIFY onAirChanged)

public:
    static LiveOutputService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    static LiveOutputService &instance();

    bool live() const { return live_; }
    qulonglong frameRev() const { return frameRev_; }
    qulonglong framesSent() const { return framesSent_; }
    QString onAirTitle() const { return onAirTitle_; }
    int onAirIndex() const { return onAirIndex_; }
    int onAirTotal() const { return onAirTotal_; }
    QVariantMap onAirSlide() const { return onAirSlide_; }

    Q_INVOKABLE void goLive();
    // ANY-CONTENT go-live (scripture verses, a sermon, media items): the
    // caller hands finished slides + the on-air name; the controller runs them
    // through the same pipeline. Answers via liveChanged either way — callers
    // can read live() to see whether it took.
    Q_INVOKABLE void goLiveWithSlides(const QString &name, const QVariantList &slides);
    Q_INVOKABLE void stop();
    // The runtime's Next()/Previous() while live.
    Q_INVOKABLE bool next();
    Q_INVOKABLE bool previous();
    Q_INVOKABLE bool jumpTo(int index);

    // Any slide of the live set as design blocks (same shape as onAirSlide):
    // a thumbnail strip renders the whole presentation from this. Empty map
    // when not live or the index is out of range (hidden slides are skipped,
    // so the index is a VISIBLE index, matching onAirIndex).
    Q_INVOKABLE QVariantMap onAirSlideAt(int index) const;

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
    // Re-reads the runtime's current slide into onAirSlide_ (onAirChanged
    // piggybacks the emit). The slide's blocks change without the title or
    // index moving (a document edit re-synced while live), so the 10Hz poll
    // keeps the QML preview honest rather than keying on title/index only.
    void refreshOnAirSlide();

    bool live_ = false;
    qulonglong frameRev_ = 0;
    qulonglong framesSent_ = 0;
    QString onAirTitle_;
    int onAirIndex_ = -1;
    int onAirTotal_ = 0;
    QVariantMap onAirSlide_;
    QTimer *poll_ = nullptr;   // while live: onAir/frames refresh at 10Hz
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
