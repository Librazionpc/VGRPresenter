#include "services/RoutingService.h"

#include "models/BusListModel.h"
#include "models/OutputListModel.h"
#include "models/PresetListModel.h"
#include "services/EngineWorker.h"

#include "modules/audio/AudioEngine.hpp"
#include "modules/production/ProductionEngine.hpp"
#include "modules/rendering/RenderEngine.hpp"

#include <QAbstractItemModel>
#include <QDebug>
#include <QModelIndex>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <cstdint>

//   1. The WORKER never reads a model. QAbstractListModel belongs to the
//      interface thread, so every job below carries a snapshot (RowSpec) built
//      on that thread, and the only thing that crosses back is plain data.
//   2. The INTERFACE thread never reads the graph. busRowForOutput() answers
//      from the published snapshot, so drawing a wire set cannot race the
//      worker filling it, and no QML binding can block on the engine.

namespace {
constexpr const char *kSpeakersNodeId = "out:speakers";
// Every node this service owns starts here ("out:<stable row id>").
constexpr const char *kNodePrefix = "out:";

bool IsScreenKind(const QString &kind)
{
    return kind.compare(QLatin1String("HDMI"), Qt::CaseInsensitive) == 0;
}

bool IsSystemKind(const QString &kind)
{
    return kind.compare(QLatin1String("SYSTEM"), Qt::CaseInsensitive) == 0;
}

bool IsStreamKind(const QString &kind)
{
    return kind.compare(QLatin1String("STREAM"), Qt::CaseInsensitive) == 0;
}

bps::production::OutputConfig CapsFor(const QString &kind)
{
    bps::production::OutputConfig cfg;
    if (IsSystemKind(kind)) {
        cfg.caps.supportsVideo = false;
        cfg.caps.supportsAudio = true;
        cfg.caps.supportsSeparateAudio = true;
        return cfg;
    }
    cfg.caps.supportsVideo = true;
    cfg.caps.supportsAudio = true;
    cfg.caps.supportsSeparateAudio = true;
    return cfg;
}
} // namespace

QPointer<RoutingService> RoutingService::s_instance;

RoutingService *RoutingService::instance()
{
    if (!s_instance)
        s_instance = new RoutingService(nullptr);
    return s_instance;
}

RoutingService *RoutingService::create(QQmlEngine *, QJSEngine *)
{
    // The QML engine builds the singleton itself; this is the C++ door to the
    // SAME object (see the header's note on the two-object trap).
    // engine-owned: instance() `new`s it (never a static), so the engine deleting it at teardown is correct; s_instance is a QPointer.
    return instance();
}

void RoutingService::adoptEngineInstance(RoutingService *service)
{
    if (service && !s_instance)
        s_instance = service;
}

RoutingService::RoutingService(QObject *parent)
    : QObject(parent)
{
    if (!s_instance)
        s_instance = this;

    auto *outputs = OutputListModel::instance();
    if (outputs) {
        // Structure: a row appeared, vanished or the roster was reloaded — the
        // graph needs a node created or pruned, which is a worker round trip.
        connect(outputs, &QAbstractItemModel::rowsInserted, this, [this] { refresh(); });
        connect(outputs, &QAbstractItemModel::rowsRemoved, this, [this] { refresh(); });
        connect(outputs, &QAbstractItemModel::modelReset, this, [this] { refresh(); });
        connect(outputs, &QAbstractItemModel::dataChanged, this, [this] { rebuildRows(); });
        connect(outputs, &OutputListModel::outputRosterChanged, this, [this] { refresh(); });
    }

    if (auto *buses = BusListModel::Instance()) {
        // Buses move rows when one is added or removed, and an assignment names
        // a bus by its NUMBER — so the row translation has to be redone.
        connect(buses, &QAbstractItemModel::rowsInserted, this, [this] { rebuildRows(); });
        connect(buses, &QAbstractItemModel::rowsRemoved, this, [this] { rebuildRows(); });
        connect(buses, &QAbstractItemModel::modelReset, this, [this] { rebuildRows(); });
        connect(buses, &QAbstractItemModel::dataChanged, this, [this] { rebuildRows(); });
    }

    // Deferred once: the singleton is built while QML is still loading, and the
    // graph commands below belong to the engine worker.
    QTimer::singleShot(0, this, [this] { refresh(); });

    // 1.5 s window — see kFeedLiveWindowMs).
    feedTimer_.setInterval(500);
    connect(&feedTimer_, &QTimer::timeout, this, &RoutingService::refreshFeedHealth);
    feedTimer_.start();
}

RoutingService::~RoutingService()
{
    if (s_instance == this)
        s_instance = nullptr;
}

// ---------------------------------------------------------------------------
// Node ids
// ---------------------------------------------------------------------------

QString RoutingService::nodeIdFor(const QString &stableId, const QString &kind)
{
    Q_UNUSED(kind)
    if (stableId.isEmpty())
        return {};
    return QStringLiteral("out:%1").arg(stableId);
}

QString RoutingService::busNodeFor(int busRow, bool audio)
{
    const int number = BusListModel::busNumberAt(busRow);
    if (number < 0)
        return {};
    return QStringLiteral("bus:%1#%2").arg(number).arg(audio ? QLatin1Char('a') : QLatin1Char('v'));
}

int RoutingService::busRowFromNode(const QString &nodeId)
{
    if (!nodeId.startsWith(QLatin1String("bus:")))
        return -1;
    const int hash = nodeId.indexOf(QLatin1Char('#'));
    if (hash < 0)
        return -1;
    bool ok = false;
    const int number = nodeId.mid(4, hash - 4).toInt(&ok);
    if (!ok)
        return -1;
    // A bus that was DELETED reads as unwired here even though the assignment
    // may still name it; refresh() clears those dangling ids in the graph.
    return BusListModel::rowForBusNumber(number);
}

