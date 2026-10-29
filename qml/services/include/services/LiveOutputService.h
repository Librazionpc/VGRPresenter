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
#include <QHash>
#include <QImage>
#include <QQuickImageProvider>
#include <QSet>
#include <QObject>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// The feed's two engine-side anchors, needed by value in NdiSendPlan below:
// the display provider that owns the NDI sender, and the render output the
// live loop fills with the picture it feeds.
#include "modules/display/Providers.hpp"       // NdiDisplayProvider
#include "modules/rendering/RenderOutputs.hpp" // FrameBufferOutput

class LivePreviewProvider;
class LiveMediaFrameProvider;
class QTimer;
class QMediaPlayer;
// The reading step's one animator (see readingScrollAnim_): a QVariantAnimation
// keeps the pace, the curve and the frame cadence in Qt's hands, and every value
// it produces is pushed to the engine's live loop as well as to QML.
class QVariantAnimation;
class QAudioOutput;
class QVideoSink;
namespace bps::rendering { struct Frame; }

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
    // Unstyled source for per-output local monitor previews. `stagedSlide`
    // remains the active-output preview used by the main preview pane.
    Q_PROPERTY(QVariantMap stagedSlideRaw READ stagedSlideRaw NOTIFY stagedChanged)
    // Is there something for Clear all's UNDO to put back? clearAll() (the
    // wall toolbar's Clear all) snapshots the content layers it takes down,
    // and the toolbar's "Show all" reads this to know whether it has anything
    // to restore — and to keep the toolbar on screen after a Clear all, since
    // the one control that undoes it lives in that toolbar.
    Q_PROPERTY(bool canRestoreCleared READ canRestoreCleared NOTIFY clearedChanged)
    // IS THE OUTPUT DELIBERATELY SHOWING NOTHING? A blackout is the one action
    // that puts a clearStage carrier on air (SceneBuilder paints NOTHING for
    // it — no slide, no style colour, no background image), so the state is
    // read straight off the on-air slide instead of kept as a second flag that
    // could disagree with what the engine is drawing. The wall toolbar's
    // Blackout control is lit while this is true, and pressing it again brings
    // the hidden content back (endBlackout) — which is why a blackout is a
    // LATCH and not a fire-and-forget clear: a live service needs a way out.
    Q_PROPERTY(bool blackedOut READ blackedOut NOTIFY onAirChanged)
    // ---- Feed health (the header's stall pill + the automatic blackout) ----
    // Is at least one live output's PROGRAM FEED stalled right now? The live
    // loop re-presents the composed frame to every output once a second even
    // while the picture is static (LiveOutputController's kHeartbeatMs), so a
    // stall is measured against that HEARTBEAT, never against "did the slide
    // change": a verse that sits for a minute is healthy, an output whose
    // stamp stops moving for a few seconds is not. See sampleFeedHealth().
    Q_PROPERTY(bool feedStalled READ feedStalled NOTIFY feedHealthChanged)
    // Which outputs, in the operator's terms: "M1, M2" (M<n> = the n-th ENABLED
    // output in roster order). Empty when nothing is stalled.
    Q_PROPERTY(QString feedStallLabels READ feedStallLabels NOTIFY feedHealthChanged)
    // The header pill's own line: "M1, M2 · No live feed" while stalled,
    // "M1 · Feed restored" for a few seconds after it comes back, "" otherwise.
    Q_PROPERTY(QString feedNoticeText READ feedNoticeText NOTIFY feedHealthChanged)
    // "stalled" | "restored" | "" — picks the pill's colour.
    Q_PROPERTY(QString feedNoticeKind READ feedNoticeKind NOTIFY feedHealthChanged)
    // Bumped with every change above: a monitor tile reads its own stall state
    // through a binding that has to depend on something notifiable, and an
    // invokable alone is opaque to the QML engine's dependency tracking.
    Q_PROPERTY(int feedHealthRev READ feedHealthRev NOTIFY feedHealthChanged)
    // EVERY OUTPUT'S OWN RUNTIME STATE, keyed by roster name:
    //   { presented: bool, ageMs: qint64, frames: qulonglong, stalled: bool }
    // — the same three facts for an HDMI row as for an NDI row. `frames` is
    // what that output has received (IRenderOutput::FramesReceived through
    // RenderEngine::OutputFeedAges), `ageMs` how long since its last present
    // (-1 = never), `stalled` whether the feed probe has given up on it.
    //
    // WHY IT EXISTS: the Outputs screen's live line used to be NDI/SDI only,
    // so a HDMI row said nothing about itself — "the notification is only
    // instantiated in New Screen 2" (whichever row had a sender), when every
    // row has a feed that can be measured. Emptied when the session ends.
    Q_PROPERTY(QVariantMap outputRuntime READ outputRuntime NOTIFY outputRuntimeChanged)
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
    // Unstyled runtime slide, for per-output monitor tiles to compose with
    // their own saved template. The main preview continues to use onAirSlide.
    Q_PROPERTY(QVariantMap onAirSlideRaw READ onAirSlideRaw NOTIFY onAirChanged)
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
    // Effective fill for the media currently on air: the file's saved override,
    // or the app-wide Outputs setting when the file has no override.
    Q_PROPERTY(QString mediaFit READ mediaFit NOTIFY mediaChanged)
    // App-wide fallback used when a media file has no personal fill choice.
    Q_PROPERTY(QString defaultMediaFit READ defaultMediaFit NOTIFY mediaChanged)
    // DOES THE PICTURE ON AIR ACTUALLY CARRY TRANSPARENCY? True only when the
    // take is a STILL whose decoded pixels include at least one with alpha < 255
    // (see preflightMediaTake). A video never does, and neither does an opaque
    // photo saved with an alpha channel — so a surface can tell "this frame is
    // transparent" (show the checkerboard convention) from "this frame is
    // opaque" (show nothing of the sort) without guessing from the file's
    // extension. The monitor tile used to treat EVERY take as a transparent
    // frame, which painted the tile's checkerboard under a taken VIDEO: a 9:16
    // clip in a 16:9 pane showed checkered pillarbox bars that the real output
    // never had ("this shouldn't even be happening").
    Q_PROPERTY(bool mediaHasAlpha READ mediaHasAlpha NOTIFY mediaChanged)
    // The label a taken file's audio meter lands under in
    // EngineBridge::inputLevels (see applyMediaTake). Exposed so the monitor
    // tile matches the take's snapshot by the ONE spelling the service publishes
    // rather than by a copy that can drift from it.
    Q_PROPERTY(QString mediaMeterLabel READ mediaMeterLabel CONSTANT)
    // ---- NDI PROGRAM SENDER ---------------------------------------------
    // True while the selected enabled NDI-kind output (or the first enabled
    // NDI row when a projector is on air) is being fed by the
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
    // TRUE while an enabled NDI output WANTS feeding but no frame has reached
    // the wire yet (the first second or two after GO LIVE, once the live loop
    // is warming up). Lets Settings · Outputs show "warming up" on the row
    // inline, instead of the misleading "ready (go live to send)" that a
    // not-yet-sending feed reads as. False the moment frames flow (or when no
    // NDI row / not live).
    Q_PROPERTY(bool ndiWarming READ ndiWarming NOTIFY ndiChanged)
    // The roster name of the row being warmed ("" when not warming) — only
    // selected NDI output feeds, so the row can gate on its own name.
    Q_PROPERTY(QString ndiWarmingOutputName READ ndiWarmingOutputName NOTIFY ndiChanged)
    // Per-output NDI state so every enabled NDI row reports its own sender.
    Q_PROPERTY(QVariantMap ndiOutputStates READ ndiOutputStates NOTIFY ndiChanged)

