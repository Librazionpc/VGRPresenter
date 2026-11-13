#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QQuickImageProvider>
#include <QSet>
#include <QObject>
#include <QPointer>
#include "core/threading/QtThread.hpp"
#include <QVideoSink>
#include <QQmlEngine>
#include <QtQml/qqmlregistration.h>
#include <QVariantMap>
#include <QVector>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "modules/display/Providers.hpp"       // NdiDisplayProvider
#include "modules/rendering/RenderOutputs.hpp" // FrameBufferOutput

class LivePreviewProvider;
class LiveMediaFrameProvider;
class QTimer;
class QMediaPlayer;
class QVariantAnimation;
class QAudioOutput;
class QVideoSink;
namespace bps::rendering { struct Frame; }
namespace bps::display { struct NdiRgbaFrame; }
class QThreadPool;

// ONE MEASURED LINE of a passage, as a span of its source text (not a string): where the line starts
// and one past where it ends (the break itself is not included). Shared by the reading run's cache
// (SpansToRunLines below) and the measuring helpers in LiveOutputReading.cpp.
struct VisualSpan {
    int start = 0;   // first character of the line
    int end = 0;     // one past its last character
};

class LiveOutputService : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // On air? (the engine loop is running)
    Q_PROPERTY(bool live READ live NOTIFY liveChanged)
    Q_PROPERTY(QVariantMap stagedSlide READ stagedSlide NOTIFY stagedChanged)
    // Unstyled source for per-output local monitor previews. `stagedSlide`
    // remains the active-output preview used by the main preview pane.
    Q_PROPERTY(QVariantMap stagedSlideRaw READ stagedSlideRaw NOTIFY stagedChanged)
    // Bumped once per staged change. A monitor tile memoises its own composition on this
    // (plus its style) instead of on the slide itself: deciding "same picture" must not
    // cost a read of the slide, which is a JS conversion of the whole passage per handler.
    Q_PROPERTY(int stagedRev READ stagedRev NOTIFY stagedChanged)
    // Is there a reading passage in the staged (or on-air) picture? A monitor tile needs
    // that answer to pick its memo key, and must not convert the slide to find it.
    Q_PROPERTY(bool stagedHasFlow READ stagedHasFlow NOTIFY stagedChanged)
    // ...and is there a staged slide at all (`stagedSlide.valid` without the read).
    Q_PROPERTY(bool stagedValid READ stagedValid NOTIFY stagedChanged)
    Q_PROPERTY(bool onAirHasFlow READ onAirHasFlow NOTIFY onAirChanged)
    Q_PROPERTY(bool canRestoreCleared READ canRestoreCleared NOTIFY clearedChanged)
    Q_PROPERTY(bool blackedOut READ blackedOut NOTIFY onAirChanged)
    // ---- Feed health (the header's stall pill + the automatic blackout) ----
    Q_PROPERTY(bool feedStalled READ feedStalled NOTIFY feedHealthChanged)
    // Which outputs, in the operator's terms: "M1, M2" (M<n> = the n-th ENABLED
    // output in roster order). Empty when nothing is stalled.
    Q_PROPERTY(QString feedStallLabels READ feedStallLabels NOTIFY feedHealthChanged)
    // The header pill's own line: "M1, M2 · No live feed" while stalled,
    // "M1 · Feed restored" for a few seconds after it comes back, "" otherwise.
    Q_PROPERTY(QString feedNoticeText READ feedNoticeText NOTIFY feedHealthChanged)
    // "stalled" | "restored" | "" — picks the pill's colour.
    Q_PROPERTY(QString feedNoticeKind READ feedNoticeKind NOTIFY feedHealthChanged)
    Q_PROPERTY(int feedHealthRev READ feedHealthRev NOTIFY feedHealthChanged)
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
    Q_PROPERTY(QVariantMap onAirSlide READ onAirSlide NOTIFY onAirChanged)
    // Unstyled runtime slide, for per-output monitor tiles to compose with
    // their own saved template. The main preview continues to use onAirSlide.
    Q_PROPERTY(QVariantMap onAirSlideRaw READ onAirSlideRaw NOTIFY onAirChanged)
    // ---- Taken video input (the Media pane's click-to-preview) ----------
    Q_PROPERTY(QString inputLabel READ inputLabel NOTIFY inputChanged)
    Q_PROPERTY(QString inputKind READ inputKind NOTIFY inputChanged)
    Q_PROPERTY(qulonglong inputRev READ inputRev NOTIFY inputChanged)
    Q_PROPERTY(QVariantList cardPreviews READ cardPreviewsList NOTIFY inputChanged)
    // The audio inputs the operator SELECTED in the Media tab (a metered card, or the audio of a
    // monitored bus). It is the only audio the output meters animate for: a microphone that
    // some other page happens to have open (Audio & Video's level bar) is not on air for that.
    Q_PROPERTY(QStringList monitoredAudio READ monitoredAudio NOTIFY monitoredAudioChanged)
    // True while a taken input's tap is producing (warm-up honest: false
    // until the provider's first frame decodes).
    Q_PROPERTY(bool inputLive READ inputLive NOTIFY inputChanged)
    // ---- OVERLAYS ON AIR (the Overlays pane's double-click take) ---------
    Q_PROPERTY(QVariantList activeOverlays READ activeOverlays NOTIFY overlaysChanged)
    // ---- MEDIA ON AIR (the Media pane's take-to-program) ----------------
    Q_PROPERTY(bool mediaOnAir READ mediaOnAir NOTIFY mediaChanged)
    // Absolute file path of the taken media ("" when none).
    Q_PROPERTY(QString mediaPath READ mediaPath NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaName READ mediaName NOTIFY mediaChanged)
    // True when the file is a VIDEO (audio-capable). An image paints a still.
    Q_PROPERTY(bool mediaIsVideo READ mediaIsVideo NOTIFY mediaChanged)
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
    Q_PROPERTY(bool mediaHasAlpha READ mediaHasAlpha NOTIFY mediaChanged)
    Q_PROPERTY(QString mediaMeterLabel READ mediaMeterLabel CONSTANT)
    // ---- NDI PROGRAM SENDER ---------------------------------------------
    Q_PROPERTY(bool ndiSending READ ndiSending NOTIFY ndiChanged)
    Q_PROPERTY(QString ndiSendingOutputName READ ndiSendingOutputName NOTIFY ndiChanged)
    // Frames the sender has pushed (poll-refreshed with the preview feed).
    Q_PROPERTY(qulonglong ndiFramesSent READ ndiFramesSent NOTIFY ndiChanged)
    Q_PROPERTY(bool ndiWarming READ ndiWarming NOTIFY ndiChanged)
    // The roster name of the row being warmed ("" when not warming) — only
    // selected NDI output feeds, so the row can gate on its own name.
    Q_PROPERTY(QString ndiWarmingOutputName READ ndiWarmingOutputName NOTIFY ndiChanged)
    // Per-output NDI state so every enabled NDI row reports its own sender.
    Q_PROPERTY(QVariantMap ndiOutputStates READ ndiOutputStates NOTIFY ndiChanged)

