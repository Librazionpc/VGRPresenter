#pragma once

#include <QQuickImageProvider>
#include <QObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QJSValue>
#include <QVariantList>
#include <qqml.h>

#include "core/events/EventBus.hpp"
#include "platform/IAudio.hpp"
#include "platform/IMonitor.hpp"
#include "platform/IVideo.hpp"

class QJSEngine;
class QQmlEngine;

// The bridge between the Qt UI process and the real PresentationEngine
// (src/PresentationEngine). Two independent things live here:
//
//  1. Kernel boot — boot()/shutdown() drive bps::Kernel::Instance(), the
//     29-system engine (see Kernel.hpp/.cpp). This is the heavy, side-
//     effectful step: it opens a real database, mounts content directories,
//     starts a thread pool + scheduler, and initializes Render/Display/
//     Media/Broadcast (all lazy/software-backed by default — verified by
//     reading their Initialize() before wiring this — no GPU context, no
//     hardware opened up front). Called once from main.cpp, after
//     QGuiApplication exists (needed for QStandardPaths) and before the
//     QML engine loads. NOT implicitly triggered by using the pieces below.
//
//  2. UndoRedoManager — wraps bps::project::UndoRedoManager
//     (modules/include/modules/project/UndoRedoManager.hpp), the engine's
//     command-based undo/redo stack. This one is a plain, self-contained
//     Meyers singleton with NO dependency on Kernel::Boot() (no disk I/O,
//     no threads) — usable whether or not boot() has run. See
//     docs/architecture/ProjectSystem.md's "Undo/Redo".
//
// pushCommand's doFn/undoFn are plain QML functions — the ENGINE owns
// ordering/grouping/depth-capping, QML owns what each step actually DOES.
// Both run synchronously on the calling thread; never call pushCommand/
// undo/redo/boot/shutdown from a worker thread.
class EngineBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY stackChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY stackChanged)
    Q_PROPERTY(bool booted READ booted NOTIFY bootedChanged)
    // Real hardware, enumerated by the engine's own platform layer (audio
    // via the PAL's IAudio — WinMM waveIn/waveOut on Windows — displays via
    // IMonitor) — the AV settings dialogs' device selects feed from these
    // instead of mock rosters.
    // Enumerated at boot and re-enumerated automatically on the kernel's
    // hot-plug events (platform.device_connected / device_removed /
    // monitor_connected / monitor_disconnected).
    Q_PROPERTY(QVariantList audioDevices READ audioDevices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList screenDevices READ screenDevices NOTIFY devicesChanged)
    // Real video-capture devices (Media Foundation on Windows): each entry
    // { id, label, value, modes: ["1080p60", …], maxFps } — modes are the
    // device's actual capability list and maxFps the device-wide ceiling
    // the UI greys anything faster against (0 = unknown → no gate).
    Q_PROPERTY(QVariantList videoDevices READ videoDevices NOTIFY devicesChanged)
    // NDI network sources, discovered by the ENGINE's own broadcast stack
    // (BroadcastEngine's NDI provider, which runtime-loads the NDI SDK).
    // Entries: { id, label, value, url }. ndiAvailable is false when the SDK
    // is absent (ndiStatus then carries the reason); ndiSources is empty
    // until the discovery cache warms up (~1s) — the UI re-queries.
    Q_PROPERTY(QVariantList ndiSources READ ndiSources NOTIFY devicesChanged)
    Q_PROPERTY(bool ndiAvailable READ ndiAvailable NOTIFY devicesChanged)
    Q_PROPERTY(QString ndiStatus READ ndiStatus NOTIFY devicesChanged)
    // "ready" | "notInstalled" | "error" | "unknown" (engine not booted). Only
    // "notInstalled" is something the user can fix — the UI then offers
    // openNdiDownloadPage(). ndiVersion is the runtime's own version string.
    Q_PROPERTY(QString ndiState READ ndiState NOTIFY devicesChanged)
    Q_PROPERTY(QString ndiVersion READ ndiVersion NOTIFY devicesChanged)
    Q_PROPERTY(QString ndiDownloadUrl READ ndiDownloadUrl CONSTANT)
    // LIVE audio input metering — real per-channel peak/RMS from the PAL's
    // WASAPI capture tap (a roster card's mic/line-in metered end-to-end).
    // Entries: { deviceId, label, channelCount, layout, sampleRateHz,
    // framesCaptured, peaks: [..], rms: [..] } — layout is "mono" | "stereo"
    // | "multi" (1/2/N channels of the endpoint's live mix format, engine
    // truth, not the roster's stored count). One snapshot per metered device,
    // refreshed ~20×/s by an internal GUI-side pump that only runs while at
    // least one tap is active; peaks/rms are 0..1 fractions of full scale and
    // the list is EMPTY when nothing is metered ("no signal", never a
    // synthetic waveform). QML calls startInputMeter(label) when an audio
    // dialog opens (empty label meters the default input) and
    // stopInputMeter() when it closes.
    Q_PROPERTY(QVariantList inputLevels READ inputLevels NOTIFY inputLevelsChanged)

