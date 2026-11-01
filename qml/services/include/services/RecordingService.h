#pragma once

// The QML face of the engine's RecordingEngine (Phase 16, docs/specs/28).
//
// A take belongs to ONE OUTPUT and ONE PRESET. Settings · Recording keeps the
// PRESETS (PresetListModel: a Recording or a Streaming configuration); Settings ·
// Routing wires an output to one, and that output's card starts / stops its take,
// recording with THAT preset's container / encoder / bitrate / folder:
//   RECORD  → CreateProfile(the preset's fields) + StartRecording against the
//             output's own graph node (the engine's tap listens to the buses WIRED to
//             it), then a 1 Hz pump that drives the engine's Tick() (durations,
//             segments, disk monitor — the engine is externally-clocked) and
//             refreshes the QML status. STOP → StopRecording (the file finalizes).
//   STREAM  → RoutingService gives each STREAM row its own output node carrying its
//             preset's RTMP target; the engine models and validates the destination.
//             An actual RTMP pusher is a future transport provider (the engine's
//             honest boundary today — no ffmpeg/rtmp client exists in-tree).
// Failures surface as toasts through the standard EventBus.notify path (same
// convention as MediaLibraryService).

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace bps::recording {
class RecordingEngine;
}

class QTimer;
class QQmlEngine;
class QJSEngine;

class RecordingService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Is a recording session active right now (Recording or Paused)?
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    // True while PAUSED (Recording() above stays true — one source of truth).
    Q_PROPERTY(bool paused READ paused NOTIFY recordingChanged)
    // Is a stream destination configured on the graph?
    Q_PROPERTY(bool streaming READ streaming NOTIFY recordingChanged)
    // Live session state ("" when idle): elapsed time, size, dropped frames,
    // the engine-selected encoder, the current file path and disk health.
    Q_PROPERTY(QVariantMap status READ status NOTIFY recordingChanged)
    // REAL free-disk snapshot from the engine (free GB / percent / estimated
    // hours of headroom at the current bitrate) — the meter reads this.
    Q_PROPERTY(QVariantMap storage READ storage NOTIFY storageChanged)
    // What THIS BUILD's recording engine can actually do, read from the engine's
    // registered providers (not from the machine's GPU): the encoder names ("Software",
    // later "NVENC"...) and container names ("MP4") a preset may pick. EMPTY = not known
    // yet (the engine has not finished booting), which the dialog treats as "offer all".
    Q_PROPERTY(QStringList availableEncoders READ availableEncoders NOTIFY capabilitiesChanged)
    Q_PROPERTY(QStringList availableContainers READ availableContainers NOTIFY capabilitiesChanged)
    // Where a picture recording is written when its preset names no folder: <app data>/recording/video.
    Q_PROPERTY(QString defaultFolder READ defaultFolder NOTIFY capabilitiesChanged)
    // ...and for an audio-only preset: <app data>/recording/audio (video ones: .../recording/video).
    Q_PROPERTY(QString defaultAudioFolder READ defaultAudioFolder NOTIFY capabilitiesChanged)
    // The graph node the LIVE take was started on ("out:<stable id>") — "" while
    // idle. The
    // Settings · Routing canvas reads it so the card that OWNS the take is the one
    // that offers Stop: the engine runs one session at a time, and its node names it.
    Q_PROPERTY(QString recordingNode READ recordingNode NOTIFY recordingChanged)

public:
    static RecordingService &instance();
    static RecordingService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~RecordingService() override;

    bool recording() const { return recording_; }
    bool paused() const { return paused_; }
    bool streaming() const { return streaming_; }
    QVariantMap status() const { return status_; }
    QVariantMap storage() const { return storage_; }
    QString recordingNode() const { return recordingNode_; }
    QStringList availableEncoders() const { return availableEncoders_; }
    QStringList availableContainers() const { return availableContainers_; }
    QString defaultFolder() const { return defaultFolder_; }
    QString defaultAudioFolder() const { return defaultAudioFolder_; }

    // ---- Transport -------------------------------------------------------
    // "Record" without naming an output (File > Record, Ctrl+R, the automation
    // action): starts the FIRST Recording output on the Routing canvas, with the
    // preset it is wired to. false + a toast when there is no Recording output or it
    // has no preset yet.
    Q_INVOKABLE bool startPrimaryRecording();
    // Start a take ON ONE OUTPUT NODE — the Settings · Routing canvas's
    // "Recording" card — with `settings` = that output's PRESET config.
    //
    // WHY IT MATTERS: RecordingEngine::StartPump attaches its audio tap to the
    // node it was STARTED on (AudioEngine::AddMixTap(nodeId, …), "the listener
    // hears only the buses wired to this recording's output node"). So a take
    // started from a node records exactly the bus the canvas wired to that
    // node — and a node with nothing wired to it is refused here rather than
    // producing a file that looks like a take and holds silence.
    Q_INVOKABLE bool startRecordingOn(const QString &nodeId, const QVariantMap &settings);
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void togglePause();
    // A named marker at the current position (the service notes it).
    Q_INVOKABLE void addMarker(const QString &label);

    // A native folder picker for a preset's output folder: the picked path, or "" when
    // cancelled.
    Q_INVOKABLE QString pickFolder(const QString &title);   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; nothing runs after the pick but a string assignment

    // ---- Storage ---------------------------------------------------------
    // Re-reads the free-disk snapshot (also refreshed by the 1 Hz pump while
    // recording — the disk monitor's cadence).
    Q_INVOKABLE void refreshStorage();
    // Re-reads the engine's registered encoders / containers (boot, and once at start).
    Q_INVOKABLE void refreshCapabilities();

    // Waits out the pump before engine shutdown.
    void shutdown();

signals:
    void recordingChanged();
    void storageChanged();
    void capabilitiesChanged();

private:
    explicit RecordingService(QObject *parent = nullptr);

    // The one body behind startPrimaryRecording / startRecordingOn: `nodeId` is the
    // output node the canvas wired.
    bool beginRecording(const QVariantMap &settings, const QString &nodeId);
    static RecordingService *s_instance;

    // Re-reads status_/recording_/paused_/streaming_ from the engine session.
    void refreshStatus();
    // The 1 Hz pump: engine Tick(1000) + status/storage refresh. Runs while
    // recording; self-stops otherwise.
    void pumpTick();

    bool recording_ = false;
    bool paused_ = false;
    bool streaming_ = false;
    // Which node the live take listens to (see the property); cleared with the
    // session it belongs to.
    QString recordingNode_;
    QVariantMap status_;
    QVariantMap storage_;
    QStringList availableEncoders_;
    QStringList availableContainers_;
    QString defaultFolder_;
    QString defaultAudioFolder_;
    QTimer *pump_ = nullptr;
    // The active engine session id + the profile it rides.
    QString recordingId_;
    QString profileId_;
};