constexpr uint32_t kNoAudioDevice = 0xFFFFFFFFu;
uint32_t AudioDeviceNumber(const QString &id)
{
    static const QString prefix = QStringLiteral("waveout:");
    if (!id.startsWith(prefix, Qt::CaseInsensitive))
        return kNoAudioDevice;
    bool ok = false;
    const uint32_t number = id.mid(prefix.size()).toUInt(&ok);
    return ok ? number : kNoAudioDevice;
}

// ---------------------------------------------------------------------------
// Reads (interface thread, published snapshot)
// ---------------------------------------------------------------------------

QVector<RoutingService::RowSpec> RoutingService::rowSpecs() const
{
    QVector<RowSpec> specs;
    auto *outputs = OutputListModel::instance();
    if (!outputs)
        return specs;

    const int rows = outputs->rowCount();
    specs.reserve(rows);
    for (int r = 0; r < rows; ++r) {
        const QModelIndex index = outputs->index(r);
        RowSpec spec;
        spec.row = r;
        spec.kind = outputs->data(index, OutputListModel::KindRole).toString();
        spec.name = outputs->data(index, OutputListModel::NameRole).toString();
        spec.enabled = outputs->data(index, OutputListModel::EnabledRole).toBool();
        spec.screen = IsScreenKind(spec.kind);
        // WHERE THIS ROW'S AUDIO PLAYS (Settings · Outputs' picker). Snapshotted
        // here because the worker must not read a QAbstractListModel.
        spec.audioDevice = outputs->data(index, OutputListModel::AudioDeviceRole).toString();
        spec.nodeId = nodeIdFor(outputs->data(index, OutputListModel::IdRole).toString(),
                                spec.kind);
        // A STREAM row's destination comes from its PRESET (url + "/" + key), read
        // here from the kernel store because the worker must not read a model.
        if (IsStreamKind(spec.kind) || spec.kind.compare(QLatin1String("REC"), Qt::CaseInsensitive) == 0)
            spec.recordBuffer = outputs->frameBufferFor(r);
        if (IsStreamKind(spec.kind)) {
            const QVariantMap preset = PresetListModel::lookup(
                outputs->data(index, OutputListModel::PresetIdRole).toString());
            if (preset.value(QStringLiteral("kind")).toString() == QLatin1String("streaming")) {
                const QVariantMap cfg = preset.value(QStringLiteral("config")).toMap();
                const QString url = cfg.value(QStringLiteral("serverUrl")).toString().trimmed();
                const QString key = cfg.value(QStringLiteral("streamKey")).toString().trimmed();
                if (!url.isEmpty() && !key.isEmpty())
                    spec.networkTarget = url.endsWith(QLatin1Char('/')) ? url + key
                                                                         : url + QLatin1Char('/') + key;
                spec.hardwareEncoder = cfg.value(QStringLiteral("encoder")).toString()
                                       != QLatin1String("Software");
            }
        }
        specs.append(spec);
    }
    return specs;
}

