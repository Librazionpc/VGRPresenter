#pragma once

#include <QQuickImageProvider>
#include <QObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QJSValue>
#include <QVariantList>
#include <QThreadPool>
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
    // The boot failure reason ("" = no failure). The boot splash gates its
    // reveal on `booted || bootError` — a FAILED boot must still bring the
    // splash down (the UI keeps working against mock data and the error
    // also surfaces as a toast), or a failure would leave the splash up
    // forever.
    Q_PROPERTY(QString bootError READ bootError NOTIFY bootErrorChanged)
    // Real hardware, enumerated by the engine's own platform layer (audio
    // via the PAL's IAudio — WinMM waveIn/waveOut on Windows — displays via
    // IMonitor) — the AV settings dialogs' device selects feed from these
    // instead of mock rosters.
    // Enumerated at boot and re-enumerated automatically on the kernel's
    // hot-plug events (platform.device_connected / device_removed /
    // monitor_connected / monitor_disconnected).
    Q_PROPERTY(QVariantList audioDevices READ audioDevices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList screenDevices READ screenDevices NOTIFY devicesChanged)
    // True once the sweep that carried the app's OWN device set has landed.
    // boot() opens with one enumeration before QML has handed that set over
    // (nothing configured yet, so nothing demanded) — correct for "what is
    // attached", useless as a device report: a boot summary taken off it reads
    // "0 audio device(s)". Anything that reports the ROSTERS as the app's
    // hardware (the startup toast) waits for this instead of reading a list
    // that is still the placeholder for its own truth.
    Q_PROPERTY(bool bootSweepDone READ bootSweepDone NOTIFY bootSweepDoneChanged)
    // The PRIMARY display's REAL size, straight from the engine's platform layer
    // (IMonitor: IDXGI/EnumDisplayMonitors truth), as the map { id, label,
    // widthPx, heightPx, primary }. This is what Main.qml sizes and scales the
    // app against: Qt's own Screen geometry is the DPI-SCALED logical size, so on
    // a scaled display it does not say what panel the user actually has. Empty
    // before boot, or when the PAL has no monitor backend — callers fall back to
    // Qt's Screen then.
    Q_PROPERTY(QVariantMap primaryDisplay READ primaryDisplay NOTIFY devicesChanged)
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
    // The ENGINE's feature registry (AdaptiveRuntime's FeatureManager) as the
    // Settings · Plugins screen's data source: entries { id, name, enabled,
    // defaultEnabled }. `enabled` mirrors the registry's real state — the
    // switch flips the ENGINE, not a local list. setPluginFeatureEnabled is
    // the setter; pluginFeaturesChanged fires on any change (including boot,
    // when the runtime auto-disables features the host cannot host).
    Q_PROPERTY(QVariantList pluginFeatures READ pluginFeatures NOTIFY pluginFeaturesChanged)
    // "ready" | "notInstalled" | "error" | "unknown" (engine not booted). Only
    // "notInstalled" is something the user can fix — the UI then offers
    // openNdiDownloadPage(). ndiVersion is the runtime's own version string.
    Q_PROPERTY(QString ndiState READ ndiState NOTIFY devicesChanged)
    Q_PROPERTY(QString ndiVersion READ ndiVersion NOTIFY devicesChanged)
    Q_PROPERTY(QString ndiDownloadUrl READ ndiDownloadUrl CONSTANT)
    // SDI (DeckLink): honest runtime status for the Outputs screen. An absent
    // SDK/hardware reads unavailable with the engine's own reason — never a
    // mock roster. Entries: { name, model, index, supportsCapture,
    // supportsOutput }.
    Q_PROPERTY(bool sdiAvailable READ sdiAvailable NOTIFY devicesChanged)
    Q_PROPERTY(QString sdiStatus READ sdiStatus NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList sdiDevices READ sdiDevices NOTIFY devicesChanged)
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
    // dialog opens and stopInputMeter() when it closes. An EMPTY label is
    // REFUSED (see startInputMeter below): the list stays empty until the user
    // has actually chosen a device.
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
    // NOTE: there is no QML-side "which subsystems are degraded" list any more.
    // boot() replays one warning toast per degraded subsystem (see the
    // SubsystemDegraded replay below) and folds the count into bootSummary(),
    // which Main.qml shows — so a second accessor for the same list was API
    // with no consumer.
    // One-line boot report for the startup toast: systems count + how many
    // real audio devices / displays the engine's platform layer enumerated.
    // Empty until boot() runs. QML reads it in Component.onCompleted —
    // events published before QML loads have no subscribers, so the summary
    // is pulled, not pushed.
    Q_INVOKABLE QString bootSummary() const { return bootSummary_; }
    // The device names that summary names as "in use" (audio, display, camera),
    // in the order they read there. QML hands them to the boot toast as its
    // crux, so the toast can bold WHICH hardware this session is using instead
    // of phrasing the whole sentence (see NotificationToast.crux).
    Q_INVOKABLE QStringList bootInUseDevices() const { return bootInUse_; }
    // The primary display's real pixel size (see the primaryDisplay property).
    // Never empty once the platform layer enumerated a display.
    Q_INVOKABLE QVariantMap primaryDisplay() const;
    // Re-enumerate the platform's audio + display devices (QML-invokable so
    // a screen can force a refresh when it opens). Safe before boot — yields
    // empty lists when the PAL has no backend installed.
    Q_INVOKABLE void refreshDevices();
    // Refresh only capture/render devices; does not enumerate cameras,
    // displays, open windows, or network sources on the GUI thread.
    Q_INVOKABLE void refreshAudioDevices();

    // ---- The USED-device set (boot sweep filter) -------------------------
    // Headless device probing is not free: every audio endpoint is a WASAPI
    // COM activation, every camera a Media Foundation capability walk, every
    // monitor/window an enumeration — and a session touches a handful of them.
    // QML hands over what the app actually SET UP (the audio endpoints its
    // input roster names, the cameras/NDI sources its video roster names, the
    // display or window each output is bound to — all restored from the store
    // before this call), and the FIRST sweep ASKS FOR those and nothing else:
    // the PAL enumerates them targeted (IAudio/IVideo::EnumerateFor), so an
    // endpoint or camera this setup does not use is never activated at all —
    // the boot sweep does not enumerate the machine and then throw the answer
    // away. Any device picker opening calls refreshDevices()/
    // refreshAudioDevices() — Settings · Audio & Video's Add/Edit does, which
    // is where the user is genuinely choosing a NEW device — and those do the
    // full sweep from then on: a new pick has to come from somewhere, it just
    // should not be paid for at boot. A used device that is gone this time is
    // announced once (the missing-device notice), which is the other half of
    // remembering it.
    Q_INVOKABLE void requestUsedDevices(const QStringList &audioLabels,
                                        const QStringList &videoLabels,
                                        const QStringList &displayNames);
    // The used devices that were NOT there when the boot check ran (friendly
    // names, in the order they were handed over; empty until then and after a
    // clean boot). The UI — not the bridge — publishes the notice: it has to
    // land AFTER the engine boot toast, and that toast goes out from the UI's
    // own devicesChanged handler, so the UI is the only place that can order
    // the two (see Main.qml's reportMissingDevices).
    Q_INVOKABLE QStringList missingUsedDevices() const { return missingUsedDevices_; }
    bool bootSweepDone() const { return bootSweepDone_; }

    // ---- Live input metering (see the inputLevels property) -------------
    // Taps are a SET keyed by device — the AV board keeps every device row
    // metered while it is open, and a dialog reading a device's snapshot is
    // just another consumer. startInputMeter ENSURES a tap for the capture
    // device with this roster label (the AV board stores labels as its
    // sublabels) and is idempotent per device. Safe to call when a device
    // vanishes: unknown labels warn and no-op, failures never throw.
    //
    // An EMPTY label is REFUSED — there is no "meter whatever the machine's
    // default input happens to be" mode. It used to be implicit, and that is
    // what the live report "it is animating without any device selected … so
    // the WASAPI is being opened" was: any dialog or card that reached this
    // with no pick opened a real capture session on the default microphone,
    // lit the MonitorWall's audio button (anyInputMetering) and animated every
    // strip that reads a snapshot. A tap belongs to a device the user chose.
    Q_INVOKABLE void startInputMeter(const QString &deviceLabel);
    // Stop one device's tap (identified by the label it was started with).
    Q_INVOKABLE void stopInputMeter(const QString &deviceLabel);
    // Stop every meter tap and the refresh pump — the AV screen calls this
    // when it is destroyed (nothing meters while settings are closed).
    Q_INVOKABLE void stopAllInputMeters();

    // ---- Live STREAM metering (a video/NDI row's OWN audio) ----------------
    // A media file and an NDI stream carry audio no capture device can reach,
    // so the PAL's stream tap (IAudio::StartStreamTap) is the one carrier for
    // it. startStreamMeter ENSURES a tap for this row and publishes its
    // snapshot into the SAME inputLevels list every device meter already lands
    // in (matched by label, exactly as a microphone's is), which is what lets
    // a video card's L/R pills, its Edit dialog's channel strips and the
    // mixer's own source binding read a video row's audio through the one code
    // path a device uses. Refcounted per label: a Media-pane card and an open
    // Edit dialog can hold the same row independently.
    //
    // kind is the PAL's producer kind: "media" (the backend decodes the file's
    // audio track itself) or "ndi" (a receiver pushes the samples as they
    // arrive). `source` is what the producer opens: for "media" a real FILE
    // PATH (the row's caption may be just a filename, so pass the path when the
    // row has one). A refused source (no such file, no NDI runtime) leaves the
    // row with NO tap — it meters silence, never a fabricated level.
    Q_INVOKABLE void startStreamMeter(const QString &label, const QString &kind,
                                      const QString &source = QString());
    // Release one row's stream tap (label-keyed, like the device meters).
    Q_INVOKABLE void stopStreamMeter(const QString &label);

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
    // Returns whether the tap is (now) running — the internal card previews
    // and LiveOutputService's take both surface a refused label honestly
    // (the slashed "unreachable" state) instead of a placeholder that can
    // never fill.
    Q_INVOKABLE bool startVideoPreview(const QString &deviceLabel, const QString &mode,
                                       const QString &owner = QStringLiteral("dialog"));
    // Release one device's preview tap (label-keyed, like the meter taps).
    Q_INVOKABLE void stopVideoPreview(const QString &deviceLabel,
                                      const QString &owner = QStringLiteral("dialog"));
    // Release every preview tap (the AV screen's teardown).
    Q_INVOKABLE void stopAllVideoPreviews();

    // ---- Live NDI receiver tap -------------------------------------------
    // Same owner-counted discipline as the camera/screen taps, keyed by the
    // NDI source's FULL name ("<HOST> (<sender>)" — exactly what discovery
    // reports and the roster stores). The tap is an ENGINE receiver
    // (BroadcastEngine::CreateNdiReceiver — the NDI provider runtime-loads
    // the vendor SDK); frames are pulled non-blocking on demand by
    // previewNdiFrameFor()/the videopreview provider and converted
    // UYVY/BGRA → RGBA, so QML's existing image pipeline needs no changes.
    // Returns whether a receiver is (now) attached; a tap held while the
    // NDI runtime is unavailable reports the refusal through its empty
    // frames (the warm-up placeholder stays honest).
    Q_INVOKABLE bool startNdiPreview(const QString &sourceName,
                                     const QString &owner = QStringLiteral("dialog"));
    // Release one NDI receiver (label-keyed, like the camera taps). The
    // engine receiver dies with its last owner.
    Q_INVOKABLE void stopNdiPreview(const QString &sourceName,
                                    const QString &owner = QStringLiteral("dialog"));

    // ---- Composition: which blocks paint UNDER a live feed --------------
    // The INDICES of `blocks` that the engine will paint on the scene's "bg"
    // layer, i.e. under the compositor's video layer (a live taken camera/screen
    // input, media on air). The rule itself is the engine's and lives in exactly
    // one place (SceneBuilder::PlanBlockLayers) — a QML surface asks for the
    // answer instead of re-deriving it, because a preview with its own copy of a
    // rule this geometric drifts from the outputs the moment either one changes,
    // and the operator then sees one picture on the tile and another on air.
    // The monitor tile draws these blocks' OWN PAINT below the feed and the rest
    // of them above it, which is what every real output does.
    Q_INVOKABLE QVariantList backdropBlockIndexes(const QVariantList &blocks) const;

    // ---- Live SCREEN preview --------------------------------------------
    // Same owner-counted discipline as the camera taps, keyed by the
    // monitor's roster label; frames flow through the same
    // image://videopreview/<label>?<nonce> provider path. The pane pumps
    // the nonce; the PAL BitBlts the monitor named by the resolved id.
    // Returns whether the tap is (now) running — LiveOutputService's take
    // surfaces a refused label honestly instead of showing a warm-up
    // placeholder that can never fill.
    Q_INVOKABLE bool startScreenPreview(const QString &monitorLabel,
                                        const QString &owner = QStringLiteral("dialog"));    Q_INVOKABLE void stopScreenPreview(const QString &monitorLabel,
                                        const QString &owner = QStringLiteral("dialog"));
    // QUIET reachability probe — the SAME resolution chain startScreenPreview
    // uses (cached roster → live windows → monitors; camera: cached roster →
    // live enumerate) but SILENT: the Media card previews gate their tap
    // start on this, so a CLOSED WINDOW shows the card's slashed "No signal"
    // state instead of toasting. GUI-thread only (like enumerateDevices).
    Q_INVOKABLE bool inputSourceReachable(const QString &label, const QString &kind);
    // The current meter snapshots (label → { peaks[], rms[], channelCount,
    // layout, ... }) — the Media pane's audio cards read their device's
    // peaks for the live level state window (same list the AV dialogs use).
    Q_INVOKABLE QVariantList inputMeterList() const { return inputLevels_; }
    // ---- Input metering (the only metering the bridge owns) --------------
    // There is NO program-mix meter any more. It read AudioMixer::ProgramLevel,
    // which could only ever publish from the WASAPI render callback of the
    // unreachable setAudioRender path — so it read a flat zero for its whole
    // life, and the monitor tiles' strips were really fed by the input taps
    // below. It went with that path: see the removal note on the pump in the
    // .cpp. Nothing here can acquire a sample for a "mix".
    // True while at least one INPUT tap is held (an audio/bus
    // card double-clicked in the Media pane). The
    // MonitorWall toolbar's "audio" clear button reads this (plus
    // mediaOnAir&&mediaIsAudio).
    Q_PROPERTY(bool anyInputMetering READ anyInputMetering NOTIFY audioMeteringChanged)
    bool anyInputMetering() const { return !requestedMeters_.isEmpty(); }
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
    // WHY a held camera/screen tap will never produce a frame, in the platform's
    // own words ("device activation failed", "window closed", …), or "" while the
    // tap is fine — which includes merely warming up. This is the answer the UI
    // never had: a tap whose open failed used to look exactly like one that was
    // still connecting, so the card pulsed its ring for as long as it was on
    // screen with nothing anywhere to say why (see IVideo::PreviewFailure). Empty
    // for a label with no tap, and for NDI (its receiver reports through its
    // frames, not a reason).
    Q_INVOKABLE QString previewFailure(const QString &deviceLabel);
    // THE STATUS LIGHT'S OWN PROBE — the cached roster first, and if that cannot
    // answer, a PING: one device refresh that NAMES this source, queued, so the
    // light comes right by itself. This is what "the probe gate should not affect
    // this light — give them access to ping so they can stay updated without me
    // having to open the gate" asks for: inputSourceReachable() is a pure cache
    // read, and the cache is only filled by an enumeration that the MEDIA/SOURCE
    // PICKERS used to trigger (the "gate" — the window list in particular is only
    // walked when a picker asks). A card whose only interest is a status light
    // therefore sat on a stale answer until the operator opened a screen they did
    // not need.
    //
    // Cost: the refresh is the SAME async, used-device-filtered sweep boot runs
    // (off the GUI thread), with one addition — the label the light is asking
    // about is added to what that sweep walks (see lightAsks_ below), so the
    // device the light is about is the device this sweep asks for. It is refused
    // while a scan is in flight and rate-limited here, so a binding re-evaluating
    // at the pump's rate cannot queue a second enumeration.
    Q_INVOKABLE bool inputSourceAlive(const QString &label, const QString &kind);
    // THE STATUS LIGHT'S LIVE HALF — is anything actually ARRIVING from this
    // source right now? "" while the app is not pulling it at all (or was, but
    // stopped — see kWatchWindowMs), "ok" while it is and frames are being handed
    // over — a STILL feed counts, since a frame that arrives is a frame whether
    // or not its bytes changed — "starved" while it is being WATCHED and none
    // have arrived for kDeliveryWindowMs (a tap that opened but never produced, a
    // window that closed under it, a network feed that stopped).
    //
    // NOTHING IS OPENED TO ANSWER IT, which is the whole point: it reads the
    // taps the app ALREADY holds (a card's thumbnail, a held monitor, the output
    // take, a meter tap) and the timestamps those taps write. "" therefore means
    // "only presence can be known" — the light stays on its roster answer rather
    // than opening a device to decorate a card.
    Q_INVOKABLE QString inputDelivery(const QString &label, const QString &kind);
    // A tap that has just opened is not starved for this long: opening a camera
    // or a screen-capture session takes ~100-300 ms and an NDI receiver needs a
    // moment on the network, so the verdict waits before it calls a source
    // silent. Past it, a tap that has delivered NOTHING reads starved — and
    // previewFailure() is where the platform's reason for that lives.
    static constexpr qint64 kDeliveryWindowMs = 5000;
    // WATCHED, not merely HELD. Silence only means starvation if somebody was
    // actually asking the tap for frames: a monitor that scrolled out of view, a
    // card behind a closed dock, or a thumbnail nobody renders pulls nothing, and
    // reading that as "starved" would paint a perfectly good source amber for the
    // crime of not being looked at. So the verdict needs BOTH clocks — the last
    // pull and the last frame — and a tap nobody pulled from within this window
    // answers "" (see previewPulledAt_).
    static constexpr qint64 kWatchWindowMs = 3000;
    // The two clocks behind inputDelivery's live half, as ages in ms (-1 = never),
    // for the self-test's diagnostics: WHICH half a "starved" came from is the
    // difference between a dead source and a tap nothing is watching.
    Q_INVOKABLE qint64 previewDeliveryAgeMs(const QString &label) const;
    Q_INVOKABLE qint64 previewPullAgeMs(const QString &label) const;
    // The newest frame of the NDI receiver tapped for this source name —
    // the SAME contract previewFrameFor serves the camera/screen taps (a
    // null QImage = no receiver or no frame yet, the warm-up signal).
    // Converts the engine's UYVY/BGRA frame to RGBA on the fly (decode-once
    // is unnecessary — the engine hands raw pixels, not a JPEG).
    QImage previewNdiFrameFor(const QString &sourceName);
    // The SAME label→engine-id resolution previewFrameFor uses, exposed
    // standalone: LiveOutputService::takeInput needs the resolved PAL device
    // id (not the roster label) to tell the engine's CompositorState which
    // tap SceneBuilder should pull real pixels from — "" if the label never
    // resolved (tap never started, or resolution failed).
    // Q_INVOKABLE as well: the same resolution a status light's live half and the
    // self-test's diagnostics need to say WHICH device a tap resolved to (a tap
    // that never resolved has nothing to deliver from) — it was C++-only, so a
    // QML caller read `resolvedPreviewDeviceId is not a function`.
    Q_INVOKABLE QString resolvedPreviewDeviceId(const QString &label) const { return previewIds_.value(label.trimmed()); }
    bool ndiAvailable() const { return ndiAvailable_; }
    QString ndiStatus() const { return ndiStatus_; }
    QString ndiState() const { return ndiState_; }
    QString ndiVersion() const { return ndiVersion_; }
    QString ndiDownloadUrl() const;
    // The feature registry snapshot + the switch (see pluginFeatures).
    QVariantList pluginFeatures() const;
    Q_INVOKABLE void setPluginFeatureEnabled(const QString &featureId, bool enabled);
    bool sdiAvailable() const { return sdiAvailable_; }
    QString sdiStatus() const { return sdiStatus_; }
    QVariantList sdiDevices() const { return sdiDevices_; }
    // Opens the NDI runtime download page in the user's browser.
    Q_INVOKABLE void openNdiDownloadPage();
    // Receivers currently CONNECTED to the NDI program sender (-1 = no
    // sender / the SDK can't tell). 0 while frames flow = nothing on the
    // network accepts our video — the firewall/discovery case.
    Q_PROPERTY(int ndiConnectedReceivers READ ndiConnectedReceivers NOTIFY ndiConnectedReceiversChanged)
    int ndiConnectedReceivers() const { return ndiConnectedReceivers_; }
    // ONE-TIME firewall authorization prompt (Windows Security Alert):
    // Windows shows it when the app first opens a listening socket, but a
    // missed/cancelled dialog silently blocks INBOUND NDI connections
    // forever (outbound receive keeps working — input w/o output). Runs the
    // app once on the network profile with inbound rules added. Returns:
    // "added" | "denied" | "failed" | "unavailable".
    Q_INVOKABLE QString requestNdiFirewallAccess();
    // Did the elevated rule add from requestNdiFirewallAccess() land? The
    // request returns "prompt" and the verdict arrives asynchronously (the
    // user answers UAC) — the UI calls this from a short retry timer.
    Q_INVOKABLE bool checkNdiFirewallRule();
    // True once the ask has fired this SESSION (deliberately not persisted:
    // the firewall rule itself is the saved permission — see
    // requestNdiFirewallAccess; a refused OS prompt re-asks next session).
    Q_PROPERTY(bool ndiFirewallPrompted READ ndiFirewallPrompted NOTIFY ndiFirewallPromptedChanged)
    bool ndiFirewallPrompted() const { return ndiFirewallPrompted_; }
    // Poll hook (LiveOutputService's 10 Hz poll calls this): re-reads the
    // SDK's connected-receiver count and emits on change. Cheap (1 Hz cap).
    // GUI THREAD ONLY — the poll's thread, which is also the thread the
    // probe's own completion is posted back to (see ndiConnectionProbeBusy_).
    void refreshNdiConnections();
    // Re-checks for the runtime right now (after the user installed it).
    Q_INVOKABLE void recheckNdi();
    // Called from main.cpp on QGuiApplication::aboutToQuit — orderly
    // teardown of the 29 systems (threads joined, database flushed) instead
    // of letting process exit cut them off mid-flight.
    void shutdown();
    // The "ndi" feature switch's runtime effect (see setPluginFeatureEnabled):
    // disabling tears down every NDI receiver tap and the output take.
    void applyNdiFeatureState(bool enabled);

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
    void bootErrorChanged();
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
    // Fired when the sweep that carried the app's own device set completed —
    // see bootSweepDone.
    void bootSweepDoneChanged();

    // ~20×/s while a meter tap is live: inputLevels_ was refreshed from the
    // PAL's snapshots. QML reads the property fresh on each emission — a
    // deliberately modest rate (not the ~480Hz the captures run at) so the
    // meter dots animate like a real VU without flooding the GUI thread.
    void inputLevelsChanged();
    // The feature registry moved (a switch flipped, boot auto-disabled
    // something) — the Plugins screen re-reads pluginFeatures.
    void pluginFeaturesChanged();
    // NDI program-sender connection telemetry + firewall authorization state.
    void ndiConnectedReceiversChanged();
    void ndiFirewallPromptedChanged();
    void audioMeteringChanged();