public:
    static LiveOutputService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    static LiveOutputService &instance();
    // aboutToQuit (main.cpp): stop the NDI worker thread and WAIT for it —
    // the SDK must never be torn down under an in-flight send. Called before
    // EngineBridge::shutdown().
    Q_INVOKABLE void shutdownNdiWorker();

    bool live() const { return live_; }
    qulonglong frameRev() const { return frameRev_; }
    // The 10Hz poll's CACHED copy (see pollTick): right for a preview refresh,
    // too coarse to time anything against. Use the *Now() pair below to measure.
    qulonglong framesSent() const { return framesSent_; }
    // The engine's frame counters read AT THE MOMENT OF THE CALL — two atomic
    // loads (the live loop's own publish points), no lock, no PAL, no device, so
    // a binding or a test timer may call them (tools/check_gui_thread_blocks.mjs:
    // a lone singleton acquire, and nothing blocking behind it). They exist for
    // tools/selftest/run_live_latency_test.sh, which times GO LIVE -> first frame
    // and the live cadence as slides/blocks grow: framesSent() lags those
    // measurements by up to a poll (100 ms), which is a fifth of the budget it is
    // asked to police.
    //
    // Two counters because the loop publishes twice per iteration: PREVIEW moves
    // the moment the main pass has written the live preview (the earliest thing
    // the operator can SEE — the first frame), FRAMES only after every per-output
    // pass has landed too (the whole iteration — the cadence).
    Q_INVOKABLE qulonglong previewFramesNow() const;
    Q_INVOKABLE qulonglong framesSentNow() const;
    QString onAirTitle() const { return onAirTitle_; }
    int onAirIndex() const { return onAirIndex_; }
    int onAirTotal() const { return onAirTotal_; }
    QVariantMap onAirSlide() const { return onAirSlide_; }
    QVariantMap onAirSlideRaw() const { return onAirSlideRaw_; }
    QVariantMap stagedSlide() const { return stagedSlide_; }
    QVariantMap stagedSlideRaw() const {
        return stagedSlidesRaw_.isEmpty() ? QVariantMap{} : stagedSlidesRaw_.first().toMap();
    }

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
    QString mediaFit() const { return mediaFit_; }
    QString defaultMediaFit() const { return defaultMediaFit_; }
    bool mediaHasAlpha() const { return mediaHasAlpha_; }
    QString mediaMeterLabel() const;
    // No lock inside any of these: the pill's four fields are the GUI thread's
    // own state and the frame counter is an atomic the feed thread publishes
    // (see the members). They are read from QML bindings, so they must not wait
    // on anything.
    bool ndiSending() const;
    QString ndiSendingOutputName() const;
    qulonglong ndiFramesSent() const;
    bool ndiWarming() const;
    QString ndiWarmingOutputName() const;
    QVariantMap ndiOutputStates() const { return ndiOutputStates_; }
    // The feed thread's send: sender-create (lazy) + the SDK send, from an
    // immutable latest-frame snapshot. Static; the shared frame keeps the
    // pixels alive without copying them on the GUI thread. Provider is a raw
    // pointer, resolved on the GUI thread when the plan was published; the
    // provider itself is internally mutex'd and thread-safe.
    static void ndiWorkerSendFrame(void *provider,
                                   std::shared_ptr<const bps::rendering::Frame> frame,
                                   QString senderName, float fps);
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
    // WHY this label's HELD tap will never produce a frame, in the platform's own
    // words — or "" while it is fine, including merely warming up. The third state
    // the card's state window needs: "connecting" (held, reachable, no frames
    // yet) and "can't reach" (the device is gone) must not swallow "the tap
    // died", which used to sit on the connecting ring for ever. Empty for a label
    // that is not held, and for NDI.
    Q_INVOKABLE QString inputFailure(const QString &label, const QString &kind) const;
    // THE STATUS LIGHT'S PROBE (see EngineBridge::inputSourceAlive): the same
    // reachability question, ALLOWED TO ASK — a cached-roster read first, and a
    // queued device refresh when the cache cannot answer, so a bus card's
    // connection light stays true without anyone opening the Audio & Video
    // screen. inputHealthy() (the slash state) deliberately keeps the quiet,
    // cache-only behaviour: it is asked per held card and must never enumerate.
    Q_INVOKABLE bool inputAvailable(const QString &label, const QString &kind);
    // THE STATUS LIGHT'S LIVE HALF (see EngineBridge::inputDelivery): "" while the
    // app is not pulling this source at all, "ok" while it is and data is
    // arriving, "starved" while it is and nothing has arrived for a few seconds.
    // Answered from the taps the app ALREADY holds — a card's thumbnail, a held
    // monitor, the output take, a meter tap — so asking never opens a device.
    Q_INVOKABLE QString inputDelivery(const QString &label, const QString &kind);

    // ---- BUS VIDEO WINDOWS (a "bus" block on the on-air slide) -----------
    // A design block of kind "bus" is a WINDOW onto one of the mix buses: the
    // canvas draws that bus's live feed inside it, so every real output has to
    // draw the same feed — otherwise NDI and HDMI show an empty hole where the
    // operator sees video. QML owns the answers (which bus the block names,
    // which source is routed into it), so this service resolves them off the
    // ON-AIR composition, holds one owner-"bus" tap per feed and hands each
    // bus's decoded frames to the engine (CompositorState, which SceneBuilder
    // paints at the block's own rect). One path for camera, screen AND NDI —
    // the PAL tap route the taken input uses only knows the first two.
    // Frames and taps follow the composition: a bus block that leaves the
    // slide releases its tap on the next poll. Releasing everything at once
    // (off air, app teardown) is clearBusFeeds().
    Q_INVOKABLE void clearBusFeeds();

    // ---- MEDIA ON AIR ----------------------------------------------------
    // takeMedia(path, name): a video/image file plays into the compositor
    // layer on every monitor tile (under the on-air content, over the style
    // background — the taken-input convention). Videos start playing
    // immediately; taking a different file swaps the layer. clearMedia()
    // takes it off. Audio rides the player's own output (the machine's mix =
    // the program mix the loopback tap meters).
    Q_INVOKABLE void takeMedia(const QString &path, const QString &name);
    Q_INVOKABLE void clearMedia();
    // Update the current file's optional fill override and apply it to the
    // on-air frame. An empty value returns the file to the Outputs default.
    Q_INVOKABLE void setMediaFit(const QString &fit);
    Q_INVOKABLE void setDefaultMediaFit(const QString &fit);
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
    Q_INVOKABLE void stageSlidesWithPreview(const QString &name, const QVariantList &slides,
                                            const QVariantMap &styledPreview);

    // ---- LINE STEP (the "telecaster" reading step: Alt+Down / Alt+Up) -------
    //
    // A NEXT SLIDE replaces the picture; a LINE STEP scrolls the words. Pressing
    // Alt+Down takes the line at the top of the box out and brings the next one
    // up from below it, so a reader who has just finished the last line on
    // screen carries on reading downward instead of having to find their place
    // again in a fresh slide. Alt+Up walks back.
    //
    // NOT A WINDOW OF TEXT. Every earlier version cut the page at the stepped
    // line and refilled the box from the next slide, which re-ran the fit solver
    // on each press (the type size and the wrapping changed while the words were
    // supposed to travel) and left nothing below the last line to scroll in. The
    // page is now left exactly as the operator built it: the reading block gets
    // the PASSAGE it begins in (see readingFlowText), and a step is an offset
    // into that one layout — the engine draws it (Slide::flowText), the monitor
    // tile draws it (DesignPreview), and both read one number.
    //
    // The pick (a show's slides, a passage's verses, a Table section) is the
    // unit: stageSlideSet() hands over the WHOLE set plus which slide is being
    // read. A slide with no neighbouring line in that direction is the end of
    // the step — the caller then falls back to plain next/previous slide
    // behaviour.
    Q_INVOKABLE void stageSlideSet(const QString &name, const QVariantList &slides, int index);
    // Queues a raw-text source; lineStep activates it, while slide picks stay slides.
    Q_INVOKABLE void setReadingTextSource(const QString &name, const QVariantList &pages, int index);
    // Returns true when the words moved (a new offset, and the page re-staged or
    // live-committed where the step crossed into the next page).
    Q_INVOKABLE bool lineStep(int direction);
    // ---- WHAT THE FLOW IS (the surfaces that DRAW it read these) -----------
    // `readingFlowText` is the passage the reading block is a page of — this page
    // followed by the next one, joined by the page break — and `readingScroll` is
    // where it rests or is travelling to, in LINE HEIGHTS of the reading text.
    // DesignPreview draws exactly this text, clipped to the block's own box and
    // translated by `readingScroll` lines; the engine draws the same passage at
    // the same offset on the HDMI window, the NDI feed and every recording. Two
    // views of ONE number, instead of two animations trying to agree.
    Q_PROPERTY(QString readingFlowText READ readingFlowText NOTIFY readingScrollChanged)
    // HOW MANY TIMES A PAGE WAS STAGED OUTRIGHT — a click, a ‹ › step, a search
    // pick — as opposed to the reading step walking lines inside the page it is
    // already on. The two must be told apart by the surfaces: a WALK holds one
    // type size for the whole passage (re-fitting it page by page is the template
    // changing under the reader), while a JUMP is a new page arriving and gets its
    // own auto-size, which is what the operator expects to see when they double-
    // click a paragraph ("I expected autosize to show and the font size to [fit]
    // the text area"). DesignPreview latches its solved size against this counter
    // for exactly that reason.
    Q_PROPERTY(int readingStageSerial READ readingStageSerial NOTIFY readingStageChanged)
    // THE READER'S WORD BUDGET — the per-tab "Max words per slide" setting (default
    // 40), handed here by the pane so the surfaces can solve the reading column
    // against IT rather than against whatever words a page happens to carry.
    //
    // Why the budget and not the page: a page of many words is fitted smaller than a
    // short one, so the same passage changed type size page by page — the scroll
    // changing under the reader, and a wordy page (47-0412) overflowing its box on
    // the outputs. Fitting the BUDGET gives one size per template box for every page,
    // and no page is longer than the budget, so nothing can overflow it. Slides keep
    // splitting exactly as they do; this number only tells the fit what "a full
    // screenful" is.
    Q_PROPERTY(int readingWordBudget READ readingWordBudget WRITE setReadingWordBudget
               NOTIFY readingWordBudgetChanged)
    // SLIDE / SCROLL for the wall's arrows and the output built from a pick. On: a
    // table pick is fed its chapter passage (scroll architecture); off: its slide set.
    Q_PROPERTY(bool scrollMode READ scrollMode WRITE setScrollMode NOTIFY scrollModeChanged)
    Q_PROPERTY(bool readingBudgetFitActive READ readingBudgetFitActive
               NOTIFY readingStageChanged)
    // ...AND THE OPERATOR'S OWN JUMPS, which is what the stage serial above CANNOT
    // tell apart from a page turn: the walk's own follow re-stages the page it
    // crosses into (measured: readingStageSerial climbing 10, 11, 12 ... once per
    // press), so keying the reading column on it made every paragraph boundary look
    // like a jump and re-solved the type mid-scroll. This counter moves ONLY where
    // the operator acts — a click on a paragraph, the pane's ‹ ›, a search pick (see
    // the noteOperatorJump calls in ReferencePane) — so a walk keeps the column it
    // started with and a deliberate jump still fits the page it lands on.
    Q_PROPERTY(int readingJumpSerial READ readingJumpSerial NOTIFY readingStageChanged)
    Q_PROPERTY(qreal readingScroll READ readingScroll NOTIFY readingScrollChanged)
    // THE POSITION EVERY STYLE AGREES ON: the character offset into
    // readingFlowText that the line at the top of the box begins with. Unlike
    // readingScroll (lines of the ACTIVE style's grid), this means the same WORDS
    // on every surface: a tile whose style wraps differently turns it into its own
    // line with readingLineForStyle below. Fires with readingScrollChanged (it is
    // derived from it, once per frame), so a binding on THIS property re-runs
    // through a glide and a binding on readingScroll does not have to.
    Q_PROPERTY(qreal readingChar READ readingChar NOTIFY readingScrollChanged)
    qreal readingChar() const { return readingCharForScroll(readingScroll_); }
    bool readingFlowActive() const { return !readingFlowText_.isEmpty(); }
    QString readingFlowText() const { return readingFlowText_; }
    qreal readingScroll() const { return readingScroll_; }
    // THE PAGE THE WALK IS ON — an index into the pick. NOT the same thing as
    // `readingScroll`, which is measured from the RUN's first line: the run is a
    // WINDOW that slides as the reader walks (kReadingRunRadius pages each way),
    // so the scroll number is not comparable across a window shift. A surface
    // that COMPOSES the picture rather than drawing the flow needs this one: it
    // recomposes when the PAGE changes and stays put for every step inside one
    // page. Its own notify signal (not readingScrollChanged) so reading it does
    // not make a binding re-run on every frame of a glide.
    Q_PROPERTY(int readingPage READ readingPage NOTIFY readingPageChanged)
    int readingPage() const { return lineSlideIndex_; }
    // ...and WHERE IN THE PICK the words are, in lines: the page's own start (the
    // line counts of every page before it) plus the offset inside that page.
    // Window-independent BY CONSTRUCTION, which is what makes it the number a
    // trace can compare across a window shift — a backward move or a jump in
    // THIS value is a visible skip, while the same move in `readingScroll` is
    // just the window moving under a picture that did not. Read by
    // tools/selftest/run_line_step_test.sh, which samples it per frame.
    Q_PROPERTY(qreal readingAbsoluteLine READ readingAbsoluteLine NOTIFY readingScrollChanged)
    qreal readingAbsoluteLine() const;
    // The step's pulse, still emitted once per applied step and still carrying
    // the direction (+1 = the words travel up, the next line arrives from below;
    // -1 = back down). A surface that only needs to know "it moved" reads this;
    // a surface that draws the move reads `readingScroll` above.
    // The pick the reading step is walking. A surface uses it to tell "the same
    // passage, another page" from "a new pick" — see the monitor tile's pinned
    // composition: while a walk runs, the page turning underneath must not
    // re-compose the picture.
    Q_PROPERTY(QString readingSetName READ readingSetName NOTIFY stagedChanged)
    Q_PROPERTY(int lineStepSerial READ lineStepSerial NOTIFY lineStepChanged)
    Q_PROPERTY(int lineStepDirection READ lineStepDirection NOTIFY lineStepChanged)
    // (Getter placement note: the Q_PROPERTY above names it READ.)
    QString readingSetName() const { return lineSetName_; }
    int readingStageSerial() const { return readingStageSerial_; }
    int readingJumpSerial() const { return readingJumpSerial_; }
    int readingWordBudget() const { return readingWordBudget_; }
    bool readingBudgetFitActive() const { return readingBudgetFitActive_; }
    void setReadingWordBudget(int words);
    bool scrollMode() const { return scrollMode_; }
    void setScrollMode(bool on);
    // The operator's own stage (a click, the arrows, a search pick): the walk may
    // then re-solve its column for the page it lands on. Called from QML only.
    Q_INVOKABLE void noteOperatorJump();
    int lineStepSerial() const { return lineStepSerial_; }
    int lineStepDirection() const { return lineStepDirection_; }
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
    // ---- BLACKOUT (the wall toolbar's own control) ------------------------
    // "Nothing at all on screen", as a deliberate gesture AGAIN. Clear all used
    // to send a clearStage carrier and painted the screens empty; it now keeps
    // the output's own look (see clearOnAirSlide), which left the app with no
    // way to ask for a truly empty screen at all. This is that control, and it
    // is separate on purpose: clearing CONTENT and hiding the picture from an
    // audience are different actions, and only one of them should be able to
    // put a blank screen up in the middle of a service.
    //
    // It remembers what was on air first (the whole raw set, so ‹ › still works
    // after the content comes back) and switches the output to the clearStage
    // carrier. The engine composes that as an explicitly empty frame, so the
    // HDMI window, the NDI feed and the monitor camera all go to nothing — the
    // same carrier a saved show can already carry (ShowConverter).
    Q_INVOKABLE void blackout();
    // The way OUT of a blackout: puts back exactly what blackout() hid, through
    // the same goLiveWithSlides path the content arrived by (a live replacement,
    // so the session, its NDI sender and the outputs never restart). With
    // nothing remembered — the carrier came from a saved show, or the session
    // predates the control — the honest restoration is the output's own look,
    // the same carrier GO LIVE with nothing staged sends.
    Q_INVOKABLE void endBlackout();
    // Is THIS output's program feed stalled? The monitor tile asks by roster
    // name (its own row) so only the tile whose feed died goes dark — the pill
    // above names every output that is in the same state.
    Q_INVOKABLE bool outputFeedStalled(const QString &outputName) const;
    // The getters behind the feed-health properties (see sampleFeedHealth).
    bool feedStalled() const { return !stalledOutputs_.isEmpty(); }
    QString feedStallLabels() const { return stalledLabels_; }
    QString feedNoticeText() const { return feedNoticeText_; }
    QString feedNoticeKind() const { return feedNoticeKind_; }
    int feedHealthRev() const { return feedHealthRev_; }
    // ---- CLEAR ALL / SHOW ALL (the wall toolbar's pair) -------------------
    // ONE entry point for the toolbar's Clear all, so the action and its undo
    // cannot drift apart: it snapshots every layer it is about to take down
    // (the on-air set, or the staged pick when nothing was live; the taken
    // input; the overlay stack) and then runs the same clears the button used
    // to call one by one. It deliberately never touches live_/STOP — that is
    // a separate control now (see stop()).
    Q_INVOKABLE void clearAll();
    // "Show all": puts back what the last clearAll() took down — the content
    // whole (the WHOLE on-air set, not just the slide that was showing, so a
    // restored passage keeps its ‹ › navigation), the taken input and the
    // overlay stack. If the snapshot was on air it goes back on air; if it
    // was only being previewed it goes back to the preview slot (stop()'s
    // own convention), so a restore never puts frames on a real screen by
    // itself. Consumes the snapshot — it is a one-shot undo, and the next
    // Clear all replaces it. A no-op when there is nothing to restore.
    Q_INVOKABLE void restoreCleared();
    // The runtime's Next()/Previous() while live.
    Q_INVOKABLE bool next();
    Q_INVOKABLE bool previous();
    Q_INVOKABLE bool jumpTo(int index);
    // The live group-shortcut jump (Settings · Look & Feel's "Manage
    // groups" — a group's own shortcut letter calls this with its name):
    // the next visible slide carrying this tag, cycling. false when
    // nothing is live or no slide carries it.
    Q_INVOKABLE bool jumpToGroup(const QString &tag);

    // The preview toolbar's ‹ ›: advance within the on-air set when it has
    // somewhere to go; at either end (or on a single-slide pick) the step
    // falls through to passageStepRequested — the content tab that owns the
    // air re-picks the neighbouring passage and replays it (the same path
    // its own pills take), so the toolbar behaves like the preview pane
    // instead of going dead on a one-slide passage.
    //
    // A STAGED pick (stageSlides() — what a slide double-click leaves on the
    // tile before GO LIVE commits it) is content too: the wall's toolbar is on
    // screen for it (anythingOnAir counts staged), so ‹ › steps the passage it
    // came from exactly as it does once live. It used to require live_, which
    // made both arrows do nothing in the state an operator is in most of the
    // time ("fix the < > buttons so they work to change the slide").
    Q_INVOKABLE void stepPassage(int direction);

    // Is the STAGED pick THIS title's own content? The pane's own ‹ › asks
    // that of the pick it is stepping (stepAndPlay) — a double-click STAGES,
    // so live-only ownership let a step walk the pane's selection while the
    // tile and the wall transport stayed on the previous passage.
    Q_INVOKABLE bool stagedIsOurs(const QString &title) const;

    // ---- The wall transport's steppers -------------------------------------
    // The wall's ‹ › change whatever is on the Main Output tile, and WHETHER a
    // press can change it is TAB knowledge: a reference pane re-picks the
    // neighbouring passage, the show pane steps the show's own slides. Every
    // tab that owns the tile's content ANNOUNCES that here, so the wall can
    // keep both arrows dim unless a press would really move something — the
    // reported "an arrow is lit and does nothing" (a live blank, or a staged
    // slide no tab owned: the arrows were gated on "something is on the tile",
    // which is not the same question).
    //
    // The one case needing no announcer is the wall's own in-set navigation
    // (a live set with more than one slide), which the toolbar derives from
    // onAirTotal itself — see MonitorWall.qml's canStep.
    Q_INVOKABLE void setWallStepper(const QString &id, bool active);
    Q_PROPERTY(bool wallSteppable READ wallSteppable NOTIFY wallStepperChanged)
    bool wallSteppable() const;
    // WHICH TAB OWNS THE ARROWS — the id that tab announced itself with (a pane's
    // contentType: "table", "scripture" — or "show"). A surface needs it to keep
    // per-TAB state on the content the arrows actually act for: the wall's
    // Slide/Scroll mode is remembered per tab, and "the tab I am looking at" is not
    // the same question as "the tab whose pick is on the tile". Empty when nobody
    // claims the steppers.
    Q_PROPERTY(QString wallStepperId READ wallStepperId NOTIFY wallStepperChanged)
    QString wallStepperId() const { return wallStepperId_; }

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
    // ---- WHAT A GIVEN STYLE DRAWS AT THE CURRENT STEP ----------------------
    //
    // The step's own arithmetic is measured against the ACTIVE style: the line
    // counts, the page bases and `readingScroll` are all lines of THAT picture.
    // A monitor tile (and a real output) draws with ITS OWN style, at its own
    // type size and its own wrap — so the same line number is only the same
    // WORDS if the two grids agree. This asks the question directly, for one
    // style: the RUN's lines as that style lays them out, so the caller can
    // compare "the line at the top of the box" in two grids (the reading step's
    // own cross-output check, and what tools/selftest/run_table_walk_test.sh
    // measures when the outputs carry different styles).
    //
    // Read-only and self-contained: it composes the current page at that style
    // to find the box, wraps the passage the step is drawing in it, and reports
    // the lines. Nothing about the walk's own state changes.
    Q_INVOKABLE QStringList readingRunLinesForStyle(const QString &styleId) const;
    // THE SAME POSITION, IN THAT STYLE'S OWN LINES: the line index (plus the
    // fraction of the move in flight) where character `charAt` of the passage sits
    // when `styleId` lays it out. This is what a surface that draws at its own
    // style must use — the engine's outputs do the same arithmetic in C++
    // (TextObject::FlowLineForChar). Without it, two outputs of different type
    // sizes put different sentences at the top of their boxes at the same instant
    // (measured: a line or two apart on a third of the presses).
    Q_INVOKABLE qreal readingLineForStyle(const QString &styleId, qreal charAt) const;
    // THE TYPE SIZE THE PASSAGE'S TABLE IS MEASURED AT, reported by the surface that
    // DRAWS the reading column (DesignPreview calls this from its fit solve). The
    // surface's solver is the design's own, so it is the authority: a table measured
    // at a size the picture does not have is a different grid from the words on
    // screen (measured 44.48 px drawn against the page's 40 px, and 69 table lines
    // against 75 drawn). A changed number for the REFERENCE (active) style re-seats
    // the walk, because every base and offset in this service is counted in that
    // grid. Ignored when <= 0 (nothing solved yet).
    //
    // The BOX comes with it for the same reason one step further out: the two sides
    // can pick different blocks out of the same style (measured 150x41 on the surface
    // against 754x124 here), and the wrap width IS the box.
    Q_INVOKABLE void noteReadingFit(const QString &styleId, qreal size,
                                    qreal boxWidth, qreal boxHeight, qreal pad);
    // The character starts of that style's run lines — the table the two functions
    // above read. Exposed for the walk's own diagnostic (and cheap: cached).
    Q_INVOKABLE QVariantList readingRunLineStarts(const QString &styleId) const;
    // THE SIZE THAT TABLE WAS LAID OUT AT, for a style the walk has measured (-1
    // when it has not). A surface comparing its own drawn column against the table
    // needs it: the same words at a different pixel size are a different number of
    // lines, so a mismatch here is the whole explanation for words that lag the
    // step. Diagnostic only — the table is the authority.
    Q_INVOKABLE qreal readingRunSizeForStyle(const QString &styleId) const;
    // WHERE THE READER IS, INDEPENDENT OF ANY STYLE: the CHARACTER offset into
    // readingFlowText of the line at the top of the box, in the reference (active
    // style) grid. This is the number every surface can agree on — see the
    // readingRunLinesForStyle comment and Slide::flowChar's own note.
    //
    // RUN-RELATIVE, deliberately: the engine and the tiles draw THIS text, so the
    // offset they are handed has to be an offset into it. A run window that slides
    // (a long pick, see kReadingRunRadius) therefore makes this number go BACKWARDS
    // at the moment it slides — the words it indexes have moved forward with it, so
    // the picture does not, but the number alone cannot be compared across that
    // instant. readingPassageChar below is the coordinate for that comparison.
    Q_INVOKABLE int readingCharOffset() const;
    // THE SAME POSITION IN THE PICK'S WHOLE PASSAGE — the run-relative offset above
    // plus the characters of every page before the run's window (readingRunCharBase_).
    // It is the coordinate a cross-press comparison needs, and the walk harness reads
    // it per output per press (tools/selftest/run_table_walk_test.sh).
    //
    // WHAT IT IS NOT: proof that a move was continuous. readingRunCharBase_ is
    // rebuilt from the pages' own reading text as the window slides, so a walk that
    // changes the pick's page set (the Table refills a passage's pages as it is
    // read) moves this number for reasons that have nothing to do with the drawn
    // picture. Treat a move as a finding to investigate — the harness prints the
    // page, the window and the words beside it — not as the position itself.
    Q_INVOKABLE int readingPassageChar() const;
    // Compose a staged slide with one output's own saved style, for its local
    // monitor tile (the tile must not borrow the currently active output style).
    Q_INVOKABLE QVariantMap previewWithStyle(const QVariantMap &slide, const QString &styleId) const;

    // The last preview frame for the image provider (scaled to the request).
    QImage previewFrame(const QSize &requested);

    // The taken media's last decoded frame (the mediaplay provider reads it;
    // empty when nothing is on air / not yet decoded).
    const QImage &lastMediaFrame() const { return lastMediaFrame_; }