public:
    static LiveOutputService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    static LiveOutputService &instance();
    Q_INVOKABLE void shutdownNdiWorker();
    // Quit path: take the media-on-air player (Windows Media Foundation threads behind it) down
    // while the event loop and the engine are still alive, rather than leaving it to the QML
    // singleton's destructor after Qt's own thread bookkeeping is gone.
    void shutdownMedia();

    bool live() const { return live_; }
    qulonglong frameRev() const { return frameRev_; }
    // The 10Hz poll's CACHED copy (see pollTick): right for a preview refresh,
    // too coarse to time anything against. Use the *Now() pair below to measure.
    qulonglong framesSent() const { return framesSent_; }
    Q_INVOKABLE qulonglong previewFramesNow() const;
    Q_INVOKABLE qulonglong framesSentNow() const;
    // Frames the media decoder has delivered since the app started (counted on the decoder's thread,
    // so it is the SOURCE's rate, whatever the interface thread managed to show), and the frame rate
    // the file itself declares. The video-rate self-test compares the outputs against these.
    Q_INVOKABLE qulonglong mediaFramesDecoded() const { return mediaFramesDecoded_.load(std::memory_order_relaxed); }
    Q_INVOKABLE double mediaSourceFps() const;
    QString onAirTitle() const { return onAirTitle_; }
    int onAirIndex() const { return onAirIndex_; }
    int onAirTotal() const { return onAirTotal_; }
    QVariantMap onAirSlide() const { return onAirSlide_; }
    QVariantMap onAirSlideRaw() const { return onAirSlideRaw_; }
    int stagedRev() const { return stagedRev_; }
    bool stagedHasFlow() const;
    bool stagedValid() const { return stagedSlide_.value(QStringLiteral("valid")).toBool(); }
    bool onAirHasFlow() const;
    QVariantMap stagedSlide() const { return stagedSlide_; }
    QVariantMap stagedSlideRaw() const {
        return stagedSlidesRaw_.isEmpty() ? QVariantMap{} : stagedSlidesRaw_.first().toMap();
    }

    QString inputLabel() const { return inputLabel_; }
    QString inputKind() const { return inputKind_; }
    qulonglong inputRev() const { return inputRev_; }
    QStringList monitoredAudio() const { return monitoredAudio_; }
    Q_INVOKABLE void setMonitoredAudio(const QStringList &labels)
    {
        if (labels == monitoredAudio_)
            return;
        monitoredAudio_ = labels;
        emit monitoredAudioChanged();
    }
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
    bool ndiSending() const;
    QString ndiSendingOutputName() const;
    qulonglong ndiFramesSent() const;
    bool ndiWarming() const;
    QString ndiWarmingOutputName() const;
    QVariantMap ndiOutputStates() const { return ndiOutputStates_; }
    // The frame arrives already copied to alpha-preserving RGBA: the feed tick copies each distinct buffer ONCE
    // and hands the same bytes to every output fed by it (see ndiWorkerTick).
    static void ndiWorkerSendFrame(void *provider,
                                   std::shared_ptr<const bps::display::NdiRgbaFrame> frame,
                                   QString senderName, float fps);
    QVariantList cardPreviewsList() const
    {
        QVariantList out;
        for (const QString &k : cardPreviews_.keys())
            out.append(k);
        return out;
    }

    Q_INVOKABLE void takeInput(const QString &label, const QString &kind, const QString &mode);
    Q_INVOKABLE void clearInput();
    Q_INVOKABLE void clearNdiInput();

    // ---- OVERLAYS ON AIR (multiple, stacked) -----------------------------
    Q_INVOKABLE void takeOverlay(const QString &id, const QString &name);
    Q_INVOKABLE void clearOverlay(const QString &id);
    Q_INVOKABLE void clearAllOverlays();
    Q_INVOKABLE bool overlayIsOnAir(const QString &id) const;

    // ---- INTERNAL CARD PREVIEWS (the Media pane's one-click thumbnails) ---
    Q_INVOKABLE void previewInput(const QString &label, const QString &kind, const QString &mode);
    Q_INVOKABLE void endPreviewInput(const QString &label);
    Q_INVOKABLE bool inputHealthy(const QString &label, const QString &kind) const;
    Q_INVOKABLE bool inputProducing(const QString &label) const;
    Q_INVOKABLE QString inputFailure(const QString &label, const QString &kind) const;
    Q_INVOKABLE bool inputAvailable(const QString &label, const QString &kind);
    Q_INVOKABLE QString inputDelivery(const QString &label, const QString &kind);

    // ---- BUS VIDEO WINDOWS (a "bus" block on the on-air slide) -----------
    Q_INVOKABLE void clearBusFeeds();

    // ---- MEDIA ON AIR ----------------------------------------------------
    Q_INVOKABLE void takeMedia(const QString &path, const QString &name);
    // Take a media file AS A SOURCE: it plays at that source's level (0..100 = linear gain) and mute.
    Q_INVOKABLE void takeMediaAtLevel(const QString &path, const QString &name, qreal levelPercent, bool muted);
    // Change the level/mute of the media on air now (a source edited while its file is playing).
    Q_INVOKABLE void setMediaLevel(qreal levelPercent, bool muted);
    // Show the media on air in a QML VideoOutput (the GPU video path the Media tab's thumbnail uses) fed by
    // the ONE player the app already decodes with — no second decoder, no per-frame image conversion.
    Q_INVOKABLE void attachMediaSink(QVideoSink *sink);
    // The same for the CAMERA or SCREEN that is taken to the output (LiveInputSurface.qml).
    Q_INVOKABLE void attachInputSink(QVideoSink *sink);
    // Frames handed to the attached surfaces since the app started (diagnostics + the self-test).
    Q_INVOKABLE qulonglong inputFramesFed() const { return inputFramesFed_.load(std::memory_order_relaxed); }
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

    static void runEnvSelfTest();
    // VGR_QUIETTIME_TEST: a Quiet Time pick staged and taken, then every output's own style
    // asked for the picture it would draw (see runQuietTimeSelfTestOnce).
    void runQuietTimeSelfTestOnce();
    // VGR_AUTOADVANCE_TEST: a 3-slide show with a 2 s NEXT TIMER each, live, then the wall's timer is
    // PRESSED — the proof that the timer moves the slide (see run_auto_advance_test.sh).
    void runAutoAdvanceSelfTestOnce();

    Q_INVOKABLE void goLive();
    Q_INVOKABLE void goLiveWithSlides(const QString &name, const QVariantList &slides);
    Q_INVOKABLE void stageSlides(const QString &name, const QVariantList &slides);
    Q_INVOKABLE void stageSlidesWithPreview(const QString &name, const QVariantList &slides,
                                            const QVariantMap &styledPreview);

    // ---- LINE STEP (the "telecaster" reading step: Alt+Down / Alt+Up) -------
    Q_INVOKABLE void stageSlideSet(const QString &name, const QVariantList &slides, int index);    // Queues a raw-text source; lineStep activates it, while slide picks stay slides.
    Q_INVOKABLE void setReadingTextSource(const QString &name, const QVariantList &pages, int index);
    // A pick made while Scroll is on: the picked slide and the chapter passage go to
    // the engine as ONE commit (the passage already on the slide), not a slide first
    // and the passage a beat later. Off air, or with Scroll off, it is the two calls.
    Q_INVOKABLE void stagePassagePick(const QString &name, const QVariantList &slides, int index,
                                      const QVariantList &pages, int pageIndex);
    // Returns true when the words moved (a new offset, and the page re-staged or
    // live-committed where the step crossed into the next page).
    Q_INVOKABLE bool lineStep(int direction);
    // ---- WHAT THE FLOW IS (the surfaces that DRAW it read these) -----------
    Q_PROPERTY(QString readingFlowText READ readingFlowText NOTIFY readingScrollChanged)
    Q_PROPERTY(int readingStageSerial READ readingStageSerial NOTIFY readingStageChanged)
    Q_PROPERTY(int readingWordBudget READ readingWordBudget WRITE setReadingWordBudget
               NOTIFY readingWordBudgetChanged)
    // SLIDE / SCROLL for the wall's arrows and the output built from a pick. On: a
    // table pick is fed its chapter passage (scroll architecture); off: its slide set.
    Q_PROPERTY(bool scrollMode READ scrollMode WRITE setScrollMode NOTIFY scrollModeChanged)
    Q_PROPERTY(bool readingBudgetFitActive READ readingBudgetFitActive
               NOTIFY readingStageChanged)
    Q_PROPERTY(int readingJumpSerial READ readingJumpSerial NOTIFY readingStageChanged)
    Q_PROPERTY(qreal readingScroll READ readingScroll NOTIFY readingScrollChanged)
    Q_PROPERTY(qreal readingChar READ readingChar NOTIFY readingScrollChanged)
    qreal readingChar() const
    {
        const qreal local = readingCharForScroll(readingScroll_);
        return lineSourceTextOnly_ ? readingTextWindowCharStart_ + local : local;
    }
    bool readingFlowActive() const { return !readingFlowText_.isEmpty(); }
    QString readingFlowText() const { return readingFlowText_; }
    qreal readingScroll() const { return readingScroll_; }
    Q_PROPERTY(int readingPage READ readingPage NOTIFY readingPageChanged)
    int readingPage() const;
    Q_PROPERTY(qreal readingAbsoluteLine READ readingAbsoluteLine NOTIFY readingScrollChanged)
    qreal readingAbsoluteLine() const;
    Q_PROPERTY(QString readingSetName READ readingSetName NOTIFY stagedChanged)
    Q_PROPERTY(int lineStepSerial READ lineStepSerial NOTIFY lineStepChanged)
    Q_PROPERTY(int lineStepDirection READ lineStepDirection NOTIFY lineStepChanged)
    // (Getter placement note: the Q_PROPERTY above names it READ.)
    QString readingSetName() const { return lineSetName_; }
    int readingStageSerial() const { return readingStageSerial_; }
    int readingJumpSerial() const { return readingJumpSerial_; }
    int readingWordBudget() const { return readingWordBudget_; }
    bool readingBudgetFitActive() const { return readingBudgetFitActive_; }
    Q_INVOKABLE qreal readingColumnPxFor(const QString &styleId) const;
    // What the screens are handed of the passage: the passage and the char the walk is at, with the line
    // breaks each output style cuts it into (cut once at the stage's own size; see readingBreaksFor).
    void attachReadingFlow(QVariantMap &slide, qreal lines) const;
    QVariantMap readingBreaksFor(const QString &text) const;
    // Whole lines the style's text area holds at its column size (0 = not measured yet).
    Q_INVOKABLE int readingVisibleLinesFor(const QString &styleId) const;
    // The part of the passage a surface lays out: lines [firstLine, firstLine + lineCount) of THAT style's
    // table, as {text, first, lines, whole}. Starts at a line start, so it breaks as the whole text does.
    Q_INVOKABLE QVariantMap readingFlowSliceFor(const QString &styleId, int firstLine, int lineCount) const;
    void setReadingWordBudget(int words);
    bool scrollMode() const { return scrollMode_; }
    void setScrollMode(bool on);
    // The operator's own stage (a click, the arrows, a search pick): the walk may
    // then re-solve its column for the page it lands on. Called from QML only.
    Q_INVOKABLE void noteOperatorJump();
    // THE REFERENCE LABEL FOLLOWS THE WALK. The output's label ("Genesis 1:5", "47-1100X - Fellowship 2") is the pick's own
    // reference, fixed when the pick was made — so it stayed "Genesis 1:1" while the passage scrolled on to verse 5 and 6.
    // The pane that owns the pick tells the service which piece the walk is on; the on-air (or staged) slide's reference
    // moves with it, on every output, without being a new pick. A new pick starts again from its own label.
    Q_INVOKABLE void setWalkReference(const QString &reference);
    int lineStepSerial() const { return lineStepSerial_; }
    int lineStepDirection() const { return lineStepDirection_; }
    Q_INVOKABLE void clearStaged();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void clearOnAirSlide();
    // ---- BLACKOUT (the wall toolbar's own control) ------------------------
    Q_INVOKABLE void blackout();
    Q_INVOKABLE void endBlackout();
    Q_INVOKABLE bool outputFeedStalled(const QString &outputName) const;
    // The getters behind the feed-health properties (see sampleFeedHealth).
    bool feedStalled() const { return !stalledOutputs_.isEmpty(); }
    QString feedStallLabels() const { return stalledLabels_; }
    QString feedNoticeText() const { return feedNoticeText_; }
    QString feedNoticeKind() const { return feedNoticeKind_; }
    int feedHealthRev() const { return feedHealthRev_; }
    // ---- CLEAR ALL / SHOW ALL (the wall toolbar's pair) -------------------
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void restoreCleared();
    // The runtime's Next()/Previous() while live.
    Q_INVOKABLE bool next();
    // THE ONE FORWARD / BACKWARD COMMAND. The Next and Prev hotkeys (Main.qml's runEffect), the wall's
    // buttons, the auto-advance timer and the reading walk all take this: in Scroll mode it steps a line,
    // otherwise it takes the next slide. One implementation, so a timer reaching its time moves exactly
    // like a keypress, and a new scroll implementation is written here once instead of per caller.
    Q_INVOKABLE bool stepForward();
    Q_INVOKABLE bool stepBackward();
    Q_INVOKABLE bool previous();
    Q_INVOKABLE bool jumpTo(int index);
    Q_INVOKABLE bool jumpToGroup(const QString &tag);

    Q_INVOKABLE void stepPassage(int direction);

    // ---- AUTO-ADVANCE (the wall's timer button): the on-air set slides by itself ------
    Q_PROPERTY(bool autoAdvancing READ autoAdvancing NOTIFY autoAdvanceChanged)
    bool autoAdvancing() const;
    // The timer is RECORDED WITH THE SHOW it was turned on for (shows.timedShow) and paces that show by
    // its own per-slide NEXT TIMER values (the engine advances a slide on its durationMs) — another show
    // never inherits it, and a show with no times on its slides stays manual.
    Q_INVOKABLE void toggleAutoAdvance();
    // Push the record at the engine: on only for the recorded show, while it is the one on air.
    void applyTimedAdvance();
    // The centre page's NEXT TIMER is written to the SHOW, while the set on air holds its own copy of the
    // slides — so a timer set after GO LIVE never reached the wall's pace. This adopts the show's timers
    // into the on-air pace (recording the show as timed while any slide carries one), then re-arms.
    void adoptShowNextTimers();
    // A slide's next timer (ms) as the SHOW has it now, by slide id: 0 when the show does not know the slide.
    double showNextTimerMsFor(const QString &slideId) const;
    // THE SET the timer is recorded against — the SHOW's name, not the title of the slide showing at the
    // moment of the press (onAirTitle_ follows the slide, and comparing against it disarmed the timer on
    // the first step: the reported "the timer isn't moving the slide").
    QString autoAdvanceShowKey() const;
    // The wall's pace: one shot per slide, armed with that slide's own NEXT TIMER. RE-ARMED on every slide
    // change (applyTimedAdvance calls it), so each slide keeps its own time.
    void armAutoAdvanceTimer();
    // The step: the SAME one the Next button and its hotkeys take — a line in Scroll, the next slide in
    // Slide — so a show scrolls when Scroll is on, without a second stepping path to keep in step.
    void onAutoAdvanceTimeout();

    Q_INVOKABLE bool stagedIsOurs(const QString &title) const;

    // ---- The wall transport's steppers -------------------------------------
    Q_INVOKABLE void setWallStepper(const QString &id, bool active);
    Q_PROPERTY(bool wallSteppable READ wallSteppable NOTIFY wallStepperChanged)
    bool wallSteppable() const;
    Q_PROPERTY(QString wallStepperId READ wallStepperId NOTIFY wallStepperChanged)
    QString wallStepperId() const { return wallStepperId_; }

    Q_INVOKABLE QVariantMap previewWithActiveStyle(const QVariantMap &slide) const;
    // ---- WHAT A GIVEN STYLE DRAWS AT THE CURRENT STEP ----------------------
    Q_INVOKABLE QStringList readingRunLinesForStyle(const QString &styleId) const;
    Q_INVOKABLE qreal readingLineForStyle(const QString &styleId, qreal charAt) const;
    Q_INVOKABLE void noteReadingFit(const QString &styleId, qreal size,
                                    qreal boxWidth, qreal boxHeight, qreal pad);
    Q_INVOKABLE qreal scrollColumnSize(const QString &styleId, const QVariantMap &block);
    // The character starts of that style's run lines — the table the two functions
    // above read. Exposed for the walk's own diagnostic (and cheap: cached).
    Q_INVOKABLE QVariantList readingRunLineStarts(const QString &styleId) const;
    // Just the count (the diagnostics asked for the whole table to read its length).
    Q_INVOKABLE int readingRunLineCount(const QString &styleId) const;
    // True once this style's table is the budget COLUMN's measured one (the table the outputs and the tiles
    // are drawn from during the walk), not the pre-seat grid stand-in. The break check only judges then.
    Q_INVOKABLE bool readingColumnTableMeasured(const QString &styleId) const;
    Q_INVOKABLE int readingFlowCharStart() const { return readingTextWindowCharStart_; }
    Q_INVOKABLE qreal readingRunSizeForStyle(const QString &styleId) const;
    Q_INVOKABLE int readingCharOffset() const;
    Q_INVOKABLE int readingPassageChar() const;
    // Compose a staged slide with one output's own saved style, for its local
    // monitor tile (the tile must not borrow the currently active output style).
    Q_INVOKABLE QVariantMap previewWithStyle(const QVariantMap &slide, const QString &styleId) const;
    // THE STAGED/ON-AIR SLIDE AS ONE OUTPUT STYLE DRAWS IT, composed once per (style, picture) and shared by every
    // surface of that style. A staged change used to recompose it in EACH of them (five output windows, two sets of
    // monitor tiles: fourteen compositions of the same four pictures, 5-40 ms apiece, all inside the one pick), and
    // each surface also pulled the whole slide into JS and back to ask. `pictureKey` is the surfaces' own scalar key
    // for "what is on the picture" (staged revision, stage serial, page, live, media, input); the style roster and the
    // output styles invalidate the cache here. Chooses the slide the way the surfaces do: staged if there is one,
    // else on air.
    Q_INVOKABLE QVariantMap composedForStyle(const QString &styleId, const QString &pictureKey);
    // A surface that is about to re-read after a style change calls this FIRST, so it can never be answered from a
    // composition made under the old styles whatever order the style signals reach the receivers in.
    Q_INVOKABLE void invalidateComposed() { composedCache_.clear(); composedLru_.clear(); }

    // The taken media's last decoded frame (the mediaplay provider reads it;
    // empty when nothing is on air / not yet decoded).
    const QImage &lastMediaFrame() const { return lastMediaFrame_; }
    // The newest media frame, safe to read from the image loader thread: a published immutable copy
    // (QImage is implicitly shared, so publishing costs a refcount, not a pixel copy).
    std::shared_ptr<const QImage> mediaFrameShared() const { return mediaFrameShared_.load(); }

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
    void readingScrollChanged();
    void readingPageChanged();
    // Emitted at the top of lineStep, before it reads the walk's state, so a pane
    // that owns the pick can hand the walk its own passage first (synchronous).
    void lineStepPending(int direction);
    void inputChanged();
    void monitoredAudioChanged();
    void overlaysChanged();
    // clearAll() took a snapshot / restoreCleared() consumed it.
    void clearedChanged();
    // NDI program-sender state (started/stopped, frame counter refresh).
    void ndiChanged();
    // Media-on-air state changes (take/clear/play-pause/mute/end-of-file)
    // and the ~30 fps position/frame tick while a video plays.
    void mediaChanged();
    void mediaTick();
    // A take REFUSED before it started: no enabled video output can receive the media layer, so nothing
    // decoded and nothing is on air. The explanation (and the Routing action) belongs to the UI.
    void mediaTakeBlocked(const QString &path, const QString &name);
    // stepPassage() reached the edge of the on-air set: the tab whose content
    // IS on air should re-pick the neighbouring passage (direction -1/+1).
    void passageStepRequested(int direction);
    // The set of tabs claiming they can step the tile's content changed (see
    // setWallStepper): the wall's ‹ › relight themselves from this.
    void wallStepperChanged();
    // The wall's timer button started/stopped advancing the on-air set by itself.
    void autoAdvanceChanged();