void RoutingService::rebuildRows()
{
    QVariantList list;
    auto *outputs = OutputListModel::instance();

    // Which roster row owns each output node: a sink's screen feeds are node ids
    // in the graph, and the canvas draws by ROW, so they are translated here.
    QHash<QString, int> rowOfNode;
    if (outputs) {
        for (int r = 0; r < outputs->rowCount(); ++r) {
            const QModelIndex index = outputs->index(r);
            rowOfNode.insert(nodeIdFor(outputs->data(index, OutputListModel::IdRole).toString(),
                                       outputs->data(index, OutputListModel::KindRole).toString()),
                             r);
        }
    }

    if (outputs) {
        const int rows = outputs->rowCount();
        for (int r = 0; r < rows; ++r) {
            const QModelIndex index = outputs->index(r);
            const QString kind = outputs->data(index, OutputListModel::KindRole).toString();
            const QString node = nodeIdFor(
                outputs->data(index, OutputListModel::IdRole).toString(), kind);
            const QPair<QString, QString> assignment = assignments_.value(node);
            const bool screen = IsScreenKind(kind);
            const QVector<int> videoSet = videoBuses_.value(node);
            const QVector<int> audioSet = audioBuses_.value(node);
            const int shownVideo = busRowFromNode(assignment.first);
            const int firstAudio = busRowFromNode(assignment.second);
            int videoBus = -1;
            if (videoSet.contains(shownVideo))
                videoBus = shownVideo;
            else if (!videoSet.isEmpty())
                videoBus = videoSet.first();
            int audioBus = -1;
            if (audioSet.contains(firstAudio))
                audioBus = firstAudio;
            else if (!audioSet.isEmpty())
                audioBus = audioSet.first();

            QVariantMap entry;
            entry.insert(QStringLiteral("row"), r);
            entry.insert(QStringLiteral("id"),
                         outputs->data(index, OutputListModel::IdRole).toString());
            entry.insert(QStringLiteral("name"),
                         outputs->data(index, OutputListModel::NameRole).toString());
            entry.insert(QStringLiteral("kind"), kind);
            // The recording / streaming preset a REC / STREAM row uses ("" = none).
            entry.insert(QStringLiteral("presetId"),
                         outputs->data(index, OutputListModel::PresetIdRole).toString());
            entry.insert(QStringLiteral("res"), outputs->data(index, OutputListModel::ResRole).toString());
            entry.insert(QStringLiteral("styleId"),
                         outputs->data(index, OutputListModel::StyleIdRole).toString());
            entry.insert(QStringLiteral("enabled"),
                         outputs->data(index, OutputListModel::EnabledRole).toBool());
            entry.insert(QStringLiteral("screen"), screen);
            entry.insert(QStringLiteral("system"), false);
            const auto caps = CapsFor(kind).caps;
            entry.insert(QStringLiteral("acceptsVideo"), caps.supportsVideo);
            entry.insert(QStringLiteral("acceptsAudio"), caps.supportsAudio);
            entry.insert(QStringLiteral("videoBus"), videoBus);
            // The whole wired sets, so a card can draw every wire it holds and
            // the canvas can answer "is this bus allowed on this output?".
            QVariantList videoList;
            for (const int busRow : videoSet)
                videoList.append(busRow);
            QVariantList audioList;
            for (const int busRow : audioSet)
                audioList.append(busRow);
            entry.insert(QStringLiteral("videoBuses"), videoList);
            entry.insert(QStringLiteral("audioBus"), audioBus);
            // "direct" | "compose": which picture this row shows (see OutputListModel).
            entry.insert(QStringLiteral("videoMode"),
                         outputs->data(index, OutputListModel::VideoModeRole).toString());
            entry.insert(QStringLiteral("showOverlay"),
                         outputs->data(index, OutputListModel::OverlayGateRole).toBool());
            entry.insert(QStringLiteral("showMedia"),
                         outputs->data(index, OutputListModel::MediaGateRole).toBool());
            entry.insert(QStringLiteral("audioBuses"), audioList);
            QVariantList feeds;
            for (const QString &screenNode : screenFeeds_.value(node)) {
                const int screenRow = rowOfNode.value(screenNode, -1);
                if (screenRow >= 0)
                    feeds.append(screenRow);
            }
            entry.insert(QStringLiteral("screenFeeds"), feeds);
            entry.insert(QStringLiteral("wired"), !videoSet.isEmpty() || !audioSet.isEmpty());
            // FEED HEALTH (see refreshFeedHealth): what the engine delivered, so
            // "wired" and "live" stop being the same word on screen.
            const FeedSample sample = feed_.value(r);
            entry.insert(QStringLiteral("feedLive"), sample.live);
            entry.insert(QStringLiteral("feedAgeMs"), sample.ageMs);
            entry.insert(QStringLiteral("feedFrames"), sample.frames);
            list.append(entry);
        }
    }

    QVariantList systemAudioBuses;
    const QVector<int> speakerSet = audioBuses_.value(QString::fromLatin1(kSpeakersNodeId));
    for (const int busRow : speakerSet)
        systemAudioBuses.append(busRow);
    const int systemAudioBus = speakerSet.isEmpty() ? -1 : speakerSet.first();
    QVariantMap system;
    system.insert(QStringLiteral("row"), -1);
    system.insert(QStringLiteral("id"), QStringLiteral("system"));
    system.insert(QStringLiteral("name"), QStringLiteral("Speakers"));
    system.insert(QStringLiteral("kind"), QStringLiteral("SYSTEM"));
    system.insert(QStringLiteral("res"), QString());
    system.insert(QStringLiteral("styleId"), QString());
    system.insert(QStringLiteral("enabled"), speakersEnabled_);
    system.insert(QStringLiteral("screen"), false);
    system.insert(QStringLiteral("system"), true);
    system.insert(QStringLiteral("acceptsVideo"), false);
    system.insert(QStringLiteral("acceptsAudio"), true);
    system.insert(QStringLiteral("videoBus"), -1);
    system.insert(QStringLiteral("audioBus"), systemAudioBus);
    system.insert(QStringLiteral("audioBuses"), systemAudioBuses);
    system.insert(QStringLiteral("screenFeeds"), QVariantList());
    system.insert(QStringLiteral("wired"), systemAudioBus >= 0);
    // The speaker endpoint has no frame of its own: nothing to report, and
    // nothing should read it as a stalled output.
    system.insert(QStringLiteral("feedLive"), false);
    system.insert(QStringLiteral("feedAgeMs"), QVariant::fromValue<qint64>(-1));
    system.insert(QStringLiteral("feedFrames"), QVariant::fromValue<qint64>(0));
    list.append(system);

    rows_ = list;
    emit rowsChanged();
}

void RoutingService::reportRefused(const QString &reason)
{
    qInfo("RoutingService: refused — %s", qUtf8Printable(reason));
    vgr::EngineWorker::onInterfaceThread(this, [this, reason] { emit refused(reason); });
}

void RoutingService::refreshFeedHealth()
{
    auto *outputs = OutputListModel::instance();
    if (!outputs || outputs->rowCount() == 0)
        return;

    QHash<QString, int> rowByBuffer;
    for (int r = 0; r < outputs->rowCount(); ++r) {
        const QString buffer = outputs->frameBufferFor(r);
        if (!buffer.isEmpty())
            rowByBuffer.insert(buffer, r);
    }
    if (rowByBuffer.isEmpty())
        return;

    vgr::EngineWorker::instance().runOffThread([this, rowByBuffer] {
        const auto ages = bps::rendering::RenderEngine::Instance().OutputFeedAges();
        QHash<int, FeedSample> found;
        for (const auto &row : ages) {
            const int r = rowByBuffer.value(QString::fromStdString(row.name), -1);
            if (r < 0)
                continue;
            FeedSample sample;
            sample.ageMs = static_cast<qint64>(row.ageMs);
            sample.frames = static_cast<qint64>(row.frames);
            sample.live = row.enabled && row.ageMs >= 0 && row.ageMs < kFeedLiveWindowMs;
            found.insert(r, sample);
        }
        vgr::EngineWorker::onInterfaceThread(this, [this, found] {
            if (feed_ == found)
                return;
            feed_ = found;
            // The published rows carry these, so the canvas repaints the moment
            // a feed starts or stops — no separate binding for QML to forget.
            rebuildRows();
        });
    });
}