signals:
    void liveChanged();
    void frameRevChanged();
    void onAirChanged();
    // Any of the feed-health properties above changed (a stall started or
    // cleared, or the recovery notice expired).
    void feedHealthChanged();
    // A row's measured runtime state moved (see outputRuntime) — once per
    // probe, not once per frame.
    void outputRuntimeChanged();
    void stagedChanged();
    // One applied line step (see lineStepSerial / lineStepDirection).
    void lineStepChanged();
    // ...and a page STAGED OUTRIGHT (see readingStageSerial): a jump, not a step.
    void readingStageChanged();
    void readingWordBudgetChanged();
    void scrollModeChanged();
    // The reading passage, or where it sits, changed — every frame of a step
    // (see readingFlowText / readingScroll). The surface that draws the step
    // binds to `readingScroll`, so this fires at the animator's own cadence.
    void readingScrollChanged();
    // The walk moved to another PAGE of the pick (see the readingPage property
    // above). Emitted only when the page index actually changes — a step inside
    // one page is not a page change.
    void readingPageChanged();
    // Emitted at the top of lineStep, before it reads the walk's state, so a pane
    // that owns the pick can hand the walk its own passage first (synchronous).
    void lineStepPending(int direction);
    void inputChanged();
    void overlaysChanged();
    // clearAll() took a snapshot / restoreCleared() consumed it.
    void clearedChanged();
    // NDI program-sender state (started/stopped, frame counter refresh).
    void ndiChanged();
    // Media-on-air state changes (take/clear/play-pause/mute/end-of-file)
    // and the ~30 fps position/frame tick while a video plays.
    void mediaChanged();
    void mediaTick();
    // stepPassage() reached the edge of the on-air set: the tab whose content
    // IS on air should re-pick the neighbouring passage (direction -1/+1).
    void passageStepRequested(int direction);
    // The set of tabs claiming they can step the tile's content changed (see
    // setWallStepper): the wall's ‹ › relight themselves from this.
    void wallStepperChanged();