private:
    explicit EngineBridge(QObject *parent = nullptr);
    Q_DISABLE_COPY(EngineBridge)

    QString bootError_;
    QString bootSummary_;
    QStringList bootInUse_;
    QString logPath_;
    QThreadPool deviceEnumerationPool_;
    bool deviceEnumerationBusy_ = false;       // GUI-thread owned
    bool deviceEnumerationPending_ = false;    // coalesce hot-plug bursts
    bool deviceEnumerationStopping_ = false;   // shutdown gate

    // ---- Engine → UI relay (see startRelay in EngineBridge.cpp) ----------
    // The kernel drives everything; the UI only relays. Attaches a Logger
    // sink (engine warnings/errors/fatals) and engine-EventBus subscriptions
    // (the curated user-facing event set); each hops to the GUI thread via a
    // queued invoke before touching the UI-side EventBus.
    void startRelay();
    void stopRelay();

    // Platform discovery runs on a worker; roster properties are updated on
    // the GUI thread after the worker returns.
    void enumerateDevices();
    void enumeratePlatformDevicesAsync();
    void updateBootSummary();
    // NDI runtime probe + source discovery OFF the GUI thread (runs on the
    // global QThreadPool; results land via a queued invokeMethod + one
    // devicesChanged emit). The first successful check loads and initializes
    // the NDI runtime DLL — that must never happen inline on the GUI thread:
    // the "I've installed it — check again" button ran the whole load inline
    // and took the app down. Coalesced by ndiProbeBusy_.
    void probeNdiAsync();
    QVariantList audioDevices_;
    QVariantList screenDevices_;
    QVariantList videoDevices_;
    QVariantList ndiSources_;
    bool sdiAvailable_ = false;
    QString sdiStatus_;
    QVariantList sdiDevices_;

    // ---- Live input metering state --------------------------------------
    QVariantList inputLevels_;          // the published snapshot set
    // ONE "first meter signal" log line per session (see pumpMeterSnapshot) —
    // the mirror of the video chain's one-shot "first preview frame". Without
    // it a dead-looking meter is indistinguishable in engine.log from a
    // genuinely silent device (what the "the meters never move" reports were).
    bool firstMeterSignal_ = false;
    class QTimer *inputMeterPump_ = nullptr;   // 50 ms poll while taps are live
    QMap<uint32_t, QString> inputLabels_;   // waveIn device id → roster label
    // The used-device set and its one-shot gate (see requestUsedDevices).
    QStringList usedAudioLabels_;
    QStringList usedVideoLabels_;
    QStringList usedDisplayNames_;
    bool usedDeviceSweepOnly_ = true;    // false once anything asked for the full roster
    bool deviceHandoverSeen_ = false;    // the app has said which devices it uses (see requestUsedDevices)
    bool bootDeviceCheckDone_ = false;   // the missing-device check runs once per session
    QStringList missingUsedDevices_;     // what that check found gone (see missingUsedDevices)
    bool bootSweepDone_ = false;         // the app's own device set has been swept (see bootSweepDone)
    static bool labelWanted(const QStringList &wanted, const QString &label);
    // Every label with a requested tap, REFCOUNTED — the board row that
    // creates a device row and a dialog editing that same device can both
    // hold it open independently (e.g. closing the Edit dialog must not
    // kill the board row's own meter). Labels resolve again on
    // re-enumeration so hot-plug re-binds.
    QMap<QString, int> requestedMeters_;
    void resolveAndStartMeter(const QString &deviceLabel);
    // The 50 ms meter pump the device taps ride — created once, started whenever
    // anything holds a meter.
    void ensureMeterPump();
    // Resolve a roster DEVICE label to its PAL wavein number (the one place that
    // walks the cached roster): false when the label names no input device on
    // this machine, or the id is not a WASAPI wavein number at all.
    bool resolveDeviceTap(const QString &deviceLabel, uint32_t *deviceId) const;

    // ---- Video/NDI row audio: the stream taps behind the inputLevels list --
    QMap<QString, int> requestedStreams_;      // label → holds (refcounted)
    QMap<QString, QString> streamKinds_;       // label → the kind it was opened as
    QMap<QString, QString> streamSources_;     // label → what the producer opens
    QMap<QString, uint32_t> streamTapIds_;     // label → its stream tap id
    quint32 nextStreamTap_ = 0;                // ids counted from IAudio::kStreamTapBase
    // "label\nkind" → when its open was last REFUSED. The mix's source refresh
    // runs ~20×/s, so a row whose media file is gone (or an NDI kind nothing
    // feeds yet) is retried on a slow beat instead of re-attempting — and
    // re-logging — on every tick.
    QMap<QString, qint64> streamRefusedAt_;
    // An EXISTING tap is reused (never replaced): a stream tap is a decoder, so
    // a second hold must not restart the row's audio mid-sentence.
    bool ensureStreamTap(const QString &label, const QString &kind, const QString &source);
    // Stop a stream tap that nobody holds any more (a UI hold or the mix).
    void releaseStreamTap(const QString &label);

    // One tick of the shared meter pump: the input taps' published snapshots.
    void pumpMeterSnapshot();

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
    // HOLD-LAST-FRAME (see heldPreviewFrame in the .cpp): per tap key — the
    // device id for camera/screen taps, the NDI source name for receivers —
    // the newest frame that actually arrived, served back whenever a pull
    // comes up empty so panes never blink to their glyphs between frames.
    // Cleared everywhere the tap itself dies (per-owner stops + stopAll).
    QMap<QString, QImage> previewLastFrame_;
    QImage heldPreviewFrame(const QString &key) const;
    void setHeldPreviewFrame(const QString &key, const QImage &frame);
    // NDI receiver taps: source name → engine receiver id ("ndi-recv-N").
    // Owner counting rides the shared previewOwners_ table keyed by the
    // RAW source name (no "ndi:" prefix) — an NDI full name has the SDK's
    // own "<HOST> (<sender>)" shape, distinctive enough that a real camera/
    // screen label collision is not a practical concern.
    QHash<QString, QString> ndiTaps_;
    // Snapshot of label -> owners taken right before the "ndi" feature
    // switch tears every tap down (applyNdiFeatureState) — card/dialog/
    // board previews have no "my tap died, please re-fetch" signal of their
    // own (only the OUTPUT take is re-synced via LiveOutputService::
    // clearNdiInput), so without this they stayed permanently dark after an
    // off->on cycle even once NDI was usable again. Replayed (one
    // startNdiPreview call per label/owner pair) the moment the switch goes
    // back on, then cleared.
    QHash<QString, QSet<QString>> ndiTapsPendingReattach_;
    bool ndiAvailable_ = false;
    int ndiRetries_ = 0;   // deferred NDI re-query budget (2) per boot/refreshDevices()
    // One async NDI probe at a time (see probeNdiAsync): a second recheck
    // while one is in flight coalesces into the running probe instead of
    // stacking a second runtime load/discovery pass.
    std::atomic<bool> ndiProbeBusy_{false};
    // The SDK's connected-receiver query can block while NDI is sending, so
    // the 1 Hz live telemetry refresh is coalesced onto the worker pool too.
    //
    // GUI THREAD ONLY, and deliberately NOT atomic. Every touch of it is on
    // the poll's own thread: refreshNdiConnections() arms it (the poll calls
    // it), and the WORKER only ever clears it by POSTING BACK to that thread
    // — QMetaObject::invokeMethod's context object lives here, so the
    // callback runs here. Nothing on the pool thread reads or writes it, and
    // the sibling below (ndiConnectedReceivers_) has always been a plain int
    // written by that same callback. An atomic bought nothing and cost a
    // Release warning: the store inside the queued lambda, inlined through
    // Qt's functor plumbing, made GCC's object-size pass think a possibly-null
    // pointer was being written through ('writing 1 byte into a region of size
    // 0 overflows the destination [-Wstringop-overflow=]').
    bool ndiConnectionProbeBusy_ = false;
    int ndiConnectedReceivers_ = -1;   // SDK connection count, poll-refreshed
    bool ndiFirewallPrompted_ = false;   // once per install (settings-backed)
    qint64 lastEnumerationMs_ = 0;   // throttles refreshDevices()
    qint64 lastAudioEnumerationMs_ = 0; // throttles the audio-only picker refresh
    // Throttles the status light's own ping (see inputSourceAlive): the light is
    // read from bindings that re-evaluate at the pump's rate, so an unthrottled
    // ping would queue a device refresh every tick.
    qint64 lastLightPingMs_ = 0;
    // The sources the lights have asked about since the last sweep (see
    // inputSourceAlive). Each is added to what the next enumeration WALKS, so the
    // answer is about the device the light names and not about whatever the
    // setup's used-device set happened to contain; consumed (cleared) by the
    // sweep that takes the snapshot, which is why several lights asking between
    // two sweeps cost one enumeration, not one each.
    QStringList lightAsks_;
    // One diagnostic line per (label, kind) a light could not answer for, so a
    // mislabelled source is distinguishable in engine.log from a device that is
    // genuinely gone. See inputSourceAlive.
    QSet<QString> lightProbeLogged_;
    // When each OPEN tap last handed over a real frame, by tap key (the device id
    // for camera/screen taps, the source name for NDI receivers — the same key
    // previewLastFrame_ uses). Written where the frame is actually fetched and
    // cleared wherever the tap dies, so a closed tap can never read as a live one.
    // See inputDelivery.
    QMap<QString, qint64> previewDeliveredAt_;
    // ...and when each open tap was last ASKED for a frame, same key. Held and
    // watched are different facts: a tap can be held with nothing rendering it,
    // and then its silence says nothing about the device (see kWatchWindowMs).
    QMap<QString, qint64> previewPulledAt_;
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