private:
    explicit LiveOutputService(QObject *parent = nullptr);
    ~LiveOutputService() override { stopInputFeed(); }   // the feed thread must never outlive the members it reads

    static LiveOutputService *s_instance;

    void pollTick();   // refresh onAir* + framesSent from the engine
    void ensurePollTimer();
    void pumpTick();
    // takeInput's steps: validate, release, acquire, prime the pump.
    static bool InputKindHasTap(const QString &kind);
    void StopPreviousInputTap();
    bool StartInputTap(const QString &kind, const QString &label, const QString &mode);
    void EnsureInputPump();
    // pumpTick's steps: one per source family, each reporting whether it moved.
    QImage SampleTakenInput();
    bool SampleCardPreviews();
    bool PushTakenInputFrame(const QImage &frame);
    bool PushBusFeedFrames();
    void onPreviewRefused(const QString &label, const QString &kind);
    // The newest frame of a held tap, kind-aware (camera/screen → the PAL's
    // decode cache, ndi → the engine receiver's converter) — see .cpp.
    QImage inputFrame(const QString &label, const QString &kind) const;
    void pushNdiInputFrame(const QImage &frame);
    // Clears that compositor hold when it was an NDI feed that filled it.
    void clearNdiInputFrame();

    // ---- takeMedia's off-thread preflight (see the hop in takeMedia) -------
    struct MediaPreflight {
        QString clean;             // resolved local path
        QString name;              // display name ("draws from" clean when empty)
        bool found = false;        // the file exists
        bool recognized = false;   // the extension is one of the three families
        bool isVideo = false;
        bool isImage = false;
        bool isAudio = false;
        QImage image;              // the decoded still (image takes only)
        bool hasAlpha = false;
    };
    void applyMediaTake(const MediaPreflight &pre);
    void EnsureMediaPlayer();
    void ArmMediaTake(const MediaPreflight &pre);
    void StartMediaPlayback(const MediaPreflight &pre);
    // The disk half itself (a static: it reads no member, so the worker half of
    // a take has nothing of this object to touch but the generation counter).
    static MediaPreflight preflightMediaTake(const QString &path, const QString &name);
    void refreshOnAirSlide();
    QVariantList onAirSlidesRaw() const;
    void goLiveBlank();

    bool live_ = false;
    QHash<QString, bool> wallSteppers_;
    // The tab the ‹ › act for (see wallStepperId): the last one to claim, and when
    // it withdraws, any other tab still claiming.
    QString wallStepperId_;
    qulonglong frameRev_ = 0;
    qulonglong framesSent_ = 0;
    qulonglong previewFrames_ = 0;
    QString onAirTitle_;
    int onAirIndex_ = -1;
    int onAirTotal_ = 0;
    QVariantMap onAirSlide_;
    QString onAirGroupTag_;
    QVariantMap onAirSlideRaw_;

    // ---- stageSlides() — see the header comment on stagedSlide ----
    QString stagedName_;
    QVariantList stagedSlidesRaw_;
    mutable QString lastStagedIsOursLine_;
    QVariantMap stagedSlide_;
    // The revision a surface memoises on (see Q_PROPERTY stagedRev) and the ONE
    // place a staged change is announced from.
    void BumpStaged();
    int stagedRev_ = 0;

    // ---- the line stepper (see stageSlideSet/lineStep) --------------------
    QString lineSetName_;
    QVariantList lineSetRaw_;
    bool lineSourceTextOnly_ = false;
    bool readingBudgetFitActive_ = false;
    QString pendingTextSourceName_;
    QVariantList pendingTextSourcePages_;
    int pendingTextSourceIndex_ = 0;
    // Scroll walks one stable JSON passage. These original word-budget slices
    // remain only to map the current character back to the pane's page/verse.
    QVariantList readingTextSourcePages_;
    QVector<int> readingTextSourcePageEnds_;
    QString readingTextSourceText_;
    int readingTextSourceStartChar_ = -1;
    bool readingTextSourceInitialPositionPending_ = false;
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
    QString readingEngineFlowText_;
    // stagePassagePick: the picked slide is held here instead of being committed, and
    // the passage publish sends it (carrying the flow) as the one engine commit.
    mutable QHash<QString, QVariantList> readingBreaksCache_;   // (column, size, text) -> its line starts
    QTimer *republishTimer_ = nullptr;
    QString lastRepublishSig_;
    bool holdingPassagePick_ = false;
    // While a pick stages its INTERMEDIATE state (the single slide, before its passage is attached) nobody
    // is told: the monitors would re-compose and re-lay-out everything for a state replaced a moment later.
    bool quietStage_ = false;
    bool heldPickSent_ = false;
    QVariantMap heldPickSlide_;
    int readingTextWindowCharStart_ = 0;
    bool readingWindowRefreshPending_ = false;
    bool readingScrollPreparing_ = false;
    int readingLayoutTargetChar_ = -1;
    int pendingReadingSteps_ = 0;
    qreal readingScroll_ = 0.0;
    // When the surfaces were last told the scroll moved, and what that tell cost
    // (see applyReadingScroll: the next one waits out the last one's price).
    mutable QElapsedTimer readingScrollNotifyClock_;
    qint64 readingScrollNotifyCostMs_ = 0;
    // The tell price SMOOTHED (see applyReadingScroll): one slow tell must not freeze every tile for twice its length.
    double readingScrollNotifyAvgMs_ = 0.0;
    // The position the surfaces were last told (-1e9 = never). A tell that says the
    // same number is a full surface re-layout bought for nothing.
    qreal readingScrollNotified_ = -1e9;
    int readingRunFirst_ = -1;             // first slide in the run, -1 = none
    int readingRunLast_ = -1;              // last slide in the run
    QString readingRunText_;               // the run, joined by page breaks
    int readingRunBaseLine_ = 0;           // run start -> the CURRENT page's first line
    int readingRunCharBase_ = 0;
    int readingRebaseSlide_ = -1;
    bool readingStepGlide_ = false;
    QVariantAnimation *readingScrollAnim_ = nullptr;
    static constexpr int kReadingRunRadius = 4;
    // Drops the run (a new pick: its pages are not this pick's pages).
    void resetReadingRun();
    // The whole new-pick reset: run, rebase origin, scroll and its animation.
    void ResetReadingPick();
    // Drops the passage's own text source (pages, offsets, pending activation).
    void ClearReadingTextSource();
    void activatePendingTextSource();
    void rebuildReadingRun(int anchor, bool force = false);
    // Lines from the run's first page to `index`'s first line.
    int readingBaseForSlide(int index) const;
    int readingPickBase_ = 0;
    void shiftPickBase(int from, int to);
    // ---- THE MEASUREMENT BASIS CHANGED: re-seat the walk -------------------
    void reseadReadingWalk();
    void connectReadingBasis();
    bool readingBasisConnected_ = false;
    qreal readingWindowOffset(qreal absolute) const;
    void announceReadingPage();
    int readingPageAnnounced_ = -1;
    void applyReadingScroll(qreal lines, bool notify = true);
    // Tell the surfaces where the walk is — the one place that also remembers the number
    // it told them, so the same position is never laid out twice.
    void AnnounceReadingScroll();
    bool updateReadingPick(const QString &name, const QVariantMap &shown);
    // RE-ATTACH the reading passage to the on-air slide after Slide mode took it
    // off (see setScrollMode). The engine no-ops when the slide already has it.
    void republishReadingFlow();
    // Coalesced: column seats and table measures land in bursts, one hand-off covers them all.
    void scheduleReadingRepublish();
    void drainPendingReadingSteps();
    void animateReadingScroll(qreal from, qreal to);
    void commitPendingRebase();
    void applyReadingRestage(bool commitPage = true, bool forceRebuild = false,
                             bool forceScrollNotify = false);
    QStringList readingLines(const QVariantMap &slide) const;
    QStringList readingLinesAt(int index) const;
    void prewarmReadingLines();
    void schedulePrewarmReadingLines();
    bool readingPrewarmQueued_ = false;
    QVariantMap readingStagedPreview(const QVariantMap &slide);
    QVariantMap readingPreviewRaw_;
    QVariantMap readingPreviewOut_;
    QString readingPreviewKey_;
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
    mutable QSet<QString> readingRunLinesPending_;
    mutable QStringList readingRunLinesLru_;
    const StyleRunLines &styleRunLines(const QString &styleId) const;
    const StyleRunLines &styleRunLinesForText(const QString &styleId, const QString &text) const;
    // ---- THE STEP'S UNIT IS THE PASSAGE'S OWN LINE -------------------------
    QHash<int, int> readingRunPageStart_;   // page index -> its text's char offset in the run
    QHash<int, int> readingRunPageLines_;   // page index -> its lines in the passage's table
    mutable QHash<QString, int> readingFitAcceptedJump_;
    struct ScrollColumn {
        QVariantMap block;
        qreal size = 0.0;
        QString sig;
    };
    QHash<QString, ScrollColumn> scrollColumns_;
    // THE STEP'S TWO CHARACTERS (window-local; -1 = no step in flight). Every output wraps the passage at its
    // own size and keeps that wrap for the whole walk — nothing re-flows on a press. A press only says which
    // two characters the glide runs between; each screen turns them into whole lines of ITS OWN table and
    // glides between those (flowscroll::SnappedLineForChar), so Main, the Lower Third and the engine's
    // outputs all move by whole lines with their breaks where they were.
    QHash<QString, QVariantMap> composedCache_;   // composedForStyle: by style|pictureKey (cleared with the styles)
    QStringList composedLru_;
    bool composedInvalidationConnected_ = false;
    void connectComposedInvalidation();
    qreal readingSpanFrom_ = -1.0;
    qreal readingSpanTo_ = -1.0;
    void prewarmReadingFonts();       // boot-time font warm-up, one output style per event-loop turn
    QStringList readingPrewarmQueue_;
    QElapsedTimer readingPrewarmClock_;
    bool readingPrewarmed_ = false;
    QString walkReference_;   // setWalkReference: the label the walk last moved the pick's reference to
    QSet<QString> inlineSlowStyles_;   // styles whose inline column-size solve was slow: they solve on a worker
    bool unnamedSeatOpen_ = false;    // the grace period for an unnamed ask to seat a vacant column is over
    bool unnamedSeatArmed_ = false;   // ...and its timer is running (see scrollColumnSize)
    QHash<QString, qreal> scrollSizeBySig_;   // block + pick + budget -> solved size
    QSet<QString> scrollSizePending_;          // sigs being solved on a worker (QML retries on readingScrollChanged)
    mutable QHash<QString, double> readingRunSizeForStyle_;
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
    int readingTextSourcePageForChar(int charOffset) const;
    int readingCharSpan(int from, int to) const;
    // The character starts of one style's run lines — the table readingLineForStyle
    // maps through. Shared with readingRunLineStarts (the QML side of it).
    const QVector<int> &readingRunCharStarts(const QString &styleId) const
    {
        return styleRunLines(styleId).charStarts;
    }
    // The reference-grid line number turned into the passage's character offset
    // (plus the fraction of the move in flight) — see applyReadingScroll.
    qreal readingCharForScroll(qreal lines) const;
    bool refreshReadingTextWindow(int passageChar);
    qreal readingFlowCharForScroll(qreal lines) const;

    // ---- the reading run's named steps (LiveOutputReading.cpp) -------------
    QString ScrollColumnKey(const QString &styleId) const;
    const StyleRunLines *CachedStyleRunLines(const QString &key) const;
    const StyleRunLines &CacheStyleRunLines(const QString &key, StyleRunLines entry) const;
    // Wrapped text -> the run lines and their character starts (both cache paths build this).
    static StyleRunLines SpansToRunLines(const QString &text, const QVector<VisualSpan> &spans);
    QVariantMap ReadingBoxForPage(const QString &styleId, QString *fitText) const;
    double ReadingPageSizeFor(const QString &styleId, const QVariantMap &box,
                              const QString &fitText) const;
    void QueueScrollColumnMeasure(const QString &columnKey, const QString &columnCacheKey,
                                  const QString &text, qreal size, const QVariantMap &block,
                                  const QString &sig) const;

    void RebuildReadingRunUnderCommit(bool forceRebuild);
    void DeriveReadingScrollFromRun();
    void LogReadingContinuity(bool flowChanged, int passageBefore, qreal previousScroll);
    void LogReadingRestage(bool commitPage, bool flowChanged, bool scrollChanged);
    void PublishReadingFlowOnly(bool flowChanged, bool scrollChanged, bool pageChanged);
    void CommitReadingPageOnAir(bool flowChanged, bool scrollChanged, bool pageChanged);
    void StageReadingPageOffAir(bool flowChanged, bool scrollChanged, bool pageChanged);

    void BuildReadingRunText(int first, int last);
    void CountReadingRunPageLines();
    void LogReadingRunShape(int first, int last);
    void RebaseReadingWindow(int previousBase, int first, int last);

    bool ReadingFitKeepsWalkColumn(const QString &styleId, const ReadingFit &report,
                                   const ReadingFit &previous, bool havePrevious);
    void PurgeReadingTablesForStyle(const QString &styleId);
    void LogReadingFitHeld(const QString &styleId, const ReadingFit &report, double previousSize);
    void LogReadingFitAccepted(const QString &styleId, const ReadingFit &report,
                               double previousSize, bool resets);

    void LogLineStepRequest(int direction);
    int LineStepOwnLines() const;
    int OwedLineStepAnswer(int direction, int ownLines);
    void RepairReadingRunWithoutLines();
    void MoveLineStepWithinPage(int offset, qreal absolute, qreal absoluteFrom);
    void MoveLineStepToNextPage(int page, int ownLines, qreal absolute, qreal absoluteFrom);
    void MoveLineStepBackPage(int page, int offset, qreal absoluteFrom);

    // ---- Clear all's undo (see clearAll/restoreCleared) -------------------
    struct ClearedSnapshot {
        bool valid = false;
        bool contentWasOnAir = false;   // restore re-sends it live vs re-stages it
        QString contentTitle;
        QVariantList contentSlidesRaw;  // whole visible set, raw (unstyled) maps
        QVariantMap contentStyled;      // the styled map the preview slot keeps
        QString inputLabel;
        QString inputKind;
        QString inputMode;
        QString mediaPath;
        QString mediaName;
        QVariantList overlays;          // [{id,name}], insertion order = z-order
    };
    ClearedSnapshot cleared_;
    bool canRestoreCleared() const { return cleared_.valid; }

    // ---- Feed health (see sampleFeedHealth) --------------------------------
    void sampleFeedHealth();
    void setFeedNotice(const QString &text, const QString &kind);
    struct FeedSample {
        qint64 ageMs = -1;
        qulonglong frames = 0;
    };
    void ClearFeedHealthWhenIdle();
    QString FeedBufferForRow(int row) const;
    void PublishOutputRuntime(const QHash<QString, FeedSample> &samples);
    QSet<QString> StalledOutputsFrom(const QHash<QString, FeedSample> &samples,
                                     QStringList &labels, std::vector<std::string> &toBlack);
    void AnnounceFeedStalled(const QString &labelLine, const QStringList &labels,
                             const std::vector<std::string> &toBlack);
    void AnnounceFeedRestored(const QString &labelLine);
    // Roster names (not labels) of the outputs whose feed is stalled, the "M1,
    // M2" line built from the SAME rows, and the pill's own text/kind.
    QSet<QString> stalledOutputs_;
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
    QString blackoutRestoreTitle_;
    QVariantList blackoutRestoreSlides_;
    bool blackoutActive_ = false;
    bool blackedOut() const;

    QTimer *poll_ = nullptr;   // while live: onAir/frames refresh at 10Hz
    QTimer *previewTimer_ = nullptr;
    std::unique_ptr<LivePreviewProvider> provider_;

    // ---- overlays-on-air state ----
    // {id,name} maps, insertion order = z-order (last taken = topmost).
    QVariantList activeOverlays_;

    void recheckNdiPlanOffThread();
    struct NdiSendRequest {
        QString name;
        QString nodeId;
        QString buffer;
        QString sender;
        float fps = 0.0f;
        bool audioWired = false;
        std::shared_ptr<bps::display::NdiDisplayProvider> provider;
    };
    std::vector<NdiSendRequest> CollectNdiSendRequests();
    void LogNdiRoster(const std::vector<NdiSendRequest> &requests);
    void WarnSilentNdiOutputs();
    void ForgetRetiredNdiProviders(const QSet<QString> &enabledNames);
    bool AnnounceNdiOutputStates(const std::vector<NdiSendRequest> &requests);
    void StopNdiWithoutPlan();
    void StartNdiPlanProbe(std::vector<NdiSendRequest> requests);
    void connectToNdiOutputRoster();
    bool ndiRosterConnected_ = false; // GUI thread, singleton may be created after this service
    class OutputListModel *ndiRosterModel_ = nullptr;
    QSet<QString> ndiSessionNames_;
    QSet<QString> ndiAnnouncedNames_;
    // ---- And the ones that never start -------------------------------------
    QHash<QString, qint64> ndiSilentSinceMs_;   // first seen not-sending
    QSet<QString> ndiSilentWarned_;             // one warning per output
    static constexpr qint64 kNdiSilentWarnMs = 6000;
    void warnSilentNewOutput(const QString &name, const QString &reason);
    QString ndiRosterLogSignature_; // GUI thread: suppress duplicate roster diagnostics
    QString ndiProbeLogSignature_;  // GUI thread: log probe transitions, not every poll
    struct NdiProbeResult {
        QString outputName;
        // The graph output this row's sender is the transport for ("out:<id>"),
        // resolved on the walk because the roster is only readable there.
        QString nodeId;
        // Whether an audio bus is wired into that node — decided on the walk from
        // the published graph, so an unwired output sends no silent audio track.
        bool audioWired = false;
        QString senderName;
        float fps = 30.0f;
        std::shared_ptr<bps::display::NdiDisplayProvider> provider;
        std::shared_ptr<bps::rendering::FrameBufferOutput> buffer;
        bool bufferOk = false;
        bool enabled = false;
        bool hasFrame = false;
    };
    void applyNdiPlanProbe(const std::vector<NdiProbeResult> &results);
    QSet<QString> ActiveNdiOutputNames() const;
    void AnnounceNdiSending(const NdiProbeResult &result, bool sending,
                            const QVariantMap &oldStatus);
    void LogNdiProbeResults(const std::vector<NdiProbeResult> &results);
    std::atomic<bool> ndiPlanProbeBusy_{false};   // one engine walk in flight at a time
    QVariantMap ndiOutputStates_;                 // GUI thread: name -> {sending, warming, frames}
    QHash<QString, std::shared_ptr<bps::display::NdiDisplayProvider>> ndiProviders_; // GUI thread owns each output sender
    void stopNdiFeed();

    // ---- NDI audio: an output's OWN mix, on the wire ----------------------
    struct NdiAudioBlock {
        std::vector<float> samples;   // interleaved, frames * channels
        uint32_t frames = 0;
        uint32_t channels = 0;
        uint32_t rateHz = 0;
        uint64_t startFrame = 0;      // its place on the mixer's sample clock
    };
    struct NdiAudio {
        std::string nodeId;
        uint64_t mixTap = 0;                  // 0 = not listening yet
        std::mutex mutex;                     // guards queue/queuedFrames
        std::deque<NdiAudioBlock> queue;
        uint64_t queuedFrames = 0;            // the backlog bound counts FRAMES, not blocks
        std::atomic<qulonglong> blocksSent{0};
        std::atomic<qulonglong> framesSent{0};
    };
    // A second of audio, then the OLDEST block goes: a stalled sender drops the
    // past instead of growing a latency the operator can only escape by restarting.
    static constexpr uint32_t kNdiAudioBacklogMs = 1000;
    // GUI thread: begin/end listening on one output's node.
    void listenOnNdiOutput(const QString &nodeId, const std::shared_ptr<NdiAudio> &audio);
    void silenceNdiOutput(const std::shared_ptr<NdiAudio> &audio);
    // Feed thread: hand this output's queued mix to its sender (planar float32).
    static void ndiWorkerSendAudio(void *providerVoid, const std::shared_ptr<NdiAudio> &audio);
    QHash<QString, std::shared_ptr<NdiAudio>> ndiAudio_;   // GUI thread only, keyed by node id
    static QString ndiSenderDisplayName(const QString &rowName = QString());
    void armNdiSenderForSession();
    QString ndiArmedName_;

    // ---- bus video windows (see clearBusFeeds above) ----
    struct BusFeed {
        QString label;      // the roster caption the tap is keyed by
        QString kind;       // "camera" | "screen" | "ndi"
        QString mode;       // a camera's capture-mode pick ("" for the others)
        QString deviceId;   // the resolved PAL id / NDI source name, for the log
        qint64 lastFrameKey = 0;
    };
    QHash<int, BusFeed> busFeeds_;
    QHash<QString, qint64> busTapRetry_;
    void refreshBusFeeds();
    void releaseBusFeed(int busId);
    bool FeedForSourceRow(int sourceRow, BusFeed &feed) const;
    bool FeedForBusRoute(int busId, BusFeed &feed) const;
    void AddWantedBusFeed(int busId, QList<int> &namedBuses, QHash<int, BusFeed> &wanted);
    void CollectBusFeedWants(QHash<int, BusFeed> &wanted, QList<int> &namedBuses);
    void AddTemplateWindowsFor(int busId, QHash<int, BusFeed> &wanted);
    void ReleaseStaleBusFeeds(const QHash<int, BusFeed> &wanted);
    bool ClaimWantedBusFeeds(const QHash<int, BusFeed> &wanted);
    void EnsureBusFeedPump(bool claimed);
    // Mirrors the held feeds' identity (device id + kind) into CompositorState —
    // the engine half of the "bus window is empty on air" diagnostic.
    void publishBusFeeds();

    // the env self-test's phases (runEnvSelfTest is static: these are called
    // through instance())
    QString PickSelfTestInputLabel() const;
    void RunInputTakeProbe(const QString &label);
    void RunInputReactivateProbe(const QString &label, bool held);
    void RunMeterProbe();
    void CheckMeterProbe(const QString &audioLabel);

    // ---- taken-input state ----
    QStringList monitoredAudio_;
    QString inputLabel_;
    QString inputKind_;
    QString inputMode_;
    qulonglong inputRev_ = 0;
    bool inputLive_ = false;
    bool ndiInputLive_ = false;
    QTimer *inputPump_ = nullptr;   // while taken: bus/input frames into the compositor
    // The taken input's last pushed frame (cacheKey), for the same reason
    // BusFeed::lastFrameKey exists — a stationary source costs nothing.
    qint64 inputFrameKey_ = 0;
    quint64 inputFrameSeq_ = 0;   // the taken camera/screen tap's frame counter at the last pump tick
    QHash<QString, qint64> cardFrameKeys_;
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
    bool mediaAlphaDecided_ = false;
    bool mediaIsVideo_ = false;
    std::mutex mediaMirrorMutex_;                    // the decoder thread forwards frames; the interface thread attaches
    std::vector<QPointer<QVideoSink>> mediaMirrorSinks_;

    // ---- A TAKEN CAMERA / SCREEN ON THE GPU (LiveInputSurface.qml) ------------------------------------
    // The output window and the monitor tiles drew a taken camera from `image://videopreview`: a JPEG the
    // tap re-encodes ~15 times a second, decoded again on the interface thread for every consumer. That
    // is what made HDMI lag. Now one worker thread hands the tap's own newest frame to every attached
    // VideoOutput as it arrives (a QVideoFrame, no JPEG anywhere), and the interface thread does nothing
    // per frame. The image path stays as the fallback for a QML scene with no GPU.
    std::mutex inputMirrorMutex_;
    std::vector<QPointer<QVideoSink>> inputMirrorSinks_;
    bps::QtThread inputFeedThread_;
    std::atomic<bool> inputFeedStop_{false};
    std::atomic<quint64> inputFramesFed_{0};
    void startInputFeed(const QString &resolvedDeviceId);
    void spawnInputFeedWorker(const std::string &deviceId);   // starts the worker thread (see the .cpp)
    void stopInputFeed();
    void forwardInputFrame(const QVideoFrame &frame);
    bool mediaIsAudio_ = false;
    QString mediaState_ = QStringLiteral("stopped");
    qlonglong mediaPosition_ = 0;
    qlonglong mediaDuration_ = 0;
    qulonglong mediaRev_ = 0;
    bool mediaMuted_ = false;
    // The file's sound obeys the Routing screen: it is heard on the PC's speakers only while the Speakers
    // output is on, at the source's level, and not muted (the transport's or the source's).
    void applyMediaAudio();
    void refreshSpeakersAudible();
    bool speakersAudible_ = true;
    qreal mediaLevel_ = 1.0;            // linear gain of the file's audio (the source's level / 100)
    qreal pendingMediaLevel_ = 1.0;      // a source's level, waiting for the (asynchronous) take to arrive
    bool pendingMediaMuted_ = false;
    bool pendingFromSource_ = false;
    QMediaPlayer *mediaPlayer_ = nullptr;
    QAudioOutput *mediaAudio_ = nullptr;
    QVideoSink *mediaSink_ = nullptr;
    QImage lastMediaFrame_;
    std::atomic<std::shared_ptr<const QImage>> mediaFrameShared_;
    void setLastMediaFrame(const QImage &frame)
    {
        lastMediaFrame_ = frame;
        mediaFrameShared_.store(std::make_shared<const QImage>(frame));
    }
    std::atomic<quint64> mediaTakeToken_{0};
    // A media frame's trip to the interface thread is ONE queued update at a time (the decoder thread
    // sets this, the interface thread clears it): the compositor gets every frame, the interface only
    // ever the newest.
    std::atomic<bool> mediaGuiUpdatePending_{false};
    std::atomic<quint64> mediaFramesDecoded_{0};

    // ---- NDI program sender (see the properties above) --------------------
    bool ndiSending_ = false;                    // GUI thread only
    QString ndiSendingOutput_;                   // GUI thread only (the pill's LIVE row)
    // True between "an NDI row wants feeding" and "first frame is on the wire".
    bool ndiWarming_ = false;                    // GUI thread only
    QString ndiWarmingOutput_;                   // GUI thread only
    // Update the warming state and notify ONLY on a real change (the send
    // tick would otherwise emit ndiChanged 30-60x/s for a static value).
    void setNdiWarming(bool on, const QString &name);
    std::atomic<qulonglong> ndiFramesSent_{0};
    // ---- The plan: WHAT to send, HANDED to the feed thread -----------------
    struct NdiSendPlan {
        QString outputName;
        std::shared_ptr<bps::display::NdiDisplayProvider> provider;
        std::shared_ptr<bps::rendering::FrameBufferOutput> buffer;
        QString senderName;
        float fps = 30.0f;
        QString nodeId;
        std::shared_ptr<NdiAudio> audio;
        bool operator==(const NdiSendPlan &o) const {
            return outputName == o.outputName && provider == o.provider && buffer == o.buffer
                   && senderName == o.senderName && fps == o.fps
                   && nodeId == o.nodeId && audio == o.audio;
        }
    };
    struct NdiPlanBuild {
        std::vector<NdiSendPlan> plans;
        QVariantMap states;
        QString firstSending;
        QString firstWarming;
        qulonglong totalFrames = 0;
    };
    NdiPlanBuild BuildNdiPlan(const std::vector<NdiProbeResult> &results,
                              const QSet<QString> &activeNames);
    std::vector<NdiSendPlan> ndiFeedPlan_;                // feed thread only
    // The last plan handed over, so a poll that changes nothing posts nothing
    // (the 10 Hz poll republishes the same plan otherwise).
    std::vector<NdiSendPlan> ndiSentPlan_;               // GUI thread only
    bool ndiPlanPublished_ = false;                      // GUI thread only
    void handOverNdiPlan(const std::vector<NdiSendPlan> &plans);
    // A probe result -> the plan handed to the feed thread (GUI thread).
    NdiSendPlan MakeNdiSendPlan(const NdiProbeResult &result);
    void AddNdiPlanFor(const NdiProbeResult &result, const QSet<QString> &activeNames,
                       NdiPlanBuild &build);
    bool ndiGateLogged_ = false;      // GUI thread only
    // Written by the feed thread AND the send pool, so they are guarded by ndiSendBookMutex_.
    QSet<QString> ndiSendFailedNames_;
    QSet<QString> ndiFirstFrameNames_;
    QSet<QString> ndiSlowNames_;
    QHash<QString, int> ndiFastStreaks_;
    bool ndiWaitLogged_ = false;      // GUI thread only; "waiting for buffer/frame" note
    QElapsedTimer ndiSendClock_;
    // Feed thread: each sender's NEXT DEADLINE (ms on ndiSendClock_), advanced by exactly one period per
    // frame so a late tick delays one frame instead of shifting every later one.
    QHash<QString, double> ndiSendDueMs_;
    // ---- The NDI feed: WHAT on the GUI thread, WHEN on the feed thread -----
    void startNdiSendClock();          // go-live: bring the feed thread up + publish
    void stopNdiSendClock();           // off air: clear the plan, stop its clock
    void pollTickRecheckNdi();
    // ---- Output frame-rate drift watch ---------------------------------------
    void checkNdiFeedRate();
    struct NdiRateWindow {
        QElapsedTimer clock;        // invalid = not sampling
        qulonglong frames = 0;      // last sample of THIS output's counter
        int slowWindows = 0;        // consecutive windows below its setting
        int noProgressWindows = 0;  // consecutive windows with zero successful sends
        bool warned = false;        // one drift warning per slow episode
        int tier = -1;              // -1 unknown, 0 at rate, 1 ~half, 2 lower
        float configured = 0.0f;    // its configured rate last window (re-baseline on change)
        double measured = 0.0;      // its ACTUAL rate last window (what it held, not what was asked)
    };
    QHash<QString, NdiRateWindow> ndiRateWindows_;   // GUI thread only
    static constexpr int kDriftWindowMs = 4000;
    static constexpr int kDriftWindowsToWarn = 2;
    void PruneNdiRateWindows();
    bool MeasureNdiRateWindow(const QString &name, const QVariantMap &status, NdiRateWindow &w,
                              double *measured, float *configured);
    void ResetNdiSenderForRecovery(const QString &name);
    void ReportNdiRateStep(const QString &name, double measured, float configured,
                           NdiRateWindow &w);
    QString autoAdvanceShow_;            // the show this timer was recorded for
    bool autoAdvanceLoaded_ = false;     // shows.timedShow read once
    bool timedAdvanceApplied_ = false;   // the last value pushed at the engine
    QTimer *autoAdvanceTimer_ = nullptr;   // GUI thread: single shot per slide, re-armed on every change
    double onAirDurationMs_ = 0.0;         // the on-air slide's own NEXT TIMER (0 = it waits for a hand)
    qreal lastAutoLine_ = -1.0;            // settled absolute line at the previous auto step (-1 = none)
    QTimer *ndiSendTimer_ = nullptr;   // feed thread only

    // ---- The send pool: a few threads, each sender's cadence still its own ---------------------
    // One sender's SDK handoff used to hold every sender behind it on the single feed thread, so a slow
    // route delayed the whole board. The tick still decides WHO is due and WHEN (one clock per sender);
    // the pool only runs the work. Small on purpose: the runtime compresses on its own threads, so this
    // side's share is the conversion plus a non-blocking handoff.
    QThreadPool *ndiPool_ = nullptr;      // created with the feed thread (GUI side)
    std::mutex ndiSendBookMutex_;         // the per-sender notes the pool writes
    std::mutex ndiInFlightMutex_;         // guards ndiInFlight_
    QSet<QString> ndiInFlight_;           // one send per sender at a time (feed thread + pool)
    // Converted frames kept for REUSE, keyed by the frame buffer: a fresh 4 MB buffer every tick is malloc
    // churn on top of the conversion. A slot a sender still reads has use_count() > 1 and is never refilled.
    std::mutex ndiRgbaMutex_;
    QHash<const void *, std::vector<std::shared_ptr<bps::display::NdiRgbaFrame>>> ndiRgbaSlots_;
    // The last conversion per frame buffer: while the buffer still holds the SAME snapshot (a still slide,
    // a paused picture) the next tick re-sends the converted frame instead of converting 8 MB again.
    struct NdiConvertedFrame {
        std::shared_ptr<const void> source;   // the snapshot that was converted (held, so its address stays unique)
        std::shared_ptr<bps::display::NdiRgbaFrame> converted;
    };
    QHash<const void *, NdiConvertedFrame> ndiConverted_;   // guarded by ndiRgbaMutex_

    // ---- The NDI feed thread --------------------------------------------------
    QThread *ndiThread_ = nullptr;
    QObject *ndiWorker_ = nullptr;     // lives on ndiThread_
    bool ndiClockArmed_ = false;
    // Create the feed thread once (GUI side); the thread's own clock is
    // created on it by setNdiPlan's first published frame.
    void ensureNdiWorker();
    void setNdiPlan(const std::vector<NdiSendPlan> &plans);
    void clearNdiPlan();
    // One beat of the feed (FEED THREAD): read the plan, keep the clock's
    // interval at the published rate, and send at most one frame per period.
    void ndiWorkerTick();
    // Convert ONCE for this buffer and send it to every plan in `sends` on the pool (posted by the tick).
    void postNdiSendGroup(std::shared_ptr<bps::rendering::FrameBufferOutput> buffer,
                          std::vector<NdiSendPlan> sends);
    // The pool measurement's second NDI output: added for VGR_NDI_POOL_TEST runs, removed otherwise.
    void ensureNdiPoolTestRoster();
    // GO LIVE with nothing picked: put the wall's LOADED show on air (true = it went live with it).
    bool goLiveLoadedShow();
    // ---- GO LIVE load check: what this machine carried last session vs what is configured now ----
    // A warning, never a block: the numbers are the machine's own, and the user may know better.
    void warnIfNdiLoadExceedsSustained();
    void RecordNdiSustainedLoad(qint64 pixelsPerSecond, const QString &detail);
};

// A picture shrunk for a small preview tile, quickly: each destination pixel averages a small grid of
// source pixels (alpha-weighted, so a transparent area does not darken its edges). QImage::scaled with
// SmoothTransformation took ~10 ms for a 1080p frame, per tile, per frame, on the one image-loader
// thread every tile shares — the queue the interface thread then waited on. Enlarging or a mild
// shrink (under 2x) falls back to Qt's own scaler.
QImage shrinkForPreview(const QImage &src, const QSize &target, Qt::AspectRatioMode mode);

// QQuickImageProvider over the engine preview output's last frame:
//   image://livepreview?v=<frameRev>
class LivePreviewProvider final : public QQuickImageProvider {
public:
    LivePreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};

class LiveMediaFrameProvider final : public QQuickImageProvider {
public:
    LiveMediaFrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;
};
