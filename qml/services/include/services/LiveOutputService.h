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
class LiveMediaFrameProvider;
class QTimer;
class QMediaPlayer;
class QAudioOutput;
class QVideoSink;

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
    // Is there a snapshot of the last go-live (whole show, or an ad-hoc
    // scripture/table pick) that resumeSlide() can bring back? The MonitorWall
    // toolbar's slide-clear button toggles on this — stop() intentionally
    // does not clear this, so "clear" then "bring back" round-trips even
    // though there is no independent slide-only hide in the engine yet.
    Q_PROPERTY(bool canResumeSlide READ canResumeSlide NOTIFY onAirChanged)
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
    // ---- MEDIA ON AIR (the Media pane's take-to-program) ----------------
    // A video/image FILE playing into the compositor: the service owns a real
    // decoder (QMediaPlayer → QVideoSink), frames go through a dedicated
    // image provider and every monitor tile composites them UNDER the on-air
    // content — the exact layer convention the taken camera input uses.
    Q_PROPERTY(bool mediaOnAir READ mediaOnAir NOTIFY mediaChanged)
    // Absolute file path of the taken media ("" when none).
    Q_PROPERTY(QString mediaPath READ mediaPath NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY mediaChanged)
    // True when the file is a VIDEO (audio-capable). An image paints a still.
    Q_PROPERTY(bool mediaIsVideo READ mediaIsVideo NOTIFY mediaChanged)
    // True when the file is AUDIO-ONLY — a real QMediaPlayer transport
    // (play/pause/seek all work, same as video), but no compositor frame:
    // there is nothing to paint, so the tiles' media-frame layer stays off
    // for this one (see mediaIsVideo for that gate).
    Q_PROPERTY(bool mediaIsAudio READ mediaIsAudio NOTIFY mediaChanged)
    // Video playback state: playing / paused / stopped-off-air.
    Q_PROPERTY(QString mediaState READ mediaState NOTIFY mediaChanged)
    // Position (ms), duration (ms; 0 for images) and the UI's rev bump (tiles
    // re-fetch their provider URL on every bump — ~30 fps while playing).
    Q_PROPERTY(qlonglong mediaPosition READ mediaPosition NOTIFY mediaTick)
    Q_PROPERTY(qlonglong mediaDuration READ mediaDuration NOTIFY mediaChanged)
    Q_PROPERTY(qulonglong mediaRev READ mediaRev NOTIFY mediaTick)
    // Muted playback (monitoring the file's own audio off the program mix).
    Q_PROPERTY(bool mediaMuted READ mediaMuted NOTIFY mediaChanged)
    // ---- NDI PROGRAM SENDER ---------------------------------------------
    // True while the ACTIVE output's kind is NDI and the live loop is
    // feeding the engine's NDI display provider (RGBA→UYVY→BroadcastEngine;
    // real NDI when the runtime is installed, software loopback otherwise).
    Q_PROPERTY(bool ndiSending READ ndiSending NOTIFY ndiChanged)
    // Frames the sender has pushed (poll-refreshed with the preview feed).
    Q_PROPERTY(qulonglong ndiFramesSent READ ndiFramesSent NOTIFY ndiChanged)

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
    bool canResumeSlide() const { return canResumeSlide_; }

    QString inputLabel() const { return inputLabel_; }
    QString inputKind() const { return inputKind_; }
    qulonglong inputRev() const { return inputRev_; }
    bool inputLive() const { return inputLive_; }
    bool mediaOnAir() const { return mediaOnAir_; }
    QString mediaPath() const { return mediaPath_; }
    QString mediaName() const { return mediaName_; }
    bool mediaIsVideo() const { return mediaIsVideo_; }
    bool mediaIsAudio() const { return mediaIsAudio_; }
    QString mediaState() const { return mediaState_; }
    qlonglong mediaPosition() const { return mediaPosition_; }
    qlonglong mediaDuration() const { return mediaDuration_; }
    qulonglong mediaRev() const { return mediaRev_; }
    bool mediaMuted() const { return mediaMuted_; }
    bool ndiSending() const { return ndiSending_; }
    qulonglong ndiFramesSent() const { return ndiFramesSent_; }
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

    // ---- MEDIA ON AIR ----------------------------------------------------
    // takeMedia(path, name): a video/image file plays into the compositor
    // layer on every monitor tile (under the on-air content, over the style
    // background — the taken-input convention). Videos start playing
    // immediately; taking a different file swaps the layer. clearMedia()
    // takes it off. Audio rides the player's own output (the machine's mix =
    // the program mix the loopback tap meters).
    Q_INVOKABLE void takeMedia(const QString &path, const QString &name);
    Q_INVOKABLE void clearMedia();
    // Transport: play/pause toggle, seek to a fraction of the duration (0..1;
    // images ignore), mute toggle. stop() (going off air) pauses the player.
    Q_INVOKABLE void mediaTogglePlay();
    Q_INVOKABLE void mediaSeek(qreal fraction);
    Q_INVOKABLE void mediaToggleMuted();
    // ms → "m:ss" for the transport readout.
    Q_INVOKABLE QString formatMediaTime(qlonglong ms) const;

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
    // Replays the last successful go-live (goLive()'s open show, or the last
    // goLiveWithSlides() ad-hoc pick, whichever happened last) — the
    // MonitorWall toolbar's slide-clear button's "click again" side. No-op
    // already live or nothing to resume.
    Q_INVOKABLE void resumeSlide();
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

    // The taken media's last decoded frame (the mediaplay provider reads it;
    // empty when nothing is on air / not yet decoded).
    const QImage &lastMediaFrame() const { return lastMediaFrame_; }

