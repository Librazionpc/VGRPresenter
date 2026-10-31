#pragma once

// The QML face of the engine's RecordingEngine (Phase 16, docs/specs/28) —
// the Recording & Streaming settings screen's window onto REAL recording.
//
// One user gesture maps to one engine session:
//   RECORD  → CreateProfile(the screen's current settings) + StartRecording
//             against the production graph's MASTER PROGRAM BUS node, then a
//             1 Hz pump that drives the engine's Tick() (durations, segments,
//             disk monitor — the engine is externally-clocked) and refreshes
//             the QML status. STOP → StopRecording (the file finalizes on
//             disk; the engine's sink-model containers write the real bytes).
//   STREAM  → the RTMP destination: an Output node wired to the master buses
//             with networkTarget = the URL+key. The engine models the
//             destination and validates it (ValidateProduction); an actual
//             RTMP pusher is a future transport provider (the engine's
//             honest boundary today — no ffmpeg/rtmp client exists in-tree).
//
// Settings persist as one JSON blob in `session.recordingConfig` (platform,
// URL, key, bitrates, container/encoder, screens) so the screen comes back
// exactly as left. Failures surface as toasts through the standard
// EventBus.notify path (same convention as MediaLibraryService).

#include <QObject>
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
    // The persisted settings blob ({ platform, serverUrl, streamKey, ... });
    // writes go through setConfig() so they save.
    Q_PROPERTY(QVariantMap config READ config WRITE setConfig NOTIFY configChanged)
    // The graph node the LIVE take was started on ("out:<stable id>", or
    // "out:stream" for the RTMP row) — "" while idle and for a program take. The
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
    QVariantMap config() const { return config_; }
    QString recordingNode() const { return recordingNode_; }
    void setConfig(const QVariantMap &cfg);

    // ---- Transport -------------------------------------------------------
    // Start recording the program mix with the given settings (the screen's
    // current values: container/encoder/bitrate/resolution/screens). Returns
    // false + a toast on refusal (engine not running, disk full, already
    // recording, no program bus).
    Q_INVOKABLE bool startRecording(const QVariantMap &settings);
    // Start a take ON ONE OUTPUT NODE — the Settings · Routing canvas's
    // "Recording" card — instead of on the program.
    //
    // WHY IT MATTERS: RecordingEngine::StartPump attaches its audio tap to the
    // node it was STARTED on (AudioEngine::AddMixTap(nodeId, …), "the listener
    // hears only the buses wired to this recording's output node"), and
    // startRecording() above can only name the program's first bus plane. So a
    // take started from a node records exactly the bus the canvas wired to that
    // node — and a node with nothing wired to it is refused here rather than
    // producing a file that looks like a take and holds silence.
    Q_INVOKABLE bool startRecordingOn(const QString &nodeId, const QVariantMap &settings);
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void togglePause();
    // A named marker at the current position (the service notes it).
    Q_INVOKABLE void addMarker(const QString &label);

    // ---- Streaming (RTMP destination) ------------------------------------
    // Wire the RTMP destination: an Output node ("stream") connected to the
    // master buses with networkTarget = serverUrl + "/" + streamKey. Returns
    // false + a toast when the URL/key are unusable.
    Q_INVOKABLE bool applyStreamTarget(const QString &serverUrl, const QString &streamKey);
    Q_INVOKABLE void clearStreamTarget();

    // ---- Storage ---------------------------------------------------------
    // Re-reads the free-disk snapshot (also refreshed by the 1 Hz pump while
    // recording — the disk monitor's cadence).
    Q_INVOKABLE void refreshStorage();

    // Waits out the pump before engine shutdown.
    void shutdown();

signals:
    void recordingChanged();
    void storageChanged();
    void configChanged();

private:
    explicit RecordingService(QObject *parent = nullptr);

    // The one body behind startRecording / startRecordingOn. `nodeId` empty =
    // the program (first audio bus plane); otherwise the node the canvas wired.
    bool beginRecording(const QVariantMap &settings, const QString &nodeId);
    static RecordingService *s_instance;

    void loadConfig();
    void saveConfig();
    // Finds the master program bus's node id ("bus:<n>#a" — the first bus in
    // the graph; the routing board's first row is the program mix by
    // convention). "" when the graph has no buses yet.
    QString masterBusNodeId() const;
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
    QVariantMap config_;
    QTimer *pump_ = nullptr;
    // The active engine session id + the profile it rides.
    QString recordingId_;
    QString profileId_;
};