private:
    explicit LiveOutputService(QObject *parent = nullptr);

    static LiveOutputService *s_instance;

    void pollTick();   // refresh onAir* + framesSent from the engine
    // Create (once) and arm the 10 Hz poll. Called from the CONSTRUCTOR: it
    // publishes frameRev for the IDLE preview too, so it must not be something
    // a live session switches on (see pollTick's head).
    void ensurePollTimer();
    // The shared input pump tick (~15 Hz while ANY tap is held): flips
    // inputLive on the taken input's first frame (decode-cache poll) and
    // bumps inputRev so every consuming Image re-fetches. Stops itself when
    // neither a take nor a card preview is held.
    void pumpTick();
    // A split tap OPEN came back refused (EngineBridge::previewRefused — see
    // §3.9/§6.3 of docs/gui_thread_audit.md): the bridge accepted the claim on
    // this thread and opened the device on the engine worker, so the refusal
    // arrives a beat after the take/pip was published. Unwind that here —
    // clearInput() for the output take, endPreviewInput() for a card preview —
    // instead of leaving a warm-up placeholder that can never fill.
    void onPreviewRefused(const QString &label, const QString &kind);
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

    // ---- takeMedia's off-thread preflight (see the hop in takeMedia) -------
    // The half of a take that touches the DISK: path resolution, existence,
    // the extension classification and — for a still — the decode itself. A
    // plain VALUE (no QObject, no signal, no live member), so it is built on
    // the worker thread and handed to the interface thread to apply. A mixed
    // result is meaningful: `found && !recognized` is the "unrecognized
    // extension" refusal, and `recognized && image.isNull()` is the "decoded
    // nothing" still that still goes on air frameless (the tile's placeholder
    // stays honest) — exactly the states the old synchronous body had.
    struct MediaPreflight {
        QString clean;             // resolved local path
        QString name;              // display name ("draws from" clean when empty)
        bool found = false;        // the file exists
        bool recognized = false;   // the extension is one of the three families
        bool isVideo = false;
        bool isImage = false;
        bool isAudio = false;
        QImage image;              // the decoded still (image takes only)
        // The still carries REAL transparency (a pixel with alpha < 255), which
        // is what the monitor tile's checkerboard convention stands for. An
        // alpha CHANNEL is not enough: a photo saved as PNG/ARGB is opaque
        // everywhere, and reading hasAlphaChannel() alone would have put the
        // checkers in the letterbox margins of every such file.
        bool hasAlpha = false;
    };
    void applyMediaTake(const MediaPreflight &pre);
    // The disk half itself (a static: it reads no member, so the worker half of
    // a take has nothing of this object to touch but the generation counter).
    static MediaPreflight preflightMediaTake(const QString &path, const QString &name);
    // Re-reads the runtime's current slide into onAirSlide_ (onAirChanged
    // piggybacks the emit). The slide's blocks change without the title or
    // index moving (a document edit re-synced while live), so the 10Hz poll
    // keeps the QML preview honest rather than keying on title/index only.
    void refreshOnAirSlide();
    // Every visible slide of the on-air presentation in the SAME raw
    // (unstyled) shape onAirSlideRaw_ carries. clearAll() snapshots the WHOLE
    // set rather than the current slide, so a restored passage comes back with
    // its own navigation instead of collapsing to the verse that was showing.
    QVariantList onAirSlidesRaw() const;
    // The blank slide GO LIVE's fallback sends when there is no open show: EMPTY
    // blocks on a transparent background, which the active style still paints —
    // deliberately, because going live with nothing staged is meant to show the
    // output's own look rather than a black screen. Clear all sends this same
    // carrier (see clearOnAirSlide): the clear is about the CONTENT.
    void goLiveBlank();
    // Clear all's content carrier: `goLiveBlank()` — empty blocks (nothing to
    // show) on a transparent background, which the active style still paints.
    // The operator's clear takes the CONTENT down and keeps the output's own
    // look, background image included, so a cleared screen is never a black
    // hole where the branded backdrop used to be. (The `clearStage` carrier that
    // paints nothing at all is now its OWN gesture — see blackout() — so
    // clearing content and hiding the picture never share a control again.)

    bool live_ = false;
    // Tab id -> "this tab owns the tile's content and can step it" (see
    // setWallStepper). GUI thread only: every writer is a QML pane, and the
    // property is read from QML bindings — no lock, and none is needed.
    QHash<QString, bool> wallSteppers_;
    // The tab the ‹ › act for (see wallStepperId): the last one to claim, and when
    // it withdraws, any other tab still claiming.
    QString wallStepperId_;
    qulonglong frameRev_ = 0;
    qulonglong framesSent_ = 0;
    // The engine's EARLY publish — the live preview is already on the wire
    // while the engine's per-output passes are still running (see the two
    // counters in pollTick: that is the difference between a monitor preview
    // that follows a click and one that queues behind the whole frame).
    qulonglong previewFrames_ = 0;
    QString onAirTitle_;
    int onAirIndex_ = -1;
    int onAirTotal_ = 0;
    QVariantMap onAirSlide_;
    QVariantMap onAirSlideRaw_;

    // ---- stageSlides() — see the header comment on stagedSlide ----
    QString stagedName_;
    QVariantList stagedSlidesRaw_;
    QVariantMap stagedSlide_;

    // ---- the line stepper (see stageSlideSet/lineStep) --------------------
    // The set the step walks, and WHERE it is: which page is on screen and how
    // many of that page's lines have already left at the top (offset 0 is the
    // plain page; offset == its line count means the NEXT page's first line has
    // taken the top of the box, and the page change is committed as the move
    // lands).
    QString lineSetName_;
    QVariantList lineSetRaw_;
    bool lineSourceTextOnly_ = false;
    bool readingBudgetFitActive_ = false;
    QString pendingTextSourceName_;
    QVariantList pendingTextSourcePages_;
    int pendingTextSourceIndex_ = 0;
    int lineSlideIndex_ = 0;
    int lineOffset_ = 0;
    // Counts applied steps and remembers which way the last one went — see the
    // Q_PROPERTYs above (the surfaces' animation pulse).
    int lineStepSerial_ = 0;
    int lineStepDirection_ = 1;
    int readingStageSerial_ = 0;
    int readingJumpSerial_ = 0;
    // "Max words per slide" for the tab that staged the pick (see readingWordBudget).
    int readingWordBudget_ = 40;
    bool scrollMode_ = false;
    // THE PASSAGE the reading block is a page of (see the Q_PROPERTYs above) and
    // where it rests, in line heights of the reading text.
    QString readingFlowText_;
    qreal readingScroll_ = 0.0;
    // THE RUN THE SCROLLER DRAWS — the page being read plus kReadingRunRadius
    // pages on each side, joined into ONE passage. `readingScroll_` is measured
    // from the RUN's first line, never from a page's, so a page turn is only a
    // pointer move: the words on screen keep their place while the reference,
    // the slide number and the engine's current slide change around them. That
    // is what "the scroller does not need to know about slides" means in code —
    // it draws `readingFlowText_` at `readingScroll_`, full stop.
    int readingRunFirst_ = -1;             // first slide in the run, -1 = none
    int readingRunLast_ = -1;              // last slide in the run
    QString readingRunText_;               // the run, joined by page breaks
    int readingRunBaseLine_ = 0;           // run start -> the CURRENT page's first line
    // Where the run's own text begins in the pick's whole passage, in characters —
    // the stable half of readingPassageChar (see its note). Moved by
    // rebuildReadingRun as the window slides, never reset by it.
    int readingRunCharBase_ = 0;
    // The page change a landing step owes (a forward step that crossed a page
    // boundary): committed when the move finishes, so the boundary is ONE move
    // and never a cut followed by a scroll. -1 = nothing owed.
    int readingRebaseSlide_ = -1;
    // The one animator EVERY surface rides: it changes readingScroll_, and its
    // every value is pushed to the engine's live loop (SetLineScroll) — so the
    // tile and the HDMI/NDI frame are two views of the same number instead of
    // two animations that have to agree.
    QVariantAnimation *readingScrollAnim_ = nullptr;
    // How many pages the run holds on EACH side of the page being read. The
    // user's own number ("build like 4 lists ahead... both for forward and
    // backward movement"), and the same radius prewarmReadingLines uses.
    static constexpr int kReadingRunRadius = 4;
    // Drops the run (a new pick: its pages are not this pick's pages).
    void resetReadingRun();
    void activatePendingTextSource();
    // Builds the run around `anchor` if the current window does not already hold
    // it. WHEN THE WINDOW SHIFTS IT REBASES readingScroll_ by exactly the lines
    // added or dropped at the front, so the words on screen do not move: the
    // offset and the column change by the same amount, which is the same picture.
    // `force` RE-MEASURES the run it already holds. The window's TEXT does not
    // change when a surface reports a new size/box for the same column, so the
    // early return below would keep the line counts and the base derived from the
    // OLD measurement. A re-report is exactly that case: same window, new grid.
    void rebuildReadingRun(int anchor, bool force = false);
    // Lines from the run's first page to `index`'s first line.
    int readingBaseForSlide(int index) const;
    // The reading line THIS page's first line sits at, in the walk's OWN
    // numbering — carried, not summed: the sum would have to measure every page
    // before it (each one a composed picture plus a text layout, and a Table pick
    // can run to a hundred paragraphs), while this is updated by the same cached
    // counts the run's own base uses, at the only moments the page index moves.
    //
    // Its ORIGIN is wherever the walk began, not necessarily the pick's first
    // page: the two numbers that read it (readingAbsoluteLine and
    // readingWindowOffset) are one another's inverse and only ever compare against
    // each other, so a walk that started at page 12 is as consistent as one that
    // started at 0 — which is what keeps a re-picked mid-passage start free of
    // any "measure everything above me" cost.
    int readingPickBase_ = 0;
    // Moves that origin with the page index (from -> to), by the cached line
    // counts of the pages in between (a step normally crosses one; an empty page
    // LineWalk steps over contributes nothing).
    void shiftPickBase(int from, int to);
    // ---- THE MEASUREMENT BASIS CHANGED: re-seat the walk -------------------
    //
    // Every line count this service steps by is measured on a COMPOSED PICTURE
    // (readingLines -> previewWithActiveStyle), and that picture belongs to the
    // ACTIVE output style. A style edit, a different active style or a roster
    // change re-measures every page at a new box and a new type size — while the
    // counts already in readingLinesCache_ were only ever cleared for a NEW PICK
    // (see its three call sites). Kept, they answer for a layout that no longer
    // exists: the step's own bound (`ownLines`) and the window's bases would come
    // from the old picture, so the words would move by the wrong amount — or a
    // boundary would land early — with nothing on the key to explain it. That is
    // the same family as the re-base the reading RUN exists to remove, so it gets
    // the same treatment: no number the step runs on may belong to the previous
    // composition.
    //
    // THE WALK RE-SEATS ON THE PAGE IT IS ALREADY ON. The counts are dropped and
    // re-measured, the offset is CLAMPED into the new page's line count (a reader
    // keeps their place wherever the new layout still has one), the pick's origin
    // becomes that page (the pages above it were summed in the old numbering, and
    // re-measuring all of them is the O(pick) walk the origin exists to avoid),
    // and the page is re-staged so every surface draws the new composition — all
    // of which applyReadingRestage already does for a page change.
    void reseadReadingWalk();
    // OutputListModel does not exist yet when this service is built (AGENTS.md: a
    // service may precede the models), so the basis connections are made on the
    // first poll tick that finds it — once.
    void connectReadingBasis();
    bool readingBasisConnected_ = false;
    // THE INVERSE, and the reason it exists: a position in the PICK, turned into
    // the window offset that SHOWS it. A glide's start has to be derived this way
    // whenever the branch that started it moved the run's window, because
    // `readingScroll_` is measured from the window's first line and the window may
    // have just slid — a raw copy of it then means a different line (measured by
    // tools/selftest/run_line_step_test.sh as a 3-line jump in a single backward
    // press).
    qreal readingWindowOffset(qreal absolute) const;
    // Announces the page the walk is on to the surfaces that COMPOSE the picture
    // (readingPageChanged), once, and only when it really changed. Called after
    // the page has been staged/committed: a surface that recomposes on the
    // announcement must find the NEW page already in place, or it caches the old
    // page's composition under the new page's key and stays one page behind
    // forever (exactly what the wall did — see the call sites).
    void announceReadingPage();
    int readingPageAnnounced_ = -1;
    void applyReadingScroll(qreal lines);
    void animateReadingScroll(qreal from, qreal to);
    // Settles a page change a landing step owes (readingRebaseSlide_), the
    // same commit readingScrollAnim_'s own finished handler makes when a
    // step's glide completes naturally. Also called synchronously at the
    // top of lineStep(): a rapid second press stops the in-flight animation
    // before it reaches finished() (QAbstractAnimation::stop() does not
    // emit it), so a press that interrupts a page-boundary-crossing step
    // must settle that owed commit itself or it is lost — lineOffset_ is
    // then stuck one past the slide's last line forever, and every
    // following step's own bounds check silently fails (reported:
    // stepping freezes after a fast double press). A no-op when nothing
    // is owed.
    void commitPendingRebase();
    // Re-stage the page being read with its passage and offset. `commitPage` is
    // false for a step INSIDE one page: on air that needs no new presentation at
    // all (the offset travels on its own), and only a PAGE change has to be
    // committed for the reference/heading around the words to follow.
    // `forceRebuild` is for a measurement change on the SAME page (see
    // rebuildReadingRun's own note): the page is re-staged so the engine draws the
    // same words in the new grid, but the walk's page, offset and pick origin are
    // left exactly where the reader was.
    void applyReadingRestage(bool commitPage = true, bool forceRebuild = false,
                             bool forceScrollNotify = false);
    // A slide's READING lines — one entry per line ON SCREEN, not per authored
    // newline: the step moves by what the operator can see, so the text is
    // wrapped at the box the composed picture draws it in (see the .cpp).
    QStringList readingLines(const QVariantMap &slide) const;
    // Same answer, for the slide AT this index in the current pick
    // (lineSetRaw_) — cached, so the SAME index is never composed/wrapped
    // twice. Every real call site works in terms of an index into
    // lineSetRaw_ anyway (see the .cpp callers), which is also what makes a
    // stable cache key possible without hashing the slide's own content.
    QStringList readingLinesAt(int index) const;
    // Populates readingLinesCache_ for a few slides on EITHER side of
    // lineSlideIndex_ (prewarmReadingLinesRadius), so a step that reaches
    // one of them finds its wrapped-line count already known instead of
    // composing the picture and wrapping the text at that exact moment —
    // the readingLines() work (previewWithActiveStyle + the real text
    // layout) is real enough that doing it cold, synchronously, in the
    // frame a step lands, is itself a plausible source of the residual
    // boundary stutter (user call: "if the s[l]ide can be build like 4
    // list[s] ahead... both for forward and backward movement"). Queued via
    // QTimer::singleShot(0, ...) from applyReadingRestage so it runs on the
    // NEXT event-loop turn — after the step that triggered it has already
    // painted — rather than adding its own cost to that press's response.
    void prewarmReadingLines();
    mutable QHash<int, QStringList> readingLinesCache_;
    // One style's view of the run: its wrapped lines AND where each one starts in
    // the passage (the character starts that make the offset style-independent).
    struct StyleRunLines {
        QStringList lines;
        QVector<int> charStarts;
    };
    // readingRunLinesForStyle's own answers, keyed by (style, page whose box is
    // used, the run's text). The key IS the content, so nothing invalidates it.
    mutable QHash<QString, StyleRunLines> readingRunLinesCache_;
    // LRU ORDER for the cache above, oldest at the front — see
    // styleRunLinesForText's own comment on why a bare size cap that CLEARS
    // the whole table on overflow is the wrong shape for a cache read every
    // animation frame: with 2+ outputs/styles live, a few page turns fill
    // it, and the clear then makes the very next per-frame read pay a full,
    // expensive re-measure synchronously, mid-glide — a stall on the step
    // right after whichever one happened to tip the count over (reported:
    // "it scrolls well for the first 3... then it freezes"). Evicting just
    // the one least-recently-used entry keeps the table warm instead.
    mutable QStringList readingRunLinesLru_;
    // The cached entry for a style, computing it on a miss. styleRunLines is this
    // on the run being drawn; styleRunLinesForText asks about another passage text
    // (the one a window move is ABOUT to install — see rebuildReadingRun).
    const StyleRunLines &styleRunLines(const QString &styleId) const;
    const StyleRunLines &styleRunLinesForText(const QString &styleId, const QString &text) const;
    // ---- THE STEP'S UNIT IS THE PASSAGE'S OWN LINE -------------------------
    //
    // A page's line count has TWO honest answers: the lines its text takes at the
    // size IT is fitted to (readingLinesAt — the page as the design draws it alone),
    // and the lines those same words take in the PASSAGE, which is laid out as one
    // column and therefore at one fitted size for the whole run. The step moves
    // readingScroll_, which is an index into the passage's table, so only the second
    // answer keeps its arithmetic and the drawn words in step. Measured on a real
    // walk (ReadRunShape): the per-page counts said 28 lines where the passage had
    // 17 for the same seven pages — the step was walking a coordinate 1.6x too large,
    // which is why the words stopped tracking the keys after a few pages.
    //
    // Both maps are rebuilt with the run and are empty when there is no run (the
    // fallback is the page's own count, which is all a pick with one page has).
    QHash<int, int> readingRunPageStart_;   // page index -> its text's char offset in the run
    QHash<int, int> readingRunPageLines_;   // page index -> its lines in the passage's table
    // WHICH JUMP-EPOCH EACH STYLE'S COLUMN WAS SETTLED IN (see noteReadingFit).
    //
    // Not cleared by resetReadingRun: that runs INSIDE rebuildReadingRun for every
    // page re-cut, i.e. right before each page's own report — so a marker cleared
    // there was always absent when the next page asked to be held, and the hold
    // never fired (measured: zero `held=YES` while ReadFitNote kept printing
    // `resetsRun=YES` once per press). The stage serial changes only when a page is
    // OPERATOR-STAGED outright (readingJumpSerial: a click, the pane's ‹ ›, a search
    // pick), which is exactly the moment a column should be allowed to solve again.
    mutable QHash<QString, int> readingFitAcceptedJump_;
    // THE WALK'S ONE TYPE SIZE PER STYLE (see styleRunLinesForText): solved by the
    // first page the passage was measured on and held for the rest of the walk, so a
    // page turn cannot re-flow the column. Cleared by resetReadingRun (a new pick, or
    // a style change — a new column solves its own size).
    mutable QHash<QString, double> readingRunSizeForStyle_;
    // ...and what the SURFACE reported for that style (see noteReadingFit) — the
    // size it draws the column at and the box it draws it in — which wins over the
    // solve above whenever it is present.
    struct ReadingFit {
        double size = 0.0;
        double width = 0.0;
        double height = 0.0;
        double pad = 0.0;
    };
    mutable QHash<QString, ReadingFit> readingFitReported_;
    // The step's answer for one page (see above): the passage's count when the page
    // is in the run, the page's own count otherwise.
    int readingPageLineCount(int index) const;
    // The characters of the pages in [from, to) as the run's own join counts them
    // (a non-empty page plus its break) — how readingRunCharBase_ is moved when the
    // run's window slides, and computed from scratch for a fresh run.
    int readingCharSpan(int from, int to) const;
    // The character starts of one style's run lines — the table readingLineForStyle
    // maps through. Shared with readingRunLineStarts (the QML side of it).
    QVector<int> readingRunCharStarts(const QString &styleId) const
    {
        return styleRunLines(styleId).charStarts;
    }
    // The reference-grid line number turned into the passage's character offset
    // (plus the fraction of the move in flight) — see applyReadingScroll.
    qreal readingCharForScroll(qreal lines) const;

    // ---- Clear all's undo (see clearAll/restoreCleared) -------------------
    // A plain snapshot of each content layer at the moment Clear all ran.
    // `valid` IS the gate (canRestoreCleared). The content is held as ONE
    // slot — the on-air set when something was live, the staged pick
    // otherwise — because that is exactly what the user was looking at, and
    // it is what stop() folds into the staged slot when it goes off air.
    struct ClearedSnapshot {
        bool valid = false;
        bool contentWasOnAir = false;   // restore re-sends it live vs re-stages it
        QString contentTitle;
        QVariantList contentSlidesRaw;  // whole visible set, raw (unstyled) maps
        QVariantMap contentStyled;      // the styled map the preview slot keeps
        QString inputLabel;
        QString inputKind;
        QString inputMode;
        // The media file that was on air (its path is also the id of the take).
        // MEDIA IS A COMPOSITOR LAYER LIKE THE INPUT, and Clear all did not touch
        // it: the file stayed on the monitors while every other layer went — the
        // report "when I press clear all it doesn't clear the media".
        QString mediaPath;
        QString mediaName;
        QVariantList overlays;          // [{id,name}], insertion order = z-order
    };
    ClearedSnapshot cleared_;
    bool canRestoreCleared() const { return cleared_.valid; }

    // ---- Feed health (see sampleFeedHealth) --------------------------------
    // The 1 Hz probe behind the automatic feed blackout and the header pill.
    // Called from pollTick (the 10 Hz timer) and self-throttled to ~1 Hz. A
    // plain engine read: buffer names and atomic stamps, no frame copy and no
    // device, so the interface thread spends microseconds on it.
    void sampleFeedHealth();
    void setFeedNotice(const QString &text, const QString &kind);
    // Roster names (not labels) of the outputs whose feed is stalled, the "M1,
    // M2" line built from the SAME rows, and the pill's own text/kind.
    QSet<QString> stalledOutputs_;
    // What the SAME probe measured for every row — see the outputRuntime
    // property. Value maps only; no engine handles are kept (a buffer can be
    // gone by the next probe).
    QVariantMap outputRuntime_;
    QVariantMap outputRuntime() const { return outputRuntime_; }
    QString stalledLabels_;
    QString feedNoticeText_;
    QString feedNoticeKind_;
    int feedHealthRev_ = 0;
    // Milestones on steady time: when this session went live (the warm-up grace
    // that keeps go-live itself from reading as a stall) and the last probe.
    qint64 liveSinceMs_ = 0;
    qint64 lastFeedProbeMs_ = 0;
    // Holds "Feed restored" for a few seconds after a stall clears.
    QTimer *feedNoticeTimer_ = nullptr;

    // ---- The blackout's own undo (see blackout/endBlackout) ----------------
    // What the clearStage carrier is HIDING: the on-air set as raw (unstyled)
    // maps, exactly the shape onAirSlidesRaw() carries, so endBlackout can send
    // it back through goLiveWithSlides without restyling it twice. Empty while
    // no blackout of this session's own is up (a carrier from a saved show has
    // nothing to restore, and endBlackout falls back to goLiveBlank).
    //
    // Cleared whenever OTHER content goes on air (goLiveWithSlides), so a stale
    // set can never be staged by a later stop() or put back over a newer pick.
    QString blackoutRestoreTitle_;
    QVariantList blackoutRestoreSlides_;
    // Is THIS session's blackout up? Set the moment the carrier is sent and
    // cleared when content (or STOP) takes over. `blackedOut()` also reads the
    // on-air slide's own flag — the engine's answer, and the only one an older
    // session or a saved show has — but that answer arrives one poll tick later
    // (a live replacement is applied by the render worker), and a second press
    // inside that window must not mistake the blackout for content and save the
    // CARRIER as what to restore.
    bool blackoutActive_ = false;
    // Read straight off the on-air slide's own clearStage flag — the engine's
    // answer, not a local guess — and only while live (off air nothing is on a
    // screen to be blacked out).
    bool blackedOut() const;

    QTimer *poll_ = nullptr;   // while live: onAir/frames refresh at 10Hz
    // Off air: asks the engine to keep previewing the open show (see
    // LiveOutputController::StartIdlePreview), so the Settings resource meters
    // stay live with a show open. A no-op while live or before boot.
    QTimer *previewTimer_ = nullptr;
    std::unique_ptr<LivePreviewProvider> provider_;

    // ---- overlays-on-air state ----
    // {id,name} maps, insertion order = z-order (last taken = topmost).
    QVariantList activeOverlays_;

    // Each enabled NDI-kind output gets an independent sender and frame plan;
    // per-row sending/warming/count state is published through ndiOutputStates.
    //
    // SPLIT ALONG THE THREAD BOUNDARY (docs/gui_thread_audit.md §6.6). The
    // roster read (a QML model), the bridge's PUBLISHED NDI verdict and the
    // buffer NAME stay on this thread; the ENGINE WALK — the display provider
    // and the output's frame buffer, each behind its own engine lock — runs on
    // the pool, and its answer comes back here to publish the plan and fire the
    // signals (see applyNdiPlanProbe). One walk at a time: the plan is a value
    // the feed thread reads, so a late answer is simply the next poll's answer.
    void recheckNdiPlanOffThread();
    void connectToNdiOutputRoster();
    bool ndiRosterConnected_ = false; // GUI thread, singleton may be created after this service
    // WHICH roster object this feed is following (GUI thread). OutputListModel
    // can hand its singleton over to the object QML actually built (main.cpp's
    // one-object rule), so the connection made at construction can end up
    // pointing at a roster nothing edits any more. Every poll compares this
    // against OutputListModel::instance() and re-connects when they differ —
    // otherwise a roster change is only noticed by the poll's own re-read, one
    // tick late, and a signal-driven refresh never arrives at all.
    class OutputListModel *ndiRosterModel_ = nullptr;
    // The enabled NDI rows that existed when this session went live, and the
    // ones added DURING it that have since started sending (both GUI thread).
    // An output added mid-session gets one "is live — on the network now" note
    // the first time its sender carries a frame; the rows present at GO LIVE
    // stay silent, because seven toasts on air is noise rather than news.
    QSet<QString> ndiSessionNames_;
    QSet<QString> ndiAnnouncedNames_;
    // ---- And the ones that never start -------------------------------------
    // An output added mid-session that does NOT start sending is the other half
    // of the same report ("I created a new output … it's not being pushed"):
    // before this, it said so only in a log line the operator never sees. Each
    // mid-session row is watched from the moment it is seen, and once the grace
    // below elapses without a frame on the wire it gets ONE warning that names
    // what it is actually waiting on (no buffer yet / buffer held off / no frame
    // rendered / switched off on the Outputs screen). GUI thread only.
    QHash<QString, qint64> ndiSilentSinceMs_;   // first seen not-sending
    QSet<QString> ndiSilentWarned_;             // one warning per output
    // Why 6 s: the lazy NDI SDK sender can take a few hundred ms to open, and a
    // styled output's frame buffer needs a render pass before it holds a frame —
    // so this is long enough to never fire on a healthy start, short enough that
    // a dead output is reported while the operator is still looking at it.
    static constexpr qint64 kNdiSilentWarnMs = 6000;
    void warnSilentNewOutput(const QString &name, const QString &reason);
    QString ndiRosterLogSignature_; // GUI thread: suppress duplicate roster diagnostics
    QString ndiProbeLogSignature_;  // GUI thread: log probe transitions, not every poll
    // The walk's answer, on the interface thread: install the plan, or the
    // warm-up/teardown state, exactly as the old inline resolution did. `bufferOk`
    // is whether the output's buffer resolved at all, `enabled` whether it is
    // enabled, `hasFrame` whether it has rendered anything yet — three different
    // states the UI reports per output through ndiOutputStates.
    struct NdiProbeResult {
        QString outputName;
        QString senderName;
        float fps = 30.0f;
        std::shared_ptr<bps::display::NdiDisplayProvider> provider;
        std::shared_ptr<bps::rendering::FrameBufferOutput> buffer;
        bool bufferOk = false;
        bool enabled = false;
        bool hasFrame = false;
    };
    void applyNdiPlanProbe(const std::vector<NdiProbeResult> &results);
    std::atomic<bool> ndiPlanProbeBusy_{false};   // one engine walk in flight at a time
    QVariantMap ndiOutputStates_;                 // GUI thread: name -> {sending, warming, frames}
    QHash<QString, std::shared_ptr<bps::display::NdiDisplayProvider>> ndiProviders_; // GUI thread owns each output sender
    void stopNdiFeed();
    // The network-visible NDI source name for an output row: "<AppName>-<Row
    // name>" when the row has a name, or
    // "" to fall back to the provider's own default. With no argument it
    // resolves the selected (or fallback enabled) NDI row itself.
    static QString ndiSenderDisplayName(const QString &rowName = QString());
    // NDI sender identity per live session: the NDI SDK can't rename a live
    // sender, so a name change (or a new session) must recreate it. Called
    // from goLive's paths; tracks the name it last armed.
    void armNdiSenderForSession();
    QString ndiArmedName_;

    // ---- bus video windows (see clearBusFeeds above) ----
    struct BusFeed {
        QString label;      // the roster caption the tap is keyed by
        QString kind;       // "camera" | "screen" | "ndi"
        QString mode;       // a camera's capture-mode pick ("" for the others)
        QString deviceId;   // the resolved PAL id / NDI source name, for the log
        // The cacheKey of the last frame this feed put in the compositor. The
        // pump runs at the monitor refresh now (it used to run at 15 Hz, which
        // is what made a 30/60 fps camera stutter on EVERY output — the
        // compositor only saw one frame in four), so it must not copy a 1080p
        // picture per tick when the tap has nothing new: same key, no work.
        qint64 lastFrameKey = 0;
    };
    // Bus number (a block's meta busId) → the feed held for it. GUI thread only,
    // like every other tap table here: written by refreshBusFeeds() (the 10Hz
    // poll) and read by pumpTick on the same thread.
    QHash<int, BusFeed> busFeeds_;
    // "kind|label" → earliest ms to retry a feed whose tap was refused. The
    // poll runs at 10Hz; a device that is gone must not be re-probed (or
    // re-logged) at that rate.
    QHash<QString, qint64> busTapRetry_;
    // Diffs the ON-AIR composition's bus blocks against the held feeds and
    // starts/releases taps accordingly. Cheap and idempotent — it is called from
    // the live poll, so a re-route, a roster rename or a new slide all settle
    // within one tick instead of needing an event nobody publishes.
    void refreshBusFeeds();
    // Releases one bus's tap (owner "bus") and drops its frames from the
    // compositor, so the plane goes blank rather than freezing on its last
    // picture.
    void releaseBusFeed(int busId);
    // Mirrors the held feeds' identity (device id + kind) into CompositorState —
    // the engine half of the "bus window is empty on air" diagnostic.
    void publishBusFeeds();

    // ---- taken-input state ----
    QString inputLabel_;
    QString inputKind_;
    // The camera capture mode a take was asked for ("" for screen/NDI, whose
    // takes carry none) — kept ONLY so Clear all's undo can re-take the same
    // device the same way; nothing else reads it.
    QString inputMode_;
    qulonglong inputRev_ = 0;
    bool inputLive_ = false;
    // True while an NDI take is feeding CompositorState's media layer (the
    // frame side of the take — the PAL-side taken-input identity is
    // ClearTakenInput, this is the pixels side).
    bool ndiInputLive_ = false;
    QTimer *inputPump_ = nullptr;   // while taken: bus/input frames into the compositor
    // The taken input's last pushed frame (cacheKey), for the same reason
    // BusFeed::lastFrameKey exists — a stationary source costs nothing.
    qint64 inputFrameKey_ = 0;
    // Per-label card thumbnail identity (see pumpTick): the cacheKey of the
    // decode-once frame a held card is showing. A card's Image binds its URL
    // nonce to inputRev, so the rev must bump when a CARD's picture changes —
    // including a card-only hold, where no bus or take exists to move.
    QHash<QString, qint64> cardFrameKeys_;
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
    QString mediaFit_;   // effective per-file override or shared Outputs default
    QString defaultMediaFit_ = QStringLiteral("contain");
    bool mediaFitUsesDefault_ = true;
    // Real transparency in the picture on air (see the mediaHasAlpha property).
    bool mediaHasAlpha_ = false;
    // Whether THIS take's alpha is already known: a still decides it at take
    // time (the decode scanned its pixels), a video at its first frame — the
    // scan runs once, never per frame.
    bool mediaAlphaDecided_ = false;
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
    // A take's GENERATION. takeMedia() bumps it on the interface thread before
    // it hands the disk work off; the worker reads it (the only member of a
    // take that is ever read off the interface thread — hence the atomic) and
    // the apply half re-checks it there before touching a member. clearMedia()
    // bumps it too: a decode that lands after the layer was cleared must not
    // put itself back on air.
    std::atomic<quint64> mediaTakeToken_{0};

    // ---- NDI program sender (see the properties above) --------------------
    // THE PILL'S STATE BELONGS TO THE GUI THREAD, AND NOTHING ELSE TOUCHES IT.
    // ndiSending_ / ndiSendingOutput_ are written there (there IS something to
    // feed from the moment a plan carrying a frame is published) and read there
    // (the five Q_PROPERTY reads above run from QML bindings); no other thread
    // reads or writes them, so there is nothing to synchronize and nothing a
    // reader can wait for. The mutex that used to sit here was guarding against
    // a cross-thread read that does not exist — the one piece that DOES cross
    // threads is the frame counter, and that is an atomic.
    bool ndiSending_ = false;                    // GUI thread only
    QString ndiSendingOutput_;                   // GUI thread only (the pill's LIVE row)
    // True between "an NDI row wants feeding" and "first frame is on the wire".
    bool ndiWarming_ = false;                    // GUI thread only
    QString ndiWarmingOutput_;                   // GUI thread only
    // Update the warming state and notify ONLY on a real change (the send
    // tick would otherwise emit ndiChanged 30-60x/s for a static value).
    void setNdiWarming(bool on, const QString &name);
    // The feed's TOTAL frames on the wire: the SUM of every sender's own
    // counter, published by the poll (applyNdiPlanProbe) from the counters it
    // already reads per output. It used to be written by the feed thread from
    // whichever sender had just sent — so with more than one NDI output it
    // reported the LAST sender's count, not the feed's, and the rate watch
    // compared one sender's counter against another's (a healthy seven-output
    // feed read as "no progress for two windows" and reset a sender at random).
    // Per-output counts — what the Settings · Outputs pill shows per row — are
    // ndiOutputStates, published from the same probe.
    std::atomic<qulonglong> ndiFramesSent_{0};
    // ---- The plan: WHAT to send, HANDED to the feed thread -----------------
    // A value copy — never a pointer into the roster or the model — so the feed
    // thread holds nothing the GUI thread can invalidate under it, and the
    // buffer is a shared_ptr (the render registry owns the real one), so a plan
    // outliving its output cannot leave the feed thread reading freed pixels.
    struct NdiSendPlan {
        QString outputName;
        std::shared_ptr<bps::display::NdiDisplayProvider> provider;
        std::shared_ptr<bps::rendering::FrameBufferOutput> buffer;
        QString senderName;
        float fps = 30.0f;
        bool operator==(const NdiSendPlan &o) const {
            return outputName == o.outputName && provider == o.provider && buffer == o.buffer
                   && senderName == o.senderName && fps == o.fps;
        }
    };
    // THE FEED THREAD'S OWN COPY — installed by the queued hand-over in
    // setNdiPlan/clearNdiPlan, read by ndiWorkerTick on that thread, touched by
    // nothing else. That is why there is no mutex: the value is not shared, it
    // is given away (and the equal-plan short-circuit below means a steady feed
    // hands over nothing at all).
    std::vector<NdiSendPlan> ndiFeedPlan_;                // feed thread only
    // The last plan handed over, so a poll that changes nothing posts nothing
    // (the 10 Hz poll republishes the same plan otherwise).
    std::vector<NdiSendPlan> ndiSentPlan_;               // GUI thread only
    bool ndiPlanPublished_ = false;                      // GUI thread only
    void handOverNdiPlan(const std::vector<NdiSendPlan> &plans);
    // One-shot send diagnostics (moved onto the worker with the send): the
    // gate warning, the first SendFrame failure, and the "frames flowing,
    // monitor discovering" note — each logs once per output per outage, not per
    // tick. PER SENDER, not one flag for the whole feed: with a single flag only
    // the first output to send ever announced itself, so "New Screen 3, 4, 5 …"
    // reaching the wire was invisible in the log (and a failure on a later
    // sender was swallowed for the rest of the session).
    bool ndiGateLogged_ = false;      // GUI thread only
    QSet<QString> ndiSendFailedNames_;   // feed thread only
    QSet<QString> ndiFirstFrameNames_;   // feed thread only
    QSet<QString> ndiSlowNames_;         // feed thread only
    QHash<QString, int> ndiFastStreaks_; // feed thread only
    bool ndiWaitLogged_ = false;      // GUI thread only; "waiting for buffer/frame" note
    // Backpressure is the feed thread's own pacing now: it clocks each send
    // (feed-thread only, and only ever touched there) and refuses a tick that
    // arrives inside the previous interval, so a slow send lowers the real rate
    // without ever lying about the advertised one — and without touching the
    // GUI thread at all.
    QElapsedTimer ndiSendClock_;
    QHash<QString, QElapsedTimer> ndiSendClocks_; // feed-thread pacing, one clock per sender
    // The slow-send incident state is now PER SENDER (ndiSlowNames_ /
    // ndiFastStreaks_ above, feed thread only): one slow output must not
    // suppress — or be suppressed by — another's report, and a sender recovering
    // re-arms only its own note. The GUI clears the sets when the session ends
    // (stopNdiFeed); they are the feed thread's while it runs, and the feed
    // thread is parked by then, so no lock is taken around a send.
    // ---- The NDI feed: WHAT on the GUI thread, WHEN on the feed thread -----
    // The feed's cadence lives on the FEED THREAD (ndiWorker_), never on the
    // GUI thread. It used to be a QTimer on the GUI thread, so every late tick
    // — one slow QML frame, a settings write, a dialog, a library scan — landed
    // on the wire as a HOLE: NDI got a burst of nothing followed by two frames
    // back to back, which is exactly what a receiver shows as dropped frames.
    // A QTimer cannot catch up either, so the feed's real rate wandered below
    // the setting.
    //
    // The split is now explicit and one-way:
    //   * GUI thread (this, at the 10 Hz poll): resolve WHAT to send — the
    //     enabled NDI row, its name, the display provider and the output's own
    //     frame buffer — and PUBLISH it as a plain value (NdiSendPlan, copied
    //     under its own mutex). The roster is a QML model and the engine
    //     singletons are GUI-owned, so this half must stay here.
    //   * Feed thread: owns WHEN. Its own PreciseTimer, its own interval (the
    //     published rate), its own time-since-last-send guard, and it stops its
    //     own clock when the published plan wants nothing. A slow send or a
    //     busy machine can no longer move the cadence; a beat that misses its
    //     slot is skipped, which is the ONLY frame that gets dropped.
    void startNdiSendClock();          // go-live: bring the feed thread up + publish
    void stopNdiSendClock();           // off air: clear the plan, stop its clock
    // The poll's NDI watch — now simply "publish the current plan" (it runs
    // while live, so the roster, the output's buffer and the configured rate
    // all reach the feed within one poll of changing).
    void pollTickRecheckNdi();
    // ---- Output frame-rate drift watch ---------------------------------------
    // The NDI feed is the ONE output whose wire rate is meant to track its
    // configured Refresh rate continuously (the sender re-sends the last frame
    // every clock tick), so an actual send rate well under the setting is real
    // drift worth telling the operator about. Measured as frames-sent per
    // second over a rolling window; a single warning fires only after
    // back-to-back slow windows ("more than a few seconds") and re-arms once
    // the rate recovers, so it can never flap. (Screen/HDMI outputs are NOT
    // watched this way: the live loop is change-driven — a static scene only
    // re-rasterizes on its 1Hz heartbeat — so its frame count is not the
    // output's presented rate. NDI is where the configured rate drives a real
    // stream.) Runs from pollTick while live.
    void checkNdiFeedRate();
    // ONE WINDOW PER OUTPUT. Every enabled NDI row is its own sender with its
    // own configured rate, so a single shared window was measuring the sum (or,
    // worse, one sender's counter against another's) and blaming the wrong
    // output — the reported "only New Screen 2 is ever reported" shape. Keyed by
    // the output's row name, GUI thread only (the probe's published state).
    struct NdiRateWindow {
        QElapsedTimer clock;        // invalid = not sampling
        qulonglong frames = 0;      // last sample of THIS output's counter
        int slowWindows = 0;        // consecutive windows below its setting
        int noProgressWindows = 0;  // consecutive windows with zero successful sends
        bool warned = false;        // one drift warning per slow episode
        int tier = -1;              // -1 unknown, 0 at rate, 1 ~half, 2 lower
        float configured = 0.0f;    // its configured rate last window (re-baseline on change)
    };
    QHash<QString, NdiRateWindow> ndiRateWindows_;   // GUI thread only
    static constexpr int kDriftWindowMs = 4000;
    static constexpr int kDriftWindowsToWarn = 2;
    // The selected NDI output's configured Refresh rate ("60 Hz" → 60).
    // A row with an unusable/blank value feeds at the engine's 30 fps
    // default; 0 means NO enabled NDI row (the send clock stops).
    static float ndiConfiguredFps();
    // The feed's clock: created ON the feed thread (a QTimer belongs to its
    // thread), owned by the worker object, armed from the feed thread itself —
    // the GUI only publishes the plan the feed reads. Touch this ONLY from
    // feed-thread code (ndiWorkerTick and the queued lambda in setNdiPlan).
    QTimer *ndiSendTimer_ = nullptr;   // feed thread only

    // ---- The NDI feed thread --------------------------------------------------
    // One QThread + one worker QObject moved to it, created lazily on the
    // first send and retired at quit (aboutToQuit → shutdownNdiWorker(),
    // which quits the loop and WAITS — the SDK must not be torn down under
    // an in-flight send). It owns the feed's clock and the SDK send.
    QThread *ndiThread_ = nullptr;
    QObject *ndiWorker_ = nullptr;     // lives on ndiThread_
    // GUI-side view of "the feed thread's clock is running". Set when the
    // queued arm is sent, cleared with the plan — it exists only so the 10 Hz
    // poll does not queue a redundant arm on every beat.
    bool ndiClockArmed_ = false;
    // Create the feed thread once (GUI side); the thread's own clock is
    // created on it by setNdiPlan's first published frame.
    void ensureNdiWorker();
    // Publish a plan (GUI side). Only ever a value copy plus, when it wants
    // frames and the clock is not up yet, the queued "arm the clock" — so the
    // GUI never blocks on, or reaches into, the feed thread's own state.
    void setNdiPlan(const std::vector<NdiSendPlan> &plans);
    // Drop the plan (GUI side): an empty plan is what stops the feed thread's
    // own clock on its next beat, so no cross-thread stop call is needed (and
    // none could race the plan it is stopping).
    void clearNdiPlan();
    // One beat of the feed (FEED THREAD): read the plan, keep the clock's
    // interval at the published rate, and send at most one frame per period.
    void ndiWorkerTick();
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