void RoutingService::publishRoutedVideoBuses()
{
    auto *outputs = OutputListModel::instance();
    if (!outputs)
        return;
    const QVector<RowSpec> specs = rowSpecs();   // the model is this thread's
    QVector<int> byRow;
    QVector<QVector<int>> wiredByRow;
    byRow.reserve(specs.size());
    wiredByRow.reserve(specs.size());
    for (const RowSpec &spec : specs) {
        const QString video = assignments_.value(spec.nodeId).first;
        byRow.append(BusListModel::busNumberAt(busRowFromNode(video)));
        QVector<int> wired;
        for (const int busRow : videoBuses_.value(spec.nodeId)) {
            const int number = BusListModel::busNumberAt(busRow);
            if (number > 0)
                wired.append(number);
        }
        wiredByRow.append(wired);
    }
    outputs->setRoutedVideoBuses(byRow, wiredByRow);
}

int RoutingService::busRowForOutput(int outputRow, const QString &kind) const
{
    const bool audio = kind.compare(QLatin1String("audio"), Qt::CaseInsensitive) == 0;
    // The system row: its "row" is -1 everywhere (it has no roster row), so it
    // is answered from the speaker endpoint's published feed list.
    if (outputRow < 0) {
        const QVector<int> buses = audioBuses_.value(QString::fromLatin1(kSpeakersNodeId));
        return audio && !buses.isEmpty() ? buses.first() : -1;
    }

    auto *outputs = OutputListModel::instance();
    if (!outputs || outputRow >= outputs->rowCount())
        return -1;

    const QModelIndex index = outputs->index(outputRow);
    const QString node = nodeIdFor(outputs->data(index, OutputListModel::IdRole).toString(),
                                   outputs->data(index, OutputListModel::KindRole).toString());
    if (node.isEmpty())
        return -1;

    const QPair<QString, QString> assignment = assignments_.value(node);
    return busRowFromNode(audio ? assignment.second : assignment.first);
}

bool RoutingService::acceptsSignal(int outputRow, const QString &kind) const
{
    const QString k = kind.trimmed().toLower();
    if (k != QLatin1String("video") && k != QLatin1String("audio"))
        return false;
    if (outputRow < 0)
        return k == QLatin1String("audio");

    auto *outputs = OutputListModel::instance();
    if (!outputs || outputRow >= outputs->rowCount())
        return false;

    const QString rowKind =
        outputs->data(outputs->index(outputRow), OutputListModel::KindRole).toString();
    if (IsSystemKind(rowKind))
        return k == QLatin1String("audio");
    const auto caps = CapsFor(rowKind).caps;
    return k == QLatin1String("video") ? caps.supportsVideo : caps.supportsAudio;
}

// ---------------------------------------------------------------------------
// Writes (engine worker)
// ---------------------------------------------------------------------------

