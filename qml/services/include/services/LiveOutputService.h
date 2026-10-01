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

#include <QElapsedTimer>
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
    // A slide STAGED via stageSlides() but not yet pushed live — same
    // { valid, title, blocks, background } shape as onAirSlide, so the
    // Main Output tile can draw it with the identical DesignPreview path.
    // Empty once live (onAirSlide takes over) or once nothing is staged.
    // The whole point: double-clicking a slide used to go live immediately
    // (goLiveWithSlides), which flipped the top GO LIVE/STOP button and (if
    // an Output is bound to a real screen) put content on it too — with NO
    // explicit go-live action from the user. Staging decouples "what the
    // tile shows" from "what's actually pushed to real outputs": GO LIVE
    // is now the only thing that commits a stage.
    Q_PROPERTY(QVariantMap stagedSlide READ stagedSlide NOTIFY stagedChanged)
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
    // ---- OVERLAYS ON AIR (the Overlays pane's double-click take) ---------
    // MULTIPLE overlays can be live at once, stacked (last taken = topmost),
    // independent of the on-air slide/taken input/media — same "keeps
    // showing regardless of live_" convention those already use. Each entry
    // is { id, name }; the actual design blocks/background come from
    // OverlayLibraryService::design(id) when a tile draws them — this list
    // only owns WHICH ones are on. placeUnderSlide (that service's own
    // per-design flag) decides whether a given overlay draws below or above
    // the on-air slide's own DesignPreview — the tile reads that per entry.
    Q_PROPERTY(QVariantList activeOverlays READ activeOverlays NOTIFY overlaysChanged)
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
    // True while the FIRST enabled NDI-kind output (the active one, or any
    // other enabled NDI row when a projector is on air) is being fed by the
    // live loop via the engine's NDI display provider (native RGBA →
    // BroadcastEngine, no conversion; real NDI when the runtime is installed,
    // software loopback otherwise).
    Q_PROPERTY(bool ndiSending READ ndiSending NOTIFY ndiChanged)
    // Name of the NDI output whose frames are flowing RIGHT NOW (empty when
    // none): the Settings · Outputs pill marks THAT row LIVE even when a
    // projector is the on-air output. Identity = the roster row's name.
    Q_PROPERTY(QString ndiSendingOutputName READ ndiSendingOutputName NOTIFY ndiChanged)
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
    QVariantMap stagedSlide() const { return stagedSlide_; }

    QString inputLabel() const { return inputLabel_; }
    QString inputKind() const { return inputKind_; }
    qulonglong inputRev() const { return inputRev_; }
    bool inputLive() const { return inputLive_; }
    QVariantList activeOverlays() const { return activeOverlays_; }
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
    QString ndiSendingOutputName() const { return ndiSendingOutput_; }
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
    // Release ONLY an NDI take (no-op for camera/screen takes or when nothing
    // is taken) — the "ndi" feature switch's teardown path (EngineBridge::
    // applyNdiFeatureState) calls this when NDI is switched off mid-take.
    Q_INVOKABLE void clearNdiInput();
    // Kept for source compatibility; no longer used by the tiles (the
    // service detects frames itself — see inputLive). Harmless no-op for a
    // non-taken or already-live input.
    Q_INVOKABLE void confirmInputFrame(const QString &label);

    // ---- OVERLAYS ON AIR (multiple, stacked) -----------------------------
    // takeOverlay: appends {id,name} if not already on (no-op if it is —
    // callers toggle via overlayIsOnAir). clearOverlay: drops one by id.
    // clearAllOverlays: the whole layer off (the MonitorWall toolbar's
    // overlays clear button).
    Q_INVOKABLE void takeOverlay(const QString &id, const QString &name);
    Q_INVOKABLE void clearOverlay(const QString &id);
    Q_INVOKABLE void clearAllOverlays();
    Q_INVOKABLE bool overlayIsOnAir(const QString &id) const;

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
    // (The env-gated NDI OUTPUT self-test that used to live here was removed
    // at the user's request: auto-driving synthetic sends and a real GO LIVE
    // mid-boot interfered with the boot process. NDI health is instead
    // visible through the product's own telemetry — the connected-monitor
    // and backpressure lines pushNdiFrame logs while live.)

    Q_INVOKABLE void goLive();
    // ANY-CONTENT go-live (scripture verses, a sermon, media items): the
    // caller hands finished slides + the on-air name; the controller runs them
    // through the same pipeline. Answers via liveChanged either way — callers
    // can read live() to see whether it took.
    Q_INVOKABLE void goLiveWithSlides(const QString &name, const QVariantList &slides);
    // Sets stagedSlide WITHOUT going live — a click (e.g. double-clicking a
    // slide in ShowCenter) that should update the Main Output tile's
    // preview but must never by itself flip the top GO LIVE/STOP button or
    // reach a real bound-screen output window. goLive() commits whatever is
    // staged the next time it runs (the top button's own GO LIVE click);
    // stageSlides() itself never touches live_.
    Q_INVOKABLE void stageSlides(const QString &name, const QVariantList &slides);
    // Wipes a pending stage without touching live_/onAirSlide_ — stop()
    // alone never did this (staging and live are independent: a NEXT pick
    // can sit staged while the CURRENT one is still live), which is
    // exactly why "Clear all" — the "wipe everything, start fresh" action
    // — stayed stuck showing an old staged pick: it only ever called
    // stop(), gated on live_, so with nothing live yet (only staged)
    // Clear All did nothing at all to the tile.
    Q_INVOKABLE void clearStaged();
    Q_INVOKABLE void stop();
    // Clears WHATEVER'S currently on-air as its own content — the slide-
    // status icon's click (MonitorWall.qml, same shape as the image/overlay
    // buttons beside it): live_/STOP stays exactly as it is; only the
    // on-air slide swaps to the same blank/branded-background content
    // goLive() falls back to when nothing's queued at all. A no-op when
    // not live.
    Q_INVOKABLE void clearOnAirSlide();
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

    // ANY slide (not just on-air/staged) through the SAME active-output-
    // style template composition onAirSlide/stagedSlide already apply —
    // for a library preview (ShowCenter's grid/list thumbnails) to show
    // what a slide will ACTUALLY look like on air, not just its own raw
    // blocks. `slide` is the QML block shape (peekShow's own slide maps).
    Q_INVOKABLE QVariantMap previewWithActiveStyle(const QVariantMap &slide) const;

    // The last preview frame for the image provider (scaled to the request).
    QImage previewFrame(const QSize &requested);

    // The taken media's last decoded frame (the mediaplay provider reads it;
    // empty when nothing is on air / not yet decoded).
    const QImage &lastMediaFrame() const { return lastMediaFrame_; }

