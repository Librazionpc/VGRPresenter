#include "services/RecordingService.h"

#include "models/OutputListModel.h"
#include "models/PresetListModel.h"
#include "services/EngineBridge.h"
#include "services/EngineWorker.h"
#include "services/EventBus.h"
#include "services/SettingsService.h"

#include "modules/production/ProductionEngine.hpp"
#include "modules/production/ProductionGraph.hpp"
#include "modules/recording/MediaFoundationEncode.hpp"
#include "modules/recording/RecordingEngine.hpp"
#include "modules/recording/RecordingTypes.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>

namespace rr = bps::recording;
namespace bpr = bps::production;

namespace {
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

std::string defaultRecordingDir(bool audioOnly = false)
{
    return bps::platform::PlatformAccessor::Get().Paths().UserDataDir() + "/recording/" +
           (audioOnly ? "audio" : "video");
}

// Settings-map → engine profile. The screen's free-text fields (resolution,
// frame rate) are parsed defensively — a malformed value keeps the default.
rr::RecordingProfile profileFromSettings(const QVariantMap &settings)
{
    rr::RecordingProfile p;
    p.name = QStringLiteral("service").toStdString();

    // AN AUDIO PRESET carries a "format" instead of a container + picture settings: sound
    // only, to a WAV, M4A (AAC) or MP3 file.
    const QString audioFormat = settings.value(QStringLiteral("format")).toString().toUpper();
    if (!audioFormat.isEmpty()) {
        p.container = audioFormat == QLatin1String("WAV")   ? rr::ContainerKind::WAV
                      : audioFormat == QLatin1String("MP3") ? rr::ContainerKind::MP3
                                                            : rr::ContainerKind::M4A;
        p.videoCodec = rr::VideoCodec::None;
        p.audioCodec = p.container == rr::ContainerKind::WAV ? rr::AudioCodec::PCM
                       : p.container == rr::ContainerKind::MP3 ? rr::AudioCodec::MP3
                                                               : rr::AudioCodec::AAC;
        const int bitrate = settings.value(QStringLiteral("audioBitrate")).toInt();
        p.audioBitrateKbps = bitrate > 0 ? bitrate : 192;
        p.audioChannels = 2;
        p.encoderPreference = rr::EncoderKind::Software;   // no picture, so no picture encoder to pick
        p.priority = rr::RecordingPriority::Critical;
        const QString folder = settings.value(QStringLiteral("outputFolder")).toString().trimmed();
        p.directory = folder.isEmpty() ? defaultRecordingDir(true) : folder.toStdString();
        return p;
    }

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
    // The preset's audio bitrate is real: the AAC encoder gets it (rounded to 96/128/160/192).
    const int aKbps = settings.value(QStringLiteral("audioBitrate")).toInt();
    p.audioBitrateKbps = aKbps > 0 ? aKbps : 192;
    p.audioChannels = 2;

    const QString enc = settings.value(QStringLiteral("encoder")).toString();
    // What the preset names is what the take gets: "Software" is the software encoder (not
    // "whatever is best"), and only a preset with no encoder at all means Auto.
    p.encoderPreference = enc == QLatin1String("NVENC")   ? rr::EncoderKind::NVENC
                          : enc == QLatin1String("AMF")   ? rr::EncoderKind::AMF
                          : enc == QLatin1String("QuickSync") ? rr::EncoderKind::QuickSync
                          : enc == QLatin1String("Software")  ? rr::EncoderKind::Software
                                                              : rr::EncoderKind::Auto;

    p.multitrack = false;
    p.quality = rr::QualityPreset::Broadcast;

    p.segmentDurationMs = 0;       // single file per session (MKV recommended)
    p.priority = rr::RecordingPriority::Critical;   // protect the service

    const QString folder = settings.value(QStringLiteral("outputFolder")).toString().trimmed();
    p.directory = folder.isEmpty()
        ? defaultRecordingDir()
        : folder.toStdString();

    return p;
}
} // namespace

RecordingService *RecordingService::s_instance = nullptr;

RecordingService::RecordingService(QObject *parent)
    : QObject(parent)
{
    s_instance = this;

    connect(&SettingsService::instance(), &SettingsService::changed,
            this, [this] {
                refreshStorage();
            });
    refreshStorage();
    // The engine registers its encoders / containers while it boots, which is after
    // this singleton exists: ask now, and again when boot completes.
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this,
            [this] { refreshCapabilities(); });
    refreshCapabilities();
}

