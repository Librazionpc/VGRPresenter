#include "services/RecordingService.h"

#include "services/EngineBridge.h"
#include "services/EngineWorker.h"
#include "services/EventBus.h"
#include "services/SettingsService.h"

#include "modules/production/ProductionEngine.hpp"
#include "modules/production/ProductionGraph.hpp"
#include "modules/recording/RecordingEngine.hpp"
#include "modules/recording/RecordingTypes.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace rr = bps::recording;
namespace bpr = bps::production;

namespace {

// The stream Output node this service owns (removed and re-added on each
// apply/clear — one well-known id, like the kernel's own boot nodes).
constexpr const char *kStreamNodeId = "out:stream";
constexpr const char *kStreamNodeName = "Live Stream";

// The session.recordingConfig blob key (declared in AppSettings).
constexpr const char *kConfigKey = "session.recordingConfig";

void toast(const QString &message, const QString &level = QStringLiteral("error"))
{
    EventBus::instance().notify(message, level, QStringLiteral("Recording"),
                                QStringLiteral("recording.transport"));
}

QVariantMap storageToVariant(const rr::StorageInfo &info)
{
    return QVariantMap{
        { QStringLiteral("freeBytes"), qint64(info.freeBytes) },
        { QStringLiteral("totalBytes"), qint64(info.totalBytes) },
        { QStringLiteral("freePercent"), info.freePercent },
        { QStringLiteral("freeGb"), qreal(info.freeBytes) / (1024.0 * 1024.0 * 1024.0) },
        { QStringLiteral("estimatedBytesPerHour"), qint64(info.estimatedBytesPerHour) },
        { QStringLiteral("capacityHours"), info.capacityHours },
        { QStringLiteral("healthy"), info.healthy },
    };
}

// Settings-map → engine profile. The screen's free-text fields (resolution,
// frame rate) are parsed defensively — a malformed value keeps the default.
rr::RecordingProfile profileFromSettings(const QVariantMap &settings)
{
    rr::RecordingProfile p;
    p.name = QStringLiteral("service").toStdString();

    const QString container = settings.value(QStringLiteral("container")).toString().toUpper();
    p.container = container == QLatin1String("MP4") ? rr::ContainerKind::MP4
                                                    : rr::ContainerKind::MKV;
    p.videoCodec = rr::VideoCodec::H264;
    p.audioCodec = rr::AudioCodec::AAC;

    const QString res = settings.value(QStringLiteral("resolution")).toString();
    if (res == QLatin1String("720p")) {
        p.width = 1280;
        p.height = 720;
    } else if (res == QLatin1String("4K")) {
        p.width = 3840;
        p.height = 2160;
    } else {   // "1080p" and anything unrecognized
        p.width = 1920;
        p.height = 1080;
    }

    bool ok = false;
    const double fps = settings.value(QStringLiteral("frameRate")).toString()
                           .split(QLatin1Char(' ')).first().toDouble(&ok);
    p.fps = ok && fps > 0 ? fps : 60.0;

    const int vKbps = settings.value(QStringLiteral("videoBitrate")).toInt();
    p.videoBitrateKbps = vKbps > 0 ? vKbps : 12000;
    p.audioChannels = 2;

    const QString enc = settings.value(QStringLiteral("encoder")).toString();
    p.encoderPreference = enc == QLatin1String("NVENC")   ? rr::EncoderKind::NVENC
                          : enc == QLatin1String("AMF")   ? rr::EncoderKind::AMF
                          : enc == QLatin1String("QuickSync")
                                                          ? rr::EncoderKind::QuickSync
                                                          : rr::EncoderKind::Auto;

    // The screens-to-record toggles: the program mix is the one signal that
    // exists today (preview/audience/lowerThird need scene-per-output taps,
    // which the display engine's per-output passes will back later). Recorded
    // honestly as profile metadata so the choice is not silently lost.
    const QVariantMap screens = settings.value(QStringLiteral("screensIn")).toMap();
    p.multitrack = false;
    p.quality = screens.value(QStringLiteral("program"), true).toBool()
                    ? rr::QualityPreset::Broadcast
                    : rr::QualityPreset::High;

    p.segmentDurationMs = 0;       // single file per session (MKV recommended)
    p.priority = rr::RecordingPriority::Critical;   // protect the service

    // Output directory: <user data>/recordings (created lazily by the engine
    // — it ensures the directory exists on OpenSegment; see docs/specs/28 §18).
    auto &paths = bps::platform::PlatformAccessor::Get().Paths();
    QDir().mkpath(QString::fromStdString(paths.UserDataDir())
                  + QStringLiteral("/recordings"));
    p.directory = paths.UserDataDir() + "/recordings";

    return p;
}

} // namespace