void RoutingService::refresh()
{
    auto *outputs = OutputListModel::instance();
    if (!outputs || !outputs->rosterReady()) {
        if (!warnedRosterNotReady_) {
            warnedRosterNotReady_ = true;
            qWarning("RoutingService: the output roster has not been read yet — holding off on the graph "
                     "mirror (pruning now would delete every output node the graph restored)");
        }
        return;
    }
    const QVector<RowSpec> specs = rowSpecs();   // the model is this thread's
    const bool fallbackSpeakerEnabled = speakersEnabled_;

    vgr::EngineWorker::instance().runOffThread([this, specs, fallbackSpeakerEnabled] {
        using namespace bps::production;
        QHash<QString, QPair<QString, QString>> found;
        auto &graph = ProductionEngine::Instance().Graph();

        QStringList wanted;
        wanted.reserve(specs.size());
        for (const RowSpec &spec : specs)
            if (!spec.nodeId.isEmpty())
                wanted.append(spec.nodeId);

        // 1. A row that is gone takes its node with it. Detaching first is what
        //    stops a deleted output's node from keeping its bus assignment
        //    (and drawing an edge to a bus nothing on screen is behind).
        QStringList stale;
        for (const std::string &id : graph.OutputIds()) {
            const QString node = QString::fromStdString(id);
            if (!node.startsWith(QLatin1String(kNodePrefix)))
                continue;
            if (node == QLatin1String(kSpeakersNodeId))
                continue;   // AudioEngine's node — created with the caps the mixer needs
            if (!wanted.contains(node))
                stale.append(node);
        }
        for (const QString &node : stale) {
            bps::audio::AudioEngine::Instance().StopOutputAudio(node.toStdString());
            (void)graph.RemoveNode(node.toStdString());
        }
        if (!stale.isEmpty())
            qWarning("RoutingService: pruned %d output node(s) no roster row owns: %s",
                     static_cast<int>(stale.size()), qUtf8Printable(stale.join(QStringLiteral(", "))));

        // 2. Every row owns one node, named like its row.
        for (const RowSpec &spec : specs) {
            if (spec.nodeId.isEmpty())
                continue;
            const std::string nodeId = spec.nodeId.toStdString();
            const bool stream = IsStreamKind(spec.kind);

            if (!graph.HasNode(nodeId)) {
                if (!graph.AddOutput(nodeId, spec.name.toStdString(), SignalType::Video,
                                     CapsFor(spec.kind))
                         .ok())
                    continue;
            } else {
                auto node = graph.GetNode(nodeId);
                if (node.ok() && QString::fromStdString(node.value().displayName) != spec.name)
                    (void)graph.RenameNode(nodeId, spec.name.toStdString());
            }
            if (const auto node = graph.GetNode(nodeId); node.ok()
                && node.value().enabled != spec.enabled)
                (void)graph.SetEnabled(nodeId, spec.enabled);

            if (stream) {
                const bool hadTarget = [&] {
                    const auto before = graph.GetOutputConfig(nodeId);
                    return before.ok() && before.value().networkTarget == spec.networkTarget.toStdString();
                }();
                (void)graph.SetOutputNetworkTarget(nodeId, spec.networkTarget.toStdString());   // no-op when unchanged
                if (!hadTarget) {
                    // The server only — the key is the secret half of the target.
                    const QString server = spec.networkTarget.left(
                        spec.networkTarget.lastIndexOf(QLatin1Char('/')) > 0
                            ? spec.networkTarget.lastIndexOf(QLatin1Char('/')) : 0);
                    qInfo("RoutingService: STREAM '%s' now targets %s", qUtf8Printable(spec.name),
                          server.isEmpty() ? "(nothing — no preset or no key yet)" : qUtf8Printable(server));
                }
                const std::string encoder = spec.hardwareEncoder ? "hardware" : "software";
                if (auto current = graph.GetOutputConfig(nodeId); current.ok()) {
                    // Only on a real change: each setter queues a save of the whole graph.
                    if (current.value().encoder != encoder)
                        (void)graph.SetOutputEncoder(nodeId, encoder);
                    if (current.value().group != "broadcast")
                        (void)graph.SetOutputGroup(nodeId, "broadcast");
                }
            }

            const std::string wantLocal =
                (spec.screen && spec.audioDevice.isEmpty()) ? "1" : "0";
            auto nodeInfo = graph.GetNode(nodeId);
            if (nodeInfo.ok()) {
                // WHAT A TAKE ON THIS NODE RECORDS: the row's own picture (see hasOwnPass), so the
                // style, buses and gates wired to this output decide the file's content.
                if (spec.kind.compare(QLatin1String("REC"), Qt::CaseInsensitive) == 0 || stream) {
                    const auto bufferMeta = nodeInfo.value().meta.find("recordBuffer");
                    const std::string want = spec.recordBuffer.toStdString();
                    if (bufferMeta == nodeInfo.value().meta.end() || bufferMeta->second != want) {
                        (void)graph.SetMeta(nodeId, "recordBuffer", want);
                        qInfo("RoutingService: '%s' now records its own picture (%s)", qUtf8Printable(spec.name),
                              want.empty() ? "none yet" : want.c_str());
                    }
                }
                const auto flag = nodeInfo.value().meta.find("playsLocally");
                if (flag == nodeInfo.value().meta.end() || flag->second != wantLocal)
                    (void)graph.SetMeta(nodeId, "playsLocally", wantLocal);
            }

            auto cfg = graph.GetOutputConfig(nodeId);
            if (!cfg.ok())
                continue;

            QString video = QString::fromStdString(cfg.value().videoBusId);
            QString audio = QString::fromStdString(cfg.value().audioBusId);

            // 3. A bus that was DELETED leaves a dangling id in the config. Cut
            //    it here, on the one path that already walks every output, so
            //    the graph can never validate against a bus that is not there.
            if (!video.isEmpty() && !graph.HasNode(video.toStdString())) {
                (void)graph.ClearOutputBusAssignment(nodeId, SignalType::Video);
                video.clear();
            }
            if (!audio.isEmpty() && !graph.HasNode(audio.toStdString())) {
                (void)graph.ClearOutputBusAssignment(nodeId, SignalType::Audio);
                audio.clear();
            }
            found.insert(spec.nodeId, qMakePair(video, audio));
        }

        // 4. WHAT IS WIRED, by side, for every output node — read from the EDGES,
        QHash<QString, QVector<int>> wiredVideo;
        QHash<QString, QVector<int>> wiredAudio;
        QStringList wiredNodes = found.keys();
        if (graph.HasNode(kSpeakersNodeId))
            wiredNodes.append(QString::fromLatin1(kSpeakersNodeId));
        for (const QString &node : wiredNodes) {
            QVector<int> videoRows;
            QVector<int> audioRows;
            for (const std::string &upstream : graph.Upstream(node.toStdString())) {
                const auto info = graph.GetNode(upstream);
                if (!info.ok() || info.value().kind != NodeKind::Bus)
                    continue;
                const int busRow = busRowFromNode(QString::fromStdString(upstream));
                if (busRow < 0)
                    continue;   // a bus the roster dropped reads as unwired
                if (info.value().signalType == SignalType::Audio)
                    audioRows.append(busRow);
                else if (info.value().signalType == SignalType::Video)
                    videoRows.append(busRow);
            }
            wiredVideo.insert(node, videoRows);
            wiredAudio.insert(node, audioRows);
        }

        for (auto it = found.begin(); it != found.end(); ++it) {
            const QString node = it.key();
            const QVector<int> videoRows = wiredVideo.value(node);
            const QVector<int> audioRows = wiredAudio.value(node);
            QString video = it.value().first;
            QString audio = it.value().second;

            const int shown = busRowFromNode(video);
            if (shown < 0 || !videoRows.contains(shown)) {
                video = videoRows.isEmpty() ? QString() : busNodeFor(videoRows.first(), false);
                if (video.isEmpty())
                    (void)graph.ClearOutputBusAssignment(node.toStdString(), SignalType::Video);
                else
                    (void)ProductionEngine::Instance().AssignOutputBuses(
                        node.toStdString(), video.toStdString(), std::string());
            }
            const int firstAudio = busRowFromNode(audio);
            if (firstAudio < 0 || !audioRows.contains(firstAudio)) {
                audio = audioRows.isEmpty() ? QString() : busNodeFor(audioRows.first(), true);
                if (audio.isEmpty())
                    (void)graph.ClearOutputBusAssignment(node.toStdString(), SignalType::Audio);
                else
                    (void)ProductionEngine::Instance().AssignOutputBuses(
                        node.toStdString(), std::string(), audio.toStdString());
            }
            if (it.value() != qMakePair(video, audio))
                it.value() = qMakePair(video, audio);
        }

        // The speaker endpoint's config, same rule: a single-bus reader must not be
        // told the speakers carry a bus somebody unplugged.
        if (graph.HasNode(kSpeakersNodeId)) {
            const QVector<int> audioRows = wiredAudio.value(QString::fromLatin1(kSpeakersNodeId));
            const QString first = audioRows.isEmpty() ? QString()
                                                      : busNodeFor(audioRows.first(), true);
            auto cfg = graph.GetOutputConfig(kSpeakersNodeId);
            if (cfg.ok() && QString::fromStdString(cfg.value().audioBusId) != first) {
                if (first.isEmpty())
                    (void)graph.ClearOutputBusAssignment(kSpeakersNodeId, SignalType::Audio);
                else
                    (void)ProductionEngine::Instance().AssignOutputBuses(
                        kSpeakersNodeId, std::string(), first.toStdString());
            }
        }

        bindOutputAudio(specs, wiredAudio);

        // 5. The screens whose picture feeds each sink. A screen is an Output node
        QSet<QString> screenNodes;
        for (const RowSpec &spec : specs)
            if (spec.screen && !spec.nodeId.isEmpty())
                screenNodes.insert(spec.nodeId);
        QHash<QString, QStringList> feeds;
        for (const RowSpec &spec : specs) {
            if (spec.nodeId.isEmpty() || spec.screen)
                continue;
            QStringList from;
            for (const std::string &upstream : graph.Upstream(spec.nodeId.toStdString())) {
                const QString node = QString::fromStdString(upstream);
                if (screenNodes.contains(node))
                    from.append(node);
            }
            if (!from.isEmpty())
                feeds.insert(spec.nodeId, from);
        }

        bool speakerEnabled = fallbackSpeakerEnabled;
        if (const auto speaker = graph.GetNode(kSpeakersNodeId); speaker.ok())
            speakerEnabled = speaker.value().enabled;

        vgr::EngineWorker::onInterfaceThread(this, [this, found, specs, wiredVideo, wiredAudio, feeds, speakerEnabled] {
            assignments_ = found;
            videoBuses_ = wiredVideo;
            audioBuses_ = wiredAudio;
            screenFeeds_ = feeds;
            speakersEnabled_ = speakerEnabled;
            rebuildRows();
            publishRoutedVideoBuses();
            ++revision_;
            emit routingChanged();
            if (!loggedFirstSync_) {
                loggedFirstSync_ = true;
                qInfo("RoutingService: mirrored %d output node(s) into the production graph",
                      static_cast<int>(specs.size()));
            }
        });
    });
}