signals:
    void liveChanged();
    void frameRevChanged();
    void onAirChanged();
    void inputChanged();
    // NDI program-sender state (started/stopped, frame counter refresh).
    void ndiChanged();
    // Media-on-air state changes (take/clear/play-pause/mute/end-of-file)
    // and the ~30 fps position/frame tick while a video plays.
    void mediaChanged();
    void mediaTick();
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

    // ---- resumeSlide() snapshot ----
    bool canResumeSlide_ = false;
    bool lastWasAdHocSlides_ = false;   // goLiveWithSlides() vs plain goLive()
    QString lastSlidesName_;
    QVariantList lastSlidesRaw_;
    QTimer *poll_ = nullptr;   // while live: onAir/frames refresh at 10Hz
    std::unique_ptr<LivePreviewProvider> provider_;

    // The NDI program sender: the active output (kind NDI) gets the live
    // loop's frames pushed at the poll's rate while live.
    void pushNdiFrame();
    void stopNdiFeed();

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

    // ---- media-on-air state (see the properties above) -------------------
    bool mediaOnAir_ = false;
    QString mediaPath_;
    QString mediaName_;
    bool mediaIsVideo_ = false;
    bool mediaIsAudio_ = false;
    QString mediaState_ = QStringLiteral("stopped");
    qlonglong mediaPosition_ = 0;
    qlonglong mediaDuration_ = 0;
    qulonglong mediaRev_ = 0;
    bool mediaMuted_ = false;
    // Lazily created on the first takeMedia(); the player's videoFrames flow
    // into the sink, whose frame lands in lastMediaFrame_ (the provider reads
    // it) and bumps mediaRev_.
    QMediaPlayer *mediaPlayer_ = nullptr;
    QAudioOutput *mediaAudio_ = nullptr;
    QVideoSink *mediaSink_ = nullptr;
    QImage lastMediaFrame_;

    // ---- NDI program sender (see the properties above) --------------------
    bool ndiSending_ = false;
    qulonglong ndiFramesSent_ = 0;   // provider's own counter, poll-refreshed
};

// QQuickImageProvider over the engine preview output's last frame:
//   image://livepreview?v=<frameRev>
class LivePreviewProvider final : public QQuickImageProvider {
public:
    LivePreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};

// QQuickImageProvider over the taken media's LAST DECODED FRAME:
//   image://mediaplay?v=<mediaRev>
// Every monitor tile re-fetches on each mediaRev bump while a video plays
// (~30 fps) — the same rev-driven pacing the other two providers use. A
// taken-but-not-yet-decoded video answers a 1×1 transparent (the tile keeps
// its warm-up art up; no warning spam), and an image file answers its still.
class LiveMediaFrameProvider final : public QQuickImageProvider {
public:
    LiveMediaFrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