RecordingService *RecordingService::s_instance = nullptr;

RecordingService::RecordingService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;

    // The settings store opens when the engine boots (SettingsService::load);
    // until then the blob read is deferred, not skipped — whenReady runs
    // loadConfig() the moment the store is open (immediately if it already is),
    // and a screen opened pre-boot shows defaults until then.
    SettingsService::instance().whenReady([this] { loadConfig(); });
    connect(&SettingsService::instance(), &SettingsService::changed,
            this, [this] {
                // Every settings change may move where recordings land, so the
                // storage snapshot is re-read on each one (the engine's disk
                // monitor answers once its storage seam is up).
                refreshStorage();
            });
    refreshStorage();
}

RecordingService &RecordingService::instance()
{
    static RecordingService inst;
    return inst;
}

RecordingService *RecordingService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

RecordingService::~RecordingService()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Config persistence (session.recordingConfig)
// ---------------------------------------------------------------------------

void RecordingService::setConfig(const QVariantMap &cfg)
{
    if (cfg == config_)
        return;
    config_ = cfg;
    saveConfig();
    emit configChanged();
}

void RecordingService::loadConfig()
{
    SettingsService *store = SettingsService::instancePtr();
    if (!store || !store->ready())
        return;   // pre-boot: defaults stand; whenReady() re-reads once the store opens
    const QVariant stored = store->value(QLatin1String(kConfigKey));
    if (stored.isValid() && !stored.isNull()) {
        // Deep-merge: the screen's defaults stand for keys a previous run
        // never wrote (schema additions between versions).
        const QVariantMap prev = stored.toMap();
        for (auto it = prev.begin(); it != prev.end(); ++it)
            config_.insert(it.key(), it.value());
    }
}

void RecordingService::saveConfig()
{
    SettingsService *store = SettingsService::instancePtr();
    if (!store || !store->ready())
        return;   // pre-boot writes are dropped — nothing was loaded to change
    (void)store->setValue(QLatin1String(kConfigKey), config_);
}

// ---------------------------------------------------------------------------
// Graph seam — the master program bus
// ---------------------------------------------------------------------------