void RoutingService::setSpeakersEnabled(bool enabled)
{
    if (speakersEnabled_ == enabled)
        return;
    speakersEnabled_ = enabled;
    rebuildRows();
    ++revision_;
    emit routingChanged();

    vgr::EngineWorker::instance().runOffThread([this, enabled] {
        auto &graph = bps::production::ProductionEngine::Instance().Graph();
        if (!graph.HasNode(kSpeakersNodeId)) {
            reportRefused(QStringLiteral("the speaker output is not ready yet"));
            vgr::EngineWorker::onInterfaceThread(this, [this, enabled] {
                speakersEnabled_ = !enabled;
                rebuildRows();
                ++revision_;
                emit routingChanged();
            });
            return;
        }
        const auto result = graph.SetEnabled(kSpeakersNodeId, enabled);
        if (!result.ok()) {
            reportRefused(QStringLiteral("could not change the speaker output state: %1")
                              .arg(QString::fromStdString(result.error().message)));
            vgr::EngineWorker::onInterfaceThread(this, [this, enabled] {
                speakersEnabled_ = !enabled;
                rebuildRows();
                ++revision_;
                emit routingChanged();
            });
        }
    });
}

void RoutingService::bindOutputAudio(const QVector<RowSpec> &specs,
                                     const QHash<QString, QVector<int>> &wiredAudio)
{
    auto &engine = bps::audio::AudioEngine::Instance();
    for (const RowSpec &spec : specs) {
        if (spec.nodeId.isEmpty())
            continue;
        const uint32_t device = AudioDeviceNumber(spec.audioDevice);
        const bool wanted = spec.enabled && device != kNoAudioDevice
                            && !wiredAudio.value(spec.nodeId).isEmpty();
        const std::string node = spec.nodeId.toStdString();
        if (!wanted) {
            engine.StopOutputAudio(node);
            // The row no longer has a broken stream to complain about.
            audioTrouble_.remove(spec.nodeId);
            continue;
        }
        const auto started = engine.StartOutputAudio(node, device);
        if (started.ok()) {
            audioTrouble_.remove(spec.nodeId);
            continue;
        }
        // ONE reading per row, not one per refresh: a device that is unplugged
        // would otherwise raise the same toast on every routing change.
        const QString reason = QString::fromStdString(started.error().message);
        if (audioTrouble_.value(spec.nodeId) == reason)
            continue;
        audioTrouble_.insert(spec.nodeId, reason);
        reportRefused(QStringLiteral("'%1' cannot play on %2: %3")
                          .arg(spec.name, spec.audioDevice, reason));
    }
}