signals:
    void liveChanged();
    void frameRevChanged();
    void onAirChanged();
    void stagedChanged();
    void inputChanged();
    void overlaysChanged();
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
    // The newest frame of a held tap, kind-aware (camera/screen → the PAL's
    // decode cache, ndi → the engine receiver's converter) — see .cpp.
    QImage inputFrame(const QString &label, const QString &kind) const;
    // NDI-INPUT compositor feed: pushes the taken NDI source's newest frame
    // into CompositorState's media layer (the engine's scene builder draws
    // it under the on-air content — the same layer the media decoder fills).
    // No-op unless the CURRENT take is an NDI one.
    void pushNdiInputFrame(const QImage &frame);
    // Clears that compositor hold when it was an NDI feed that filled it.
    void clearNdiInputFrame();
    // Re-reads the runtime's current slide into onAirSlide_ (onAirChanged
    // piggybacks the emit). The slide's blocks change without the title or
    // index moving (a document edit re-synced while live), so the 10Hz poll
    // keeps the QML preview honest rather than keying on title/index only.
    void refreshOnAirSlide();
    // Shared by goLive()'s "nothing to open" fallback and clearOnAirSlide():
    // pushes the one blank slide both need (empty blocks, transparent
    // background) — the style's own background paints regardless, since
    // SceneBuilder composites it unconditionally, before/regardless of
    // blocks.
    void goLiveBlank();

    bool live_ = false;
    qulonglong frameRev_ = 0;
    qulonglong framesSent_ = 0;
    QString onAirTitle_;
    int onAirIndex_ = -1;
    int onAirTotal_ = 0;
    QVariantMap onAirSlide_;

    // ---- stageSlides() — see the header comment on stagedSlide ----
    QString stagedName_;
    QVariantList stagedSlidesRaw_;
    QVariantMap stagedSlide_;

    QTimer *poll_ = nullptr;   // while live: onAir/frames refresh at 10Hz
    std::unique_ptr<LivePreviewProvider> provider_;

    // ---- overlays-on-air state ----
    // {id,name} maps, insertion order = z-order (last taken = topmost).
    QVariantList activeOverlays_;

    // The NDI program sender: the first ENABLED NDI-kind output (not only
    // the active one — a projector can be on air while NDI must still feed)
    // gets the live loop's frames pushed at that output's CONFIGURED refresh
    // rate (Settings · Outputs) while live.
    void pushNdiFrame();
    void stopNdiFeed();
    // The network-visible NDI source name for an output row: "<AppName> .
    // <Row name>" when the row has a name (Studio Monitor convention), or
    // "" to fall back to the provider's own default. With no argument it
    // resolves the first enabled NDI row itself.
    static QString ndiSenderDisplayName(const QString &rowName = QString());
    // NDI sender identity per live session: the NDI SDK can't rename a live
    // sender, so a name change (or a new session) must recreate it. Called
    // from goLive's paths; tracks the name it last armed.
    void armNdiSenderForSession();
    QString ndiArmedName_;

    // ---- taken-input state ----
    QString inputLabel_;
    QString inputKind_;
    qulonglong inputRev_ = 0;
    bool inputLive_ = false;
    // True while an NDI take is feeding CompositorState's media layer (the
    // frame side of the take — the PAL-side taken-input identity is
    // ClearTakenInput, this is the pixels side).
    bool ndiInputLive_ = false;
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
    QString ndiSendingOutput_;   // roster name of the row being fed while sending
    qulonglong ndiFramesSent_ = 0;   // provider's own counter, poll-refreshed
    // One-shot send diagnostics (see pushNdiFrame): the gate warning, the
    // first SendFrame failure, and the "frames flowing, monitor discovering"
    // note — each logs once per outage, not once per poll tick.
    bool ndiGateLogged_ = false;
    bool ndiSendFailedLogged_ = false;
    bool ndiFirstFrameLogged_ = false;
    bool ndiWaitLogged_ = false;   // "waiting for buffer/frame" warm-up note
    // FreeShow-style send backpressure, adapted to a SYNCHRONOUS sender (see
    // pushNdiFrame): our GUI-thread send cannot queue frames, but a slow one
    // stalls the UI — so the tick after a slow send is skipped (half rate),
    // logged once per incident. Plus connected-monitor telemetry (engine-side
    // SDK truth) so "is anyone actually receiving" is visible in engine.log.
    QElapsedTimer ndiSendClock_;
    bool ndiClockStarted_ = false;
    bool ndiBackoff_ = false;      // skip the tick after a slow send
    bool ndiSlowLogged_ = false;   // "send is slow" once per incident
    int ndiFastStreak_ = 0;        // consecutive fast sends; ≥30 re-arms the slow-send log (toast-spam guard)
    int ndiReceiversSeen_ = -1;    // last reported connected-monitor count
    int ndiTick_ = 0;              // send-tick counter (telemetry, ~1 Hz nominal)
    // ---- The NDI feed's own clock ------------------------------------------
    // pushNdiFrame used to ride the 10 Hz GUI poll — the feed ran at the
    // poll's rate no matter what the output's Refresh rate select said. It
    // now runs on its OWN timer paced at that rate (the poll stays at 10 Hz
    // for slide titles + the preview), so Settings · Outputs · Refresh rate
    // reaches the wire: the timer's interval IS the setting, and the sender
    // advertises it in every frame's metadata.
    void ndiSendTick();                // the paced send (syncs the interval, then pushes)
    void startNdiSendClock();          // go-live: arm the clock at a provisional cadence
    void stopNdiSendClock();           // off air: stop the clock
    // The poll's cheap NDI-clock watch: (re)arm the send timer when a clock
    // is wanted but absent (a row enabled mid-live, the blank-roster edge)
    // — the timer's own tick handles cadence sync and shutdown.
    void pollTickRecheckNdi();
    // The first enabled NDI output's configured Refresh rate ("60 Hz" → 60).
    // A row with an unusable/blank value feeds at the engine's 30 fps
    // default; 0 means NO enabled NDI row (the send clock stops).
    static float ndiConfiguredFps();
    QTimer *ndiSendTimer_ = nullptr;   // while live: the paced NDI send clock
    float ndiSendFps_ = 0.0f;          // fps the timer currently runs at (0 = unsynced)
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