public:
    // The QML image source is "image://videopreview/<deviceId>?<nonce>" —
    // the provider (main.cpp registers it up-front, before QML loads) pulls
    // the PAL's newest JPEG for that device on every request; the nonce is
    // what forces QML's image cache to re-fetch. Public: main.cpp constructs
    // it directly.
    class VideoPreviewProvider : public QQuickImageProvider {
    public:
        explicit VideoPreviewProvider(EngineBridge *owner) : QQuickImageProvider(QQuickImageProvider::Image), owner_(owner) {}
        QImage requestImage(const QString &id, QSize *size, const QSize &requested) override;
    private:
        EngineBridge *owner_;   // not owned
    };
    friend class VideoPreviewProvider;
    QImage latestPreviewFrame(const QString &deviceId);

    static EngineBridge &instance();
    static EngineBridge *create(QQmlEngine *engine, QJSEngine *jsEngine);

    bool canUndo() const;
    bool canRedo() const;
    bool booted() const;

    // Boots the real Kernel with a sandboxed, real-world-safe configuration:
    // an isolated per-app data/log directory under the OS's standard app
    // data location, no plugin directories scanned, no IPC port opened.
    // Idempotent — a second call is a harmless no-op (Kernel::Boot() itself
    // refuses a re-boot from Running state). Returns false and leaves a
    // human-readable reason in bootError() on failure.
    bool boot();
    Q_INVOKABLE bool isBooted() const { return booted(); }

    // The engine log file (<app folder>/logs/engine.log, or the user's app-data folder when the app folder is read-only).
    // Empty until boot().
    Q_INVOKABLE QString logPath() const { return logPath_; }
    // One line in the engine log, stamped with who wrote it: level is "trace"|"debug"|"info"|"warning"|"error", source is the
    // subsystem ("Projects", "Import", "Show"...). QML calls this for what the user did; the C++ services use the static form.
    Q_INVOKABLE void log(const QString &level, const QString &source, const QString &message);
    static void write(const QString &level, const QString &source, const QString &message);
    Q_INVOKABLE QString bootError() const;
    Q_INVOKABLE QStringList bootLog() const;
    Q_INVOKABLE QString health() const;
    // One-line boot report for the startup toast: systems count + how many
    // real audio devices / displays the engine's platform layer enumerated.
    // Empty until boot() runs. QML reads it in Component.onCompleted —
    // events published before QML loads have no subscribers, so the summary
    // is pulled, not pushed.
    Q_INVOKABLE QString bootSummary() const { return bootSummary_; }
    // Re-enumerate the platform's audio + display devices (QML-invokable so
    // a screen can force a refresh when it opens). Safe before boot — yields
    // empty lists when the PAL has no backend installed.
    Q_INVOKABLE void refreshDevices();

    // ---- Live input metering (see the inputLevels property) -------------
    // Taps are a SET keyed by device — the AV board keeps every device row
    // metered while it is open, and a dialog reading a device's snapshot is
    // just another consumer. startInputMeter ENSURES a tap for the capture
    // device with this roster label (the AV board stores labels as its
    // sublabels; an empty label meters the platform's DEFAULT input) and is
    // idempotent per device. Safe to call when a device vanishes: unknown
    // labels warn and no-op, failures never throw.
    Q_INVOKABLE void startInputMeter(const QString &deviceLabel);
    // Stop one device's tap (identified by the label it was started with).
    Q_INVOKABLE void stopInputMeter(const QString &deviceLabel);
    // Stop every meter tap and the refresh pump — the AV screen calls this
    // when it is destroyed (nothing meters while settings are closed).
    Q_INVOKABLE void stopAllInputMeters();

    // ---- Live video preview ---------------------------------------------
    // Start (or restart) a REAL Media Foundation Source Reader tap for the
    // capture device with this roster label, locking to the given capture
    // mode pick when non-empty ("1920x1080p60" — size honored, fps breaks
    // ties). QML then points an Image at
    //   image://videopreview/<label>?<nonce>
    // and bumps the nonce ~15×/s — each request returns the newest JPEG
    // (the provider drains straight from the PAL's tap). Idempotent per
    // device; a mode re-pick restarts the tap at the new size.
    // owner: who holds this tap ("board" row vs "dialog" pane) — the board
    // row AND a dialog pane can preview the same camera at once, and each
    // reconciles its own hold independently (mode re-picks, camera switches).
    // A start is idempotent per owner; the tap dies only when its LAST owner
    // releases. Defaults keep plain QML calls working.
    Q_INVOKABLE void startVideoPreview(const QString &deviceLabel, const QString &mode,
                                       const QString &owner = QStringLiteral("dialog"));
    // Release one device's preview tap (label-keyed, like the meter taps).
    Q_INVOKABLE void stopVideoPreview(const QString &deviceLabel,
                                      const QString &owner = QStringLiteral("dialog"));
    // Release every preview tap (the AV screen's teardown).
    Q_INVOKABLE void stopAllVideoPreviews();

    // ---- Live SCREEN preview --------------------------------------------
    // Same owner-counted discipline as the camera taps, keyed by the
    // monitor's roster label; frames flow through the same
    // image://videopreview/<label>?<nonce> provider path. The pane pumps
    // the nonce; the PAL BitBlts the monitor named by the resolved id.
    // Returns whether the tap is (now) running — LiveOutputService's take
    // surfaces a refused label honestly instead of showing a warm-up
    // placeholder that can never fill.
    Q_INVOKABLE bool startScreenPreview(const QString &monitorLabel,
                                        const QString &owner = QStringLiteral("dialog"));
    Q_INVOKABLE void stopScreenPreview(const QString &monitorLabel,
                                       const QString &owner = QStringLiteral("dialog"));
    // Each entry: { id, label, value } — value mirrors label because the AV
    // board stores the human-readable sublabel on its rows; ids ride along
    // for the future capture-graph plumbing.
    QVariantList audioDevices() const { return audioDevices_; }
    QVariantList inputLevels() const { return inputLevels_; }
    QVariantList screenDevices() const { return screenDevices_; }
    QVariantList videoDevices() const { return videoDevices_; }
    QVariantList ndiSources() const { return ndiSources_; }
    // The tap's latest decoded frame for a roster label (the SAME decode-once
    // cache requestImage drains) — LiveOutputService's pump reads this to
    // detect first frames itself, instead of QML confirming back (a write
    // into the tile's own URL binding = binding loop).
    QImage previewFrameFor(const QString &label);
    bool ndiAvailable() const { return ndiAvailable_; }
    QString ndiStatus() const { return ndiStatus_; }
    QString ndiState() const { return ndiState_; }
    QString ndiVersion() const { return ndiVersion_; }
    QString ndiDownloadUrl() const;
    // Opens the NDI runtime download page in the user's browser.
    Q_INVOKABLE void openNdiDownloadPage();
    // Re-checks for the runtime right now (after the user installed it).
    Q_INVOKABLE void recheckNdi();
    // Called from main.cpp on QGuiApplication::aboutToQuit — orderly
    // teardown of the 29 systems (threads joined, database flushed) instead
    // of letting process exit cut them off mid-flight.
    void shutdown();

    // GUI-thread-only entry point the relay's marshalled lambdas call (see
    // startRelay in EngineBridge.cpp): appends to the recent-events ring,
    // emits engineEvent, publishes to the UI EventBus. Public because the
    // EngineLogSink (a free class, not a member) invokes it via
    // QMetaObject::invokeMethod after hopping off the Logger's engine thread.
    void ingestEngineEvent(const QString &topic, const QVariantMap &payload);

    // Registers one reversible step — doFn runs now (ExecuteCommand's own
    // contract), undoFn runs on undo(), doFn runs again on redo() (ICommand's
    // default Redo() == Execute()). Clears the redo branch, same as any
    // fresh edit after an undo.
    Q_INVOKABLE void pushCommand(const QString &label, const QJSValue &doFn, const QJSValue &undoFn);
    Q_INVOKABLE bool undo();
    Q_INVOKABLE bool redo();
    Q_INVOKABLE QString undoLabel() const;
    Q_INVOKABLE QString redoLabel() const;
    Q_INVOKABLE void clearHistory();