bool RoutingService::assignBus(int outputRow, const QString &kind, int busRow)
{
    const QString k = kind.trimmed().toLower();
    if (k != QLatin1String("audio") && k != QLatin1String("video"))
        return false;
    if (!acceptsSignal(outputRow, k))
        return false;

    // outputRow < 0 is the speaker endpoint: no roster row, so its node id is the
    // engine's own. Everything below treats it like any other output.
    QString nodeId;
    if (outputRow < 0) {
        nodeId = QString::fromLatin1(kSpeakersNodeId);
    } else {
        auto *outputs = OutputListModel::instance();
        if (!outputs || outputRow >= outputs->rowCount())
            return false;
        const QModelIndex index = outputs->index(outputRow);
        nodeId = nodeIdFor(outputs->data(index, OutputListModel::IdRole).toString(),
                           outputs->data(index, OutputListModel::KindRole).toString());
    }
    if (nodeId.isEmpty())
        return false;

    const bool audio = (k == QLatin1String("audio"));
    const QString busNode = busRow >= 0 ? busNodeFor(busRow, audio) : QString();
    if (busRow >= 0 && busNode.isEmpty())
        return false;

    const bool speakers = (outputRow < 0);
    vgr::EngineWorker::instance().runOffThread([this, nodeId, audio, busNode, speakers] {
        using namespace bps::production;
        auto &graph = ProductionEngine::Instance().Graph();
        if (!graph.HasNode(nodeId.toStdString())) {
            // The mirror creates it on the next refresh — say so rather than
            // dropping the gesture, which read as "the wire is broken".
            reportRefused(QStringLiteral("the engine has not created this output's node yet — try again in a moment"));
            return;
        }

        if (speakers) {
            if (audio && !busNode.isEmpty()) {
                // A refusal here used to vanish (the result was dropped), which read as "the
                // speakers will not connect" with nothing to say why.
                const auto joined = graph.Connect(busNode.toStdString(), kSpeakersNodeId,
                                                  SignalType::Audio);
                if (!joined.ok())
                    reportRefused(QStringLiteral("cannot wire %1 into the speakers: %2")
                                      .arg(busNode, QString::fromStdString(joined.error().message)));
            }
            vgr::EngineWorker::onInterfaceThread(this, [this] {
                ++revision_;
                emit routingChanged();
                refresh();
            });
            return;
        }

        if (busNode.isEmpty()) {
            QVector<QString> dropping;
            for (const std::string &upstream : graph.Upstream(nodeId.toStdString())) {
                const auto info = graph.GetNode(upstream);
                if (!info.ok() || info.value().kind != NodeKind::Bus)
                    continue;
                if ((info.value().signalType == SignalType::Audio) == audio)
                    dropping.append(QString::fromStdString(upstream));
            }
            for (const QString &bus : dropping)
                (void)graph.Disconnect(bus.toStdString(), nodeId.toStdString());
            (void)graph.ClearOutputBusAssignment(nodeId.toStdString(),
                                                 audio ? SignalType::Audio : SignalType::Video);
        } else {
            auto cfg = graph.GetOutputConfig(nodeId.toStdString());
            if (!cfg.ok())
                return;
            QString video = QString::fromStdString(cfg.value().videoBusId);
            QString audioBus = QString::fromStdString(cfg.value().audioBusId);
            if ((audio ? audioBus : video).isEmpty()) {
                if (audio)
                    audioBus = busNode;
                else
                    video = busNode;
                const auto assigned = ProductionEngine::Instance().AssignOutputBuses(
                    nodeId.toStdString(), video.toStdString(), audioBus.toStdString());
                if (!assigned.ok()) {
                    // Refused by the graph itself (a bus of the wrong signal type, a
                    // sink that cannot carry it). Its message is the useful part.
                    reportRefused(QString::fromStdString(assigned.error().message));
                    return;
                }
            }

            const auto linked = graph.Connect(busNode.toStdString(), nodeId.toStdString(),
                                              audio ? SignalType::Audio : SignalType::Video);
            if (!linked.ok()) {
                reportRefused(QString::fromStdString(linked.error().message));
                return;
            }
        }

        auto after = graph.GetOutputConfig(nodeId.toStdString());
        if (!after.ok())
            return;
        const QString video = QString::fromStdString(after.value().videoBusId);
        const QString audioNow = QString::fromStdString(after.value().audioBusId);
        vgr::EngineWorker::onInterfaceThread(this, [this, nodeId, video, audioNow] {
            assignments_.insert(nodeId, qMakePair(video, audioNow));
            rebuildRows();
            publishRoutedVideoBuses();
            ++revision_;
            emit routingChanged();
            refresh();
        });
    });
    return true;
}

bool RoutingService::unwireBus(int outputRow, const QString &kind, int busRow)
{
    const QString k = kind.trimmed().toLower();
    if (k != QLatin1String("audio") && k != QLatin1String("video"))
        return false;
    if (busRow < 0)
        return false;
    if (!acceptsSignal(outputRow, k))
        return false;

    const bool audio = (k == QLatin1String("audio"));
    const QString busNode = busNodeFor(busRow, audio);
    if (busNode.isEmpty())
        return false;

    QString nodeId;
    if (outputRow < 0) {
        nodeId = QString::fromLatin1(kSpeakersNodeId);
    } else {
        auto *outputs = OutputListModel::instance();
        if (!outputs || outputRow >= outputs->rowCount())
            return false;
        const QModelIndex index = outputs->index(outputRow);
        nodeId = nodeIdFor(outputs->data(index, OutputListModel::IdRole).toString(),
                           outputs->data(index, OutputListModel::KindRole).toString());
    }
    if (nodeId.isEmpty())
        return false;

    vgr::EngineWorker::instance().runOffThread([this, nodeId, busNode, audio] {
        using namespace bps::production;
        auto &graph = ProductionEngine::Instance().Graph();
        if (!graph.HasNode(nodeId.toStdString()))
            return;
        (void)graph.Disconnect(busNode.toStdString(), nodeId.toStdString());

        auto cfg = graph.GetOutputConfig(nodeId.toStdString());
        if (cfg.ok()) {
            const QString named = audio ? QString::fromStdString(cfg.value().audioBusId)
                                        : QString::fromStdString(cfg.value().videoBusId);
            if (named == busNode) {
                QString replacement;   // another wired bus of the same side
                for (const std::string &upstream : graph.Upstream(nodeId.toStdString())) {
                    const auto info = graph.GetNode(upstream);
                    if (!info.ok() || info.value().kind != NodeKind::Bus)
                        continue;
                    if ((info.value().signalType == SignalType::Audio) != audio)
                        continue;
                    replacement = QString::fromStdString(upstream);
                    break;
                }
                if (replacement.isEmpty())
                    (void)graph.ClearOutputBusAssignment(
                        nodeId.toStdString(), audio ? SignalType::Audio : SignalType::Video);
                else
                    (void)ProductionEngine::Instance().AssignOutputBuses(
                        nodeId.toStdString(),
                        audio ? std::string() : replacement.toStdString(),
                        audio ? replacement.toStdString() : std::string());
            }
        }

        vgr::EngineWorker::onInterfaceThread(this, [this] {
            ++revision_;
            emit routingChanged();
            refresh();
        });
    });
    return true;
}