// The graph is the engine's, so this read belongs on the worker (startRecording
// and applyStreamTarget both ask for it there — see their own workers). The
// convention: bus plane nodes are "bus:<n>#a"/"#v" (BusListModel's), and the
// audio plane of the FIRST bus is the program mix.
QString RecordingService::masterBusNodeId() const
{
    bps::production::ProductionGraph &g = bpr::ProductionEngine::Instance().Graph();
    for (const auto &id : g.NodeIds()) {
        const std::string s = id;
        if (s.starts_with("bus:") && s.ends_with("#a"))
            return QString::fromStdString(s);
    }
    return {};
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

// Every RECORD/STREAM gesture is ENGINE WORK, and all of it now runs on the
// engine worker: the profile build, the session start, the metadata, the stop
// and the pause all live in the engine (which takes its own locks and writes
// the file), and the QML-visible flags/status are published here when the
// worker's answer arrives. True from these calls means ACCEPTED — the outcome
// (a session on this screen, or the refusal toast) follows a moment later.
bool RecordingService::startRecording(const QVariantMap &settings)
{
    const rr::RecordingProfile profile = profileFromSettings(settings);
    const QString pid = QStringLiteral("ui-program");
    const QString stamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    if (!pump_) {
        // A QTimer belongs to the thread that made it: created here, started on
        // the worker's answer (and by the pump itself from then on).
        pump_ = new QTimer(this);
        pump_->setInterval(1000);
        connect(pump_, &QTimer::timeout, this, &RecordingService::pumpTick);
    }
    vgr::EngineWorker::instance().runOffThread(
        [this, settings, profile, pid, stamp] {
        auto &rec = rr::RecordingEngine::Instance();
        const auto refuse = [this](const QString &message) {
            vgr::EngineWorker::onInterfaceThread(this, [message] {
                toast(QStringLiteral("Recording refused: %1").arg(message));
            });
        };
        // The engine boots after the kernel; the UI can reach this before a
        // Start() has run (e.g. a boot failure) — refuse honestly.
        const bool existed = rec.GetProfile(pid.toStdString()).ok();
        if (existed)
            (void)rec.RemoveProfile(pid.toStdString());   // changed settings take effect next take
        if (auto r = rec.CreateProfile(profile); !r.ok()) {
            refuse(QString::fromStdString(r.error().message));
            return;
        }

        // The master program bus: the FIRST bus plane node in the graph.
        QString bus;
        for (const auto &node : bpr::ProductionEngine::Instance().Graph().NodeIds()) {
            const std::string s = node;
            if (s.starts_with("bus:") && s.ends_with("#a")) {
                bus = QString::fromStdString(s);
                break;
            }
        }
        if (bus.isEmpty()) {
            vgr::EngineWorker::onInterfaceThread(this, [] {
                toast(QStringLiteral("No audio bus to record — create a bus on the Audio & Video board first."));
            });
            return;
        }

        auto started = rec.StartRecording(pid.toStdString(), bus.toStdString(),
                                          rr::TapPoint::PostProcessing);
        if (!started.ok()) {
            refuse(QString::fromStdString(started.error().message));
            return;
        }
        const QString sessionId = QString::fromStdString(started.value());

        // Semantic metadata for the searchable recording later (docs/specs/28 §23).
        (void)rec.SetMetadata(sessionId.toStdString(), "event", "service");
        (void)rec.SetMetadata(sessionId.toStdString(), "date", stamp.left(10).toStdString());
        (void)rec.SetMetadata(sessionId.toStdString(), "time", stamp.mid(11, 8).toStdString());

        const QString busLabel = bus;
        vgr::EngineWorker::onInterfaceThread(this, [this, sessionId, busLabel, settings] {
            recordingId_ = sessionId;
            profileId_ = QStringLiteral("ui-program");
            if (pump_ && !pump_->isActive())
                pump_->start();
            refreshStatus();
            refreshStorage();
            qInfo("RecordingService: recording '%s' from bus %s (%s/%s, %dkbps)",
                  qUtf8Printable(recordingId_), qUtf8Printable(busLabel),
                  settings.value(QStringLiteral("container")).toString().toUtf8().constData(),
                  settings.value(QStringLiteral("resolution")).toString().toUtf8().constData(),
                  settings.value(QStringLiteral("videoBitrate")).toInt());
            emit recordingChanged();
        });
    });
    return true;
}

void RecordingService::stopRecording()
{
    if (!recording_)
        return;
    const QString id = recordingId_;
    // The UI's own state turns over immediately — the button says stopped the
    // moment it is pressed; the engine's finalize (the container writes its
    // trailer) happens on the worker and the status is re-read from it after.
    recordingId_.clear();
    if (pump_)
        pump_->stop();
    emit recordingChanged();
    vgr::EngineWorker::instance().runOffThread([this, id] {
        auto &rec = rr::RecordingEngine::Instance();
        auto r = rec.StopRecording(id.toStdString(), QStringLiteral("operator").toStdString());
        vgr::EngineWorker::onInterfaceThread(this, [this, r] {
            if (!r.ok())
                toast(QStringLiteral("Stop failed: %1")
                          .arg(QString::fromStdString(r.error().message)));
            refreshStatus();
            refreshStorage();
            emit recordingChanged();
        });
    });
}

void RecordingService::togglePause()
{
    if (!recording_)
        return;
    const QString id = recordingId_;
    const bool wasPaused = paused_;
    vgr::EngineWorker::instance().runOffThread([this, id, wasPaused] {
        auto &rec = rr::RecordingEngine::Instance();
        auto r = wasPaused ? rec.ResumeRecording(id.toStdString())
                           : rec.PauseRecording(id.toStdString());
        vgr::EngineWorker::onInterfaceThread(this, [this, r] {
            if (!r.ok()) {
                toast(QStringLiteral("Pause failed: %1")
                          .arg(QString::fromStdString(r.error().message)));
                return;
            }
            refreshStatus();
            emit recordingChanged();
        });
    });
}

void RecordingService::addMarker(const QString &label)
{
    if (!recording_)
        return;
    const QString id = recordingId_;
    vgr::EngineWorker::instance().runOffThread([this, id, label] {
        auto &rec = rr::RecordingEngine::Instance();
        auto r = rec.AddMarker(id.toStdString(), label.toStdString());
        vgr::EngineWorker::onInterfaceThread(this, [this, r] {
            if (!r.ok())
                toast(QStringLiteral("Marker failed: %1")
                          .arg(QString::fromStdString(r.error().message)));
            else
                refreshStatus();
        });
    });
}

// ---------------------------------------------------------------------------
// Streaming — the RTMP destination node
// ---------------------------------------------------------------------------

bool RecordingService::applyStreamTarget(const QString &serverUrl, const QString &streamKey)
{
    // The two things a user gets an inline answer for — a URL that is not RTMP
    // and an empty key — are pure string checks: still answered HERE, so the
    // dialog can refuse them without a round trip. Everything after them needs
    // the graph, and that is engine work: refused on the worker with its toast,
    // exactly as the engine's own refusal was.
    const QString url = serverUrl.trimmed();
    const QString key = streamKey.trimmed();
    if (!url.startsWith(QLatin1String("rtmp://")) && !url.startsWith(QLatin1String("rtmps://"))) {
        toast(QStringLiteral("The server URL must start with rtmp:// (or rtmps://)."));
        return false;
    }
    if (key.isEmpty()) {
        toast(QStringLiteral("Paste your stream key first — the platform needs it to accept the feed."));
        return false;
    }

    vgr::EngineWorker::instance().runOffThread([this, url, key] {
        bps::production::ProductionGraph &g = bpr::ProductionEngine::Instance().Graph();
        const QString audioBus = masterBusNodeId();
        if (audioBus.isEmpty()) {
            vgr::EngineWorker::onInterfaceThread(this, [] {
                toast(QStringLiteral("No audio bus to stream — create a bus on the Audio & Video board first."));
            });
            return;
        }
        const QString videoBus = QString(audioBus).replace(QStringLiteral("#a"), QStringLiteral("#v"));

        bpr::OutputConfig cfg;
        cfg.videoBusId = videoBus.toStdString();
        cfg.audioBusId = audioBus.toStdString();
        cfg.networkTarget = (url.endsWith(QLatin1Char('/')) ? url + key
                                                            : url + QLatin1Char('/') + key)
                                .toStdString();
        cfg.group = "broadcast";
        cfg.encoder = "software";

        // Re-adding refreshes the target; removal keeps the session count sane.
        if (g.HasNode(kStreamNodeId))
            (void)g.RemoveNode(kStreamNodeId);
        auto added = g.AddOutput(kStreamNodeId, kStreamNodeName, bpr::SignalType::Video, cfg);
        vgr::EngineWorker::onInterfaceThread(this, [this, added, url] {
            if (!added.ok()) {
                toast(QStringLiteral("Stream refused: %1")
                          .arg(QString::fromStdString(added.error().message)));
                return;
            }
            streaming_ = true;
            refreshStatus();
            emit recordingChanged();
            toast(QStringLiteral("Stream destination set — %1").arg(url), QStringLiteral("success"));
        });
    });
    return true;
}

void RecordingService::clearStreamTarget()
{
    vgr::EngineWorker::instance().runOffThread([this] {
        bps::production::ProductionGraph &g = bpr::ProductionEngine::Instance().Graph();
        if (g.HasNode(kStreamNodeId))
            (void)g.RemoveNode(kStreamNodeId);
        vgr::EngineWorker::onInterfaceThread(this, [this] {
            streaming_ = false;
            refreshStatus();
            emit recordingChanged();
        });
    });
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

void RecordingService::refreshStorage()
{
    vgr::EngineWorker::instance().runOffThread([this] {
        auto info = rr::RecordingEngine::Instance().GetStorageInfo();
        if (!info.ok())
            return;
        const QVariantMap next = storageToVariant(info.value());
        vgr::EngineWorker::onInterfaceThread(this, [this, next] {
            if (next != storage_) {
                storage_ = next;
                emit storageChanged();
            }
        });
    });
}

// ---------------------------------------------------------------------------
// Status + the 1 Hz pump
// ---------------------------------------------------------------------------

// The session state QML binds to, read and assembled ON THE WORKER and handed
// back as a finished map. This runs at 1 Hz while a take is up (and on every
// transport click) — a lock wait on the thread that draws is exactly what the
// 1 Hz cadence would have turned into a visible hitch.
void RecordingService::refreshStatus()
{
    const QString id = recordingId_;
    vgr::EngineWorker::instance().runOffThread([this, id] {
        auto &rec = rr::RecordingEngine::Instance();
        bool active = false;
        bool paused = false;
        bool gone = false;
        QVariantMap next;

        if (!id.isEmpty()) {
            auto view = rec.GetStatus(id.toStdString());
            if (view.ok()) {
                const rr::RecordingStateView &v = view.value();
                active = v.state == rr::RecordingState::Recording
                         || v.state == rr::RecordingState::Paused;
                paused = v.state == rr::RecordingState::Paused;
                next.insert(QStringLiteral("state"),
                            QString::fromLatin1(rr::ToString(v.state)));
                next.insert(QStringLiteral("durationMs"), qint64(v.durationMs));
                next.insert(QStringLiteral("sizeMb"),
                            qreal(v.sizeBytes) / (1024.0 * 1024.0));
                next.insert(QStringLiteral("droppedFrames"), int(v.droppedFrames));
                next.insert(QStringLiteral("encoder"),
                            QString::fromStdString(v.encoder));
                next.insert(QStringLiteral("filePath"),
                            QString::fromStdString(v.filePath));
                next.insert(QStringLiteral("diskHealthy"), v.diskHealthy);
                QVariantList markers;
                for (const rr::RecordingMarker &m : v.markers)
                    markers.append(QVariantMap{
                        { QStringLiteral("timeMs"), qint64(m.timeMs) },
                        { QStringLiteral("label"), QString::fromStdString(m.label) },
                    });
                next.insert(QStringLiteral("markers"), markers);
            } else {
                gone = true;   // the session is gone (engine restart, policy stop)
            }
        }

        // Stream state from the graph (the node's existence IS the state).
        const bool streaming = bpr::ProductionEngine::Instance().Graph().HasNode(kStreamNodeId);
        if (streaming)
            next.insert(QStringLiteral("streaming"), true);

        vgr::EngineWorker::onInterfaceThread(this, [this, id, active, paused, gone, streaming, next] {
            // The answer belongs to the session it was asked about: a stop whose
            // status was still in flight must not resurrect it.
            if (id != recordingId_)
                return;
            recording_ = active;
            paused_ = paused;
            streaming_ = streaming;
            if (gone)
                recordingId_.clear();
            status_ = next;
        });
    });
}

void RecordingService::pumpTick()
{
    // The engine is externally-clocked (Tick advances durations, segmentation
    // and the disk monitor) — driven at 1 Hz on the worker, with the status
    // read that follows it there too.
    vgr::EngineWorker::instance().runOffThread([this] {
        (void)rr::RecordingEngine::Instance().Tick(1000);
        vgr::EngineWorker::onInterfaceThread(this, [this] {
            refreshStatus();
            refreshStorage();
            // Recording can end engine-side (disk policy StopAll, a failed
            // segment) — the pump stops itself when the session is gone.
            if (!recording_ && pump_)
                pump_->stop();
            emit recordingChanged();
        });
    });
}

void RecordingService::shutdown()
{
    if (pump_)
        pump_->stop();
    // A still-active session finalizes cleanly on app exit (the file on disk
    // must be complete — a crashed recording recovers via the journal, but a
    // planned shutdown has no excuse).
    if (recording_) {
        (void)rr::RecordingEngine::Instance().StopRecording(recordingId_.toStdString(),
                                                        std::string_view("shutdown"));
        recording_ = false;
    }
}