signals:
    void stackChanged();
    void bootedChanged();
    // Fired for every ENGINE-side event relayed into the UI process — kernel
    // state changes, display/recording/render/media/broadcast failures and
    // recoveries, undo/redo performed, ... (the curated set in
    // EngineBridge.cpp's startRelay). topic is the engine's own kTopic string
    // verbatim ("recording.failed", "presentation.slide_changed"), so QML can
    // filter with `if (topic === "...")`. Emitted on the GUI thread no matter
    // which engine thread produced the event. EventBus.eventPosted carries the
    // same payload for the toast pipeline; this signal is the typed,
    // engine-specific feed for screens that want to react without seeing UI-
    // internal traffic.
    void engineEvent(const QString &topic, const QVariantMap &payload);

    // Emitted whenever the device lists are (re)enumerated — boot and every
    // platform hot-plug event.
    void devicesChanged();

    // ~20×/s while a meter tap is live: inputLevels_ was refreshed from the
    // PAL's snapshots. QML reads the property fresh on each emission — a
    // deliberately modest rate (not the ~480Hz the captures run at) so the
    // meter dots animate like a real VU without flooding the GUI thread.
    void inputLevelsChanged();

private:
    explicit EngineBridge(QObject *parent = nullptr);
    Q_DISABLE_COPY(EngineBridge)

    QString bootError_;
    QString bootSummary_;
    QString logPath_;

    // ---- Engine → UI relay (see startRelay in EngineBridge.cpp) ----------
    // The kernel drives everything; the UI only relays. Attaches a Logger
    // sink (engine warnings/errors/fatals) and engine-EventBus subscriptions
    // (the curated user-facing event set); each hops to the GUI thread via a
    // queued invoke before touching the UI-side EventBus.
    void startRelay();
    void stopRelay();

    // GUI-thread-only device enumeration into audioDevices_/screenDevices_.
    void enumerateDevices();
    QVariantList audioDevices_;
    QVariantList screenDevices_;
    QVariantList videoDevices_;
    QVariantList ndiSources_;

    // ---- Live input metering state --------------------------------------
    QVariantList inputLevels_;          // the published snapshot set
    class QTimer *inputMeterPump_ = nullptr;   // 50 ms poll while taps are live
    QMap<uint32_t, QString> inputLabels_;   // waveIn device id → roster label
    // Every label with a requested tap (idempotent startInputMeter calls);
    // labels resolve again on re-enumeration so hot-plug re-binds.
    QSet<QString> requestedMeters_;
    void resolveAndStartMeter(const QString &deviceLabel);

    // ---- Live video preview state ---------------------------------------
    QMap<QString, QString> previewModes_;   // roster label → requested mode pick
    QMap<QString, QString> previewIds_;     // roster label → engine device id
    // Owner set per label — who currently holds a tap on this camera. The
    // tap dies only when its last owner releases; start/stop are idempotent
    // per owner so repeated reconcile calls can't leak or over-release.
    QHash<QString, QSet<QString>> previewOwners_;
    // Decode-once cache (see latestPreviewFrame in the .cpp): per device id,
    // the last decoded frame + the JPEG size + FNV-1a hash that produced it.
    // requestImage runs on the GUI thread and several consumers poll the
    // same tap — decoding per request starved the render loop.
    QMap<QString, QImage> previewDecoded_;
    QMap<QString, int> previewRawLen_;
    QMap<QString, quint32> previewFrameHash_;   // FNV-1a of the JPEG bytes
    // One-shot diagnostics: the first-frame log line (tap→publish→fetch→
    // decode works end to end) and the per-label no-tap warning.
    QSet<QString> previewFirstFrameLogged_;
    QSet<QString> previewNoTapWarned_;
    QSet<QString> previewServedLogged_;   // per-label frame-served confirmation

    bool ndiAvailable_ = false;
    bool ndiRetried_ = false;   // one deferred NDI re-query per boot/refreshDevices()
    qint64 lastEnumerationMs_ = 0;   // throttles refreshDevices()
    QString ndiStatus_;
    QString ndiState_ = QStringLiteral("unknown");
    QString ndiVersion_;

    std::vector<bps::Subscription> relaySubs_;
    QVariantList recentEngineEvents_;   // ring capped at kMaxRecentEvents
    static constexpr int kMaxRecentEvents = 200;

public:
    // The most recent relayed engine events (newest LAST), for diagnostics
    // surfaces (a future debug console / activity log). level/title/message
    // keys match the EventBus toast convention; "topic" is the engine topic.
    Q_INVOKABLE QVariantList recentEngineEvents(int max = 50) const;
};