bool RoutingService::showBusOn(int outputRow, int busRow)
{
    if (outputRow < 0 || busRow < 0)
        return false;
    auto *outputs = OutputListModel::instance();
    if (!outputs || outputRow >= outputs->rowCount())
        return false;
    const QModelIndex index = outputs->index(outputRow);
    const QString kind = outputs->data(index, OutputListModel::KindRole).toString();
    const QString nodeId = nodeIdFor(outputs->data(index, OutputListModel::IdRole).toString(), kind);
    if (nodeId.isEmpty())
        return false;
    if (!CapsFor(kind).caps.supportsVideo)
        return false;

    const QString busNode = busNodeFor(busRow, false);
    if (busNode.isEmpty())
        return false;

    vgr::EngineWorker::instance().runOffThread([this, nodeId, busNode, busRow] {
        using namespace bps::production;
        auto &graph = ProductionEngine::Instance().Graph();
        if (!graph.HasNode(nodeId.toStdString())) {
            reportRefused(QStringLiteral("the engine has not created this output's node yet — try again in a moment"));
            return;
        }
        bool wired = false;
        for (const std::string &upstream : graph.Upstream(nodeId.toStdString())) {
            if (QString::fromStdString(upstream) == busNode) {
                wired = true;
                break;
            }
        }
        if (!wired) {
            reportRefused(QStringLiteral("bus %1 is not wired to this output — wire it first, then click it to show it")
                              .arg(busRow));
            return;
        }
        // The audio side is left alone (an empty id means "no change").
        const auto assigned = ProductionEngine::Instance().AssignOutputBuses(
            nodeId.toStdString(), busNode.toStdString(), std::string());
        if (!assigned.ok()) {
            reportRefused(QString::fromStdString(assigned.error().message));
            return;
        }
        auto after = graph.GetOutputConfig(nodeId.toStdString());
        const QString video = after.ok() ? QString::fromStdString(after.value().videoBusId) : busNode;
        const QString audio = after.ok() ? QString::fromStdString(after.value().audioBusId) : QString();
        vgr::EngineWorker::onInterfaceThread(this, [this, nodeId, video, audio] {
            assignments_.insert(nodeId, qMakePair(video, audio));
            rebuildRows();
            // The picture changes with the pick: published here, not at the next
            // refresh, so the frame after the click is already the chosen bus.
            publishRoutedVideoBuses();
            ++revision_;
            emit routingChanged();
        });
    });
    return true;
}

bool RoutingService::linkScreen(int screenRow, int sinkRow, bool linked)
{
    auto *outputs = OutputListModel::instance();
    if (!outputs || screenRow < 0 || sinkRow < 0
        || screenRow >= outputs->rowCount() || sinkRow >= outputs->rowCount())
        return false;

    const QModelIndex screenIndex = outputs->index(screenRow);
    const QModelIndex sinkIndex = outputs->index(sinkRow);
    const QString screenKind = outputs->data(screenIndex, OutputListModel::KindRole).toString();
    const QString sinkKind = outputs->data(sinkIndex, OutputListModel::KindRole).toString();
    // The picture leaves a SCREEN and lands on a sink that takes video. Another
    // screen, the speaker endpoint and a sink without a video side are refused.
    if (!IsScreenKind(screenKind) || IsScreenKind(sinkKind) || IsSystemKind(sinkKind))
        return false;
    if (!acceptsSignal(sinkRow, QStringLiteral("video")))
        return false;

    const QString screenNode = nodeIdFor(outputs->data(screenIndex, OutputListModel::IdRole).toString(),
                                         screenKind);
    const QString sinkNode = nodeIdFor(outputs->data(sinkIndex, OutputListModel::IdRole).toString(),
                                       sinkKind);
    if (screenNode.isEmpty() || sinkNode.isEmpty() || screenNode == sinkNode)
        return false;

    vgr::EngineWorker::instance().runOffThread([this, screenNode, sinkNode, linked] {
        using namespace bps::production;
        auto &graph = ProductionEngine::Instance().Graph();
        if (!graph.HasNode(screenNode.toStdString()) || !graph.HasNode(sinkNode.toStdString())) {
            reportRefused(QStringLiteral("one of these outputs has no engine node yet — try again in a moment"));
            return;
        }
        // The edge IS the feed: the sink's video is the screen's picture while it exists.
        if (linked)
            (void)graph.Connect(screenNode.toStdString(), sinkNode.toStdString(), SignalType::Video);
        else
            (void)graph.Disconnect(screenNode.toStdString(), sinkNode.toStdString());
        vgr::EngineWorker::onInterfaceThread(this, [this] {
            ++revision_;
            emit routingChanged();
            refresh();
        });
    });
    return true;
}