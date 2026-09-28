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
#include <QSet>
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
    // ---- Taken video input (the Media pane's click-to-preview) ----------
    // A video source taken into the OUTPUT PREVIEW: the compositor layer
    // under the on-air content on every monitor tile, exactly like the
    // style's PNG/JPG background — QML Image layers all the way down. The
    // service owns the PAL tap (owner "output") for camera/screen kinds
    // and exposes the provider URL + a revision for the tiles' pump.
    Q_PROPERTY(QString inputLabel READ inputLabel NOTIFY inputChanged)
    Q_PROPERTY(QString inputKind READ inputKind NOTIFY inputChanged)
    // Bumped ~15×/s while an input is taken OR any card preview is held —
    // the tiles'/cards' Image.source cache-buster (the videopreview
    // provider returns the PAL's newest JPEG per request).
    Q_PROPERTY(qulonglong inputRev READ inputRev NOTIFY inputChanged)
    // The labels currently held as INTERNAL card previews (one-click).
    // Cards read membership to show their live thumbnail; output takes are
    // inputLabel (double-click, purple border) and stay separate.
    Q_PROPERTY(QVariantList cardPreviews READ cardPreviewsList NOTIFY inputChanged)
    // True while a taken input's tap is producing (warm-up honest: false
    // until the provider's first frame decodes).
    Q_PROPERTY(bool inputLive READ inputLive NOTIFY inputChanged)

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

    QString inputLabel() const { return inputLabel_; }
    QString inputKind() const { return inputKind_; }
    qulonglong inputRev() const { return inputRev_; }
    bool inputLive() const { return inputLive_; }
    QVariantList cardPreviewsList() const
    {
        QVariantList out;
        for (const QString &k : cardPreviews_.keys())
            out.append(k);
        return out;
    }

    // Take a video source's feed into the output preview (the Media pane's
    // click). kind: "camera" | "screen" | "media" | "ndi"; only camera and
    // screen have a real local tap today — the others report honestly and
    // stay untaken. Taking a source releases the previous one (one input
    // layer on the compositor); clearInput() releases the device.
    Q_INVOKABLE void takeInput(const QString &label, const QString &kind, const QString &mode);
    Q_INVOKABLE void clearInput();
    // Kept for source compatibility; no longer used by the tiles (the
    // service detects frames itself — see inputLive). Harmless no-op for a
    // non-taken or already-live input.
    Q_INVOKABLE void confirmInputFrame(const QString &label);

    // ---- INTERNAL CARD PREVIEWS (the Media pane's one-click thumbnails) ---
    // ONE click starts a small owner-"card" tap whose frames feed the card's
    // own state window — the INTERNAL preview, independent of the output
    // take (owner "output", double-click, purple border). Re-click releases.
    Q_INVOKABLE void previewInput(const QString &label, const QString &kind, const QString &mode);
    Q_INVOKABLE void endPreviewInput(const QString &label);
    // HEALTH: is this source currently reachable AND (when a tap is held)
    // producing frames? The card's slashed-icon "can't reach" state reads
    // this — a closed window / unplugged camera flips it within one pump
    // tick (~66 ms) and the icon gets its slash.
    Q_INVOKABLE bool inputHealthy(const QString &label, const QString &kind) const;
    // Is this label's tap PRODUCING frames (decoded at least one)? Per-label
    // — inputLive only tracks the OUTPUT take, but a card-only preview must
    // light its thumbnail too. QML reads it inside bindings that also read
    // inputRev, so it refreshes at the pump's rate.
    Q_INVOKABLE bool inputProducing(const QString &label) const;

    // Env-gated boot self-test (VGR_OUTPUT_INPUT_TEST=1): takes the first
    // real window input ~2.5s after launch and logs PASS (frames decoded) /
    // FAIL to the launch log ~4s later. Inert without the env var.
    static void runEnvSelfTest();

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

    // The preview toolbar's ‹ ›: advance within the on-air set when it has
    // somewhere to go; at either end (or on a single-slide pick) the step
    // falls through to passageStepRequested — the content tab that owns the
    // air re-picks the neighbouring passage and replays it (the same path
    // its own pills take), so the toolbar behaves like the preview pane
    // instead of going dead on a one-slide passage.
    Q_INVOKABLE void stepPassage(int direction);

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
    void inputChanged();
    // stepPassage() reached the edge of the on-air set: the tab whose content
    // IS on air should re-pick the neighbouring passage (direction -1/+1).
    void passageStepRequested(int direction);

private:
    explicit LiveOutputService(QObject *parent = nullptr);

    static LiveOutputService *s_instance;

    void pollTick();   // refresh onAir* + framesSent from the engine
    // The shared input pump tick (~15 Hz while ANY tap is held): flips
    // inputLive on the taken input's first frame (decode-cache poll) and
    // bumps inputRev so every consuming Image re-fetches. Stops itself when
    // neither a take nor a card preview is held.
    void pumpTick();
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

    // ---- taken-input state ----
    QString inputLabel_;
    QString inputKind_;
    qulonglong inputRev_ = 0;
    bool inputLive_ = false;
    QTimer *inputPump_ = nullptr;   // while taken: inputRev bump at ~15Hz
    // The internal card previews (one-click): label → {kind, mode}. Their
    // taps ride the SAME previewIds_ table under owner "card"; releasing is
    // per-label (endPreviewInput).
    QVariantMap cardPreviews_;
    // Labels whose taps have decoded at least one frame (pump-maintained);
    // inputProducing() reads it. Pruned on release.
    QSet<QString> producing_;
};

// QQuickImageProvider over the engine preview output's last frame:
//   image://livepreview?v=<frameRev>
class LivePreviewProvider final : public QQuickImageProvider {
public:
    LivePreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