void RecordingService::refreshCapabilities()
{
    QPointer<RecordingService> guard(this);
    QThreadPool::globalInstance()->start([this, guard] {
        auto &rec = rr::RecordingEngine::Instance();
        // The hardware encoders THIS machine has (NVENC, AMF, QuickSync) join the engine's list.
        (void)rr::RegisterHardwareEncoders(rec);
        QStringList encoders;
        for (const rr::EncoderKind kind : rec.DetectEncoders()) {
            switch (kind) {
            case rr::EncoderKind::NVENC: encoders << QStringLiteral("NVENC"); break;
            case rr::EncoderKind::AMF: encoders << QStringLiteral("AMF"); break;
            case rr::EncoderKind::QuickSync: encoders << QStringLiteral("QuickSync"); break;
            case rr::EncoderKind::Software: encoders << QStringLiteral("Software"); break;
            default: break;
            }
        }
        QStringList containers;
        for (const std::string &name : rec.ContainerNames())
            containers << QString::fromStdString(name).toUpper();
        const QString folder = QDir::toNativeSeparators(
            QDir::cleanPath(QString::fromStdString(defaultRecordingDir())));
        const QString audioFolder = QDir::toNativeSeparators(
            QDir::cleanPath(QString::fromStdString(defaultRecordingDir(true))));
        // Both exist from the start, so the operator can find where takes will land.
        QDir().mkpath(folder);
        QDir().mkpath(audioFolder);
        if (!guard)
            return;
        vgr::EngineWorker::onInterfaceThread(guard.data(), [this, encoders, containers, folder, audioFolder] {
            if (encoders == availableEncoders_ && containers == availableContainers_
                && folder == defaultFolder_ && audioFolder == defaultAudioFolder_)
                return;
            availableEncoders_ = encoders;
            availableContainers_ = containers;
            defaultFolder_ = folder;
            defaultAudioFolder_ = audioFolder;
            qInfo("RecordingService: this machine records with encoders [%s], containers [%s]",
                  qUtf8Printable(encoders.join(QStringLiteral(", "))),
                  qUtf8Printable(containers.join(QStringLiteral(", "))));
            emit capabilitiesChanged();
        });
    });
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
// Transport
// ---------------------------------------------------------------------------

bool RecordingService::startPrimaryRecording()
{
    auto *outputs = OutputListModel::instance();
    QString unpicked;   // the first Recording output that still has no preset
    for (int r = 0; outputs && r < outputs->rowCount(); ++r) {
        const QModelIndex index = outputs->index(r);
        if (outputs->data(index, OutputListModel::KindRole).toString().toUpper()
            != QLatin1String("REC"))
            continue;
        const QString name = outputs->data(index, OutputListModel::NameRole).toString();
        const QVariantMap preset = PresetListModel::lookup(
            outputs->data(index, OutputListModel::PresetIdRole).toString());
        const QString presetKind = preset.value(QStringLiteral("kind")).toString();
        if (presetKind != QLatin1String("recording") && presetKind != QLatin1String("audio")) {
            if (unpicked.isEmpty())
                unpicked = name;
            continue;
        }
        return startRecordingOn(
            QStringLiteral("out:%1").arg(outputs->data(index, OutputListModel::IdRole).toString()),
            preset.value(QStringLiteral("config")).toMap());
    }
    toast(unpicked.isEmpty()
              ? QStringLiteral("There is no Recording output yet — add one in Settings · Routing "
                               "(Outputs +), picking a recording preset made in Settings · Recording.")
              : QStringLiteral("The Recording output “%1” has no preset — pick one in "
                               "Settings · Routing (Configure output).").arg(unpicked),
          QStringLiteral("warning"));
    return false;
}

bool RecordingService::startRecordingOn(const QString &nodeId, const QVariantMap &settings)
{
    const QString node = nodeId.trimmed();
    if (node.isEmpty())
        return false;
    return beginRecording(settings, node);
}

bool RecordingService::beginRecording(const QVariantMap &settings, const QString &nodeId)
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
        [this, settings, profile, pid, stamp, nodeId] {
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

        const QString directory = QString::fromStdString(profile.directory);
        if (!QDir().mkpath(directory)) {
            refuse(QStringLiteral("the folder %1 cannot be created").arg(directory));
            return;
        }

        QString bus;
        {
            auto &graph = bpr::ProductionEngine::Instance().Graph();
            if (!graph.HasNode(nodeId.toStdString())) {
                vgr::EngineWorker::onInterfaceThread(this, [] {
                    toast(QStringLiteral("That output has no engine node yet — press Refresh on Routing, then start again."));
                });
                return;
            }
            bool wired = false;
            for (const std::string &upstream : graph.Upstream(nodeId.toStdString())) {
                const auto node = graph.GetNode(upstream);
                if (!node.ok() || node.value().kind != bpr::NodeKind::Bus ||
                    node.value().signalType != bpr::SignalType::Audio)
                    continue;
                wired = true;
                break;
            }
            if (!wired) {
                vgr::EngineWorker::onInterfaceThread(this, [] {
                    toast(QStringLiteral("Wire an audio bus to this output first — a recording with nothing wired would be silence."));
                });
                return;
            }
            bus = nodeId;
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
        vgr::EngineWorker::onInterfaceThread(this, [this, sessionId, busLabel, settings, nodeId] {
            recordingId_ = sessionId;
            recordingNode_ = nodeId;
            profileId_ = QStringLiteral("ui-program");
            if (pump_ && !pump_->isActive())
                pump_->start();
            refreshStatus();
            refreshStorage();
            qInfo("RecordingService: recording '%s' from %s (%s/%s, %dkbps)",
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
    recordingId_.clear();
    recordingNode_.clear();
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

QString RecordingService::pickFolder(const QString &title)
{
    auto picked = bps::platform::PlatformAccessor::Get().Dialogs().SelectFolderDialog(
        title.toStdString());
    if (!picked.ok() || !picked.value())
        return {};
    return QString::fromStdString(*picked.value());
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

        // Stream state from the graph: some output node holds a destination (a STREAM
        // row whose preset has a server + key — RoutingService writes it).
        bool streaming = false;
        {
            auto &graph = bpr::ProductionEngine::Instance().Graph();
            for (const std::string &outId : graph.OutputIds()) {
                const auto cfg = graph.GetOutputConfig(outId);
                if (cfg.ok() && !cfg.value().networkTarget.empty()) {
                    streaming = true;
                    break;
                }
            }
        }
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
            if (gone) {
                recordingId_.clear();
                recordingNode_.clear();   // the session ended engine-side
            }
            status_ = next;
        });
    });
}

void RecordingService::pumpTick()
{
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
    if (recording_) {
        (void)rr::RecordingEngine::Instance().StopRecording(recordingId_.toStdString(),
                                                        std::string_view("shutdown"));
        recording_ = false;
    }
}