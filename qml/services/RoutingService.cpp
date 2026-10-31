#include "services/RoutingService.h"

#include "models/BusListModel.h"
#include "models/OutputListModel.h"
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

// See the header for why this exists. Two invariants worth stating where the
// code is, because both were bugs waiting to happen:
//
//   1. The WORKER never reads a model. QAbstractListModel belongs to the
//      interface thread, so every job below carries a snapshot (RowSpec) built
//      on that thread, and the only thing that crosses back is plain data.
//   2. The INTERFACE thread never reads the graph. busRowForOutput() answers
//      from the published snapshot, so drawing a wire set cannot race the
//      worker filling it, and no QML binding can block on the engine.

namespace {

// RecordingService's own output node (its RTMP destination). It is rebuilt
// whenever the stream target changes, so this service never creates, renames or
// prunes it — it reads and assigns the node that already exists.
constexpr const char *kStreamNodeId = "out:stream";
// The engine's speaker endpoint — AudioMixer::kSpeakersOutputId, spelled here
// because the routing canvas draws it as the AUDIO sink row. This service does
// not create it (AudioEngine::EnsureSpeakersOutput owns that: it is made with
// exactly the caps the mixer needs) and never prunes it.
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

// What a row's output node can carry. EVERY transport carries both signals: a
// physical display (HDMI/projector) is a render target AND a sound destination,
// exactly like NDI, SDI, recording and streaming, so all of them take an audio bus
// beside the picture. The speaker endpoint is the mirror image — audio only; it
// has no picture to send.
bps::production::OutputConfig CapsFor(const QString &kind)
{
    bps::production::OutputConfig cfg;
    if (IsSystemKind(kind)) {
        cfg.caps.supportsVideo = false;
        cfg.caps.supportsAudio = true;
        cfg.caps.supportsSeparateAudio = true;
        return cfg;
    }
    // An OUTPUT is a transport, and a transport carries both signals: HDMI
    // carries sound, and so does every other row that sends a picture somewhere.
    // This table used to answer `!screen`, which is why an HDMI card had no audio
    // dot and an audio wire onto it was refused at the caps with nothing on screen
    // to aim at. A display with no speakers of its own still accepts the bus — its
    // mix is rendered and simply has no device to come out of, which is a delivery
    // fact, not a routing one.
    cfg.caps.supportsVideo = true;
    cfg.caps.supportsAudio = true;
    // "Separate audio" means the row's audio can differ from the picture it shows;
    // a display window still needs one audio bus of its own, so it is true for
    // every transport the canvas draws (the speaker endpoint sets it above).
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
        // Content: a rename, a style, an enable toggle. The published snapshot
        // is rebuilt here; a rename also reaches the graph's display name, so
        // the node and the row keep reading the same.
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

    // FEED HEALTH. The routing facts change when someone wires something; the
    // PICTURE changes every frame, so it needs its own heartbeat. 500 ms is fast
    // enough to watch a wire take effect and slow enough that the cards' "live /
    // no frames" is not a strobe (a 60 Hz output always reads live inside a
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
    if (IsStreamKind(kind))
        return QString::fromLatin1(kStreamNodeId);
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

// "waveout:<n>" → n; anything else ("" included) → kNoAudioDevice. The roster id
// is the audio layer's own naming, and a row whose saved endpoint is gone keeps
// the string but opens NOTHING — falling back to the default endpoint would play
// the sound somewhere the operator did not choose, which is the one thing the
// per-output choice exists to prevent.
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
        spec.screen = IsScreenKind(spec.kind);
        // WHERE THIS ROW'S AUDIO PLAYS (Settings · Outputs' picker). Snapshotted
        // here because the worker must not read a QAbstractListModel.
        spec.audioDevice = outputs->data(index, OutputListModel::AudioDeviceRole).toString();
        spec.nodeId = nodeIdFor(outputs->data(index, OutputListModel::IdRole).toString(),
                                spec.kind);
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
            // THE WIRED SETS, from the edges (see refresh): every bus each side
            // reaches. `videoBus` is which of the video ones ON AIR — the config's
            // own field, projected onto the set so a wire that was taken away can
            // never be named — and `audioBus` the first, because audio is summed
            // and has no "shown" bus.
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
            entry.insert(QStringLiteral("res"), outputs->data(index, OutputListModel::ResRole).toString());
            entry.insert(QStringLiteral("styleId"),
                         outputs->data(index, OutputListModel::StyleIdRole).toString());
            entry.insert(QStringLiteral("enabled"),
                         outputs->data(index, OutputListModel::EnabledRole).toBool());
            entry.insert(QStringLiteral("screen"), screen);
            entry.insert(QStringLiteral("system"), false);
            // How many dots this card gets comes from the TRANSPORT's own caps —
            // the same table its node is created with (CapsFor), not a guess made
            // here. An HDMI display carries a picture, so its card has ONE dot;
            // NDI, SDI, recording and streaming carry the audio bus beside the
            // picture, so their cards have TWO (video in and audio in).
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
            // THIS ROW's own Overlays / Media gates (see
            // OutputListModel::setShowOverlay): they gate THIS output's picture and
            // nobody else's, so its card is where they are read and toggled. Read
            // as roles — the model owns the state and the persistence.
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

    // The speaker endpoint. It is NOT a roster row — AudioEngine creates the node
    // (out:speakers) so the mixer has one place to sum the wired buses into — but
    // it is a real sink all the same: a bus is wired to it exactly like one is
    // wired to NDI, and "always on" was only ever true of the node's existence,
    // never of a bus reaching it. A bus added after boot used to stay silent
    // behind a card with no port, which is what this row now exposes.
    //
    // ONE difference matters: this sink is a SUM. AudioMixer::MixSelected sums
    // every plane whose edge reaches out:speakers, so the endpoint carries as
    // many buses as the operator wires — which is why the row publishes
    // `audioBuses` (a LIST) and not just `audioBus`. `audioBus` stays as the
    // first entry for any reader that wants one bus.
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
    system.insert(QStringLiteral("enabled"), true);
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

// WHAT THE ENGINE ACTUALLY DELIVERED, per output row.
//
// The canvas used to answer "is this output getting a picture?" from the wiring
// alone — wired meant live — which is exactly the reading that cannot be debugged:
// a wire whose bus never produced a frame, a buffer the loop never composed into,
// and a sender that is behind all looked identical on screen. The render engine
// already keeps the honest counters (each output's FramesReceived and the steady
// clock of its last Present), so the cards read those instead. Atomic reads, no
// device, no frame copy — safe to poll, and it never touches the model.
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

// The video wire, per roster row, from the published assignments — the one
// thing OutputListModel needs to give each output its own picture. A bus row
// that is gone (deleted, so busRowFromNode answers -1) publishes 0, which is
// "nothing wired": the row falls back to the program slide instead of holding a
// dangling bus number.
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
        // THE WHOLE WIRED SET, not just the one on air: an output may hold several
        // video buses, and this list is the allow-list the engine gates the
        // design's bus windows with (OutputStyleSpec::allowedVideoBuses). Bus
        // NUMBERS, because that is the key CompositorState's frames use.
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
    // The speaker endpoint takes audio and nothing else (its node is typed Audio,
    // so the graph would refuse a video edge anyway — say so here instead of
    // letting a wire vanish on release).
    if (outputRow < 0)
        return k == QLatin1String("audio");

    auto *outputs = OutputListModel::instance();
    if (!outputs || outputRow >= outputs->rowCount())
        return false;

    const QString rowKind =
        outputs->data(outputs->index(outputRow), OutputListModel::KindRole).toString();
    // The speaker endpoint is the one sink that takes audio ONLY. Every other
    // output — a display window (HDMI/projector) included — is a transport, and a
    // transport carries both: HDMI carries sound, and a display output that
    // refused an audio wire refused the very thing the transport is for. Answer
    // from the SAME caps table the engine's node was created with (CapsFor), so a
    // row cannot be draggable here and refused three calls down in the graph.
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
    // THE ROSTER HAS TO HAVE BEEN READ BEFORE ANYTHING IS PRUNED. OutputListModel
    // is built at QML load, BEFORE the engine opens kernel.json, so its first
    // hydrate always reads an EMPTY store — and an empty roster here means "no row
    // owns any output node", which the prune below would take literally and delete
    // every output node AND its wiring from the saved graph. (That is the boot race
    // behind "my outputs and their wires are gone after a restart": the nodes the
    // graph had restored were removed seconds after the restore, and the removal
    // was persisted.) A roster that is not ready yet means "ask again later": the
    // hydrate emits modelReset / outputRosterChanged, and both call this again.
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

    vgr::EngineWorker::instance().runOffThread([this, specs] {
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
            if (node == QLatin1String(kStreamNodeId))
                continue;   // RecordingService's node — never ours to remove
            if (node == QLatin1String(kSpeakersNodeId))
                continue;   // AudioEngine's node — created with the caps the mixer needs
            if (!wanted.contains(node))
                stale.append(node);
        }
        for (const QString &node : stale) {
            // A deleted row takes its own audio stream with it: the node is gone,
            // so nothing would ever stop that device's stream once it left the
            // roster (a closed row that keeps a WASAPI client open is a device
            // the rest of the machine can no longer use).
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
            const bool stream = spec.nodeId == QLatin1String(kStreamNodeId);

            if (stream) {
                // Nothing to create: without a stream target there is no node,
                // and the row reads as unwired until RecordingService makes one.
            } else if (!graph.HasNode(nodeId)) {
                if (!graph.AddOutput(nodeId, spec.name.toStdString(), SignalType::Video,
                                     CapsFor(spec.kind))
                         .ok())
                    continue;
            } else {
                auto node = graph.GetNode(nodeId);
                if (node.ok() && QString::fromStdString(node.value().displayName) != spec.name)
                    (void)graph.RenameNode(nodeId, spec.name.toStdString());
            }

            // Which outputs PLAY through the app's own audio stream. A display
            // transport does — an HDMI/projector row's monitor IS the machine's
            // render endpoint on a booth PC, so the bus wired to it must actually
            // sound. NDI, recording and streaming hand their audio to their own
            // sink (a mixer tap read by the sender or the encoder), so they stay
            // off the flag or every feed would also play on the speakers.
            //
            // Written as node META because the mixer reads the graph SNAPSHOT,
            // which carries meta and not the output config; written on every sync
            // so a node created by an older build picks the flag up without a
            // migration.
            // ...AND NEVER WHEN THE ROW PLAYS ON AN ENDPOINT OF ITS OWN: its mix
            // is then rendered by that device's own stream, and also summing it
            // into the program is the same sound twice, from two places.
            const std::string wantLocal =
                (spec.screen && spec.nodeId != QLatin1String(kStreamNodeId)
                 && spec.audioDevice.isEmpty()) ? "1" : "0";
            auto nodeInfo = graph.GetNode(nodeId);
            if (nodeInfo.ok()) {
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
        //    never from the config. A row carries several buses per side now (the
        //    wires are an allow-list, not a single assignment) and OutputConfig
        //    still holds one id per side, so the config alone can no longer answer
        //    "what does this output hold".
        //
        //    Edges are also what the ENGINE reads: AudioMixer sums every plane whose
        //    edge reaches a sink, so the wires ARE the mix. The speaker endpoint is
        //    in the same pass — it is AudioEngine's node, never created or pruned
        //    here, and a graph that has not started playout simply has none, in
        //    which case the Speakers card reads unwired and wiring it is a no-op
        //    until the engine is up, instead of this service inventing a second node
        //    with other caps (and, for the buses AudioEngine::EnsureSpeakersOutput
        //    connected at boot, reading none of them).
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

        // 4b. THE CONFIG NAMES A WIRED BUS. The video side keeps whichever bus is
        //     ON AIR for as long as that bus is still wired — unplugging the one
        //     being shown hands the output to the first wire still there, so a card
        //     can never keep showing a bus somebody took away. The audio side names
        //     the first wired bus, which is all a single-bus reader wants (the mixer
        //     sums the edges, so nothing is lost by the field holding one).
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

        // 4c. PLAY WHERE THE ROW SAYS. The picker on Settings · Outputs names an
        // endpoint per row; the WIRES decide whether there is anything to play
        // there. Both halves are read here, on the same pass that read them, so a
        // device choice and a wire can never disagree about whether a stream
        // should be open.
        bindOutputAudio(specs, wiredAudio);

        // 5. The screens whose picture feeds each sink. A screen is an Output node
        //    too, so its feed is an EDGE from the screen's node. Only the nodes
        //    this pass mirrored as screen rows count as screens, so a sink can
        //    never be read as one.
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

        vgr::EngineWorker::onInterfaceThread(this, [this, found, specs, wiredVideo, wiredAudio, feeds] {
            assignments_ = found;
            videoBuses_ = wiredVideo;
            audioBuses_ = wiredAudio;
            screenFeeds_ = feeds;
            rebuildRows();
            // THE ENGINE → THE PICTURE: what each output's video wire names is
            // now published to the model that builds the live output specs, so
            // a wired output composes its bus's feed and an unwired one keeps
            // the program slide.
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

// START/STOP THE PER-ROW STREAMS. Engine worker, from a snapshot of the roster.
//
// A row plays on its own endpoint when it HAS one and something is wired to it;
// every other row is stopped. "Stopped, not left running" is the important half:
// a stream on a row whose wire was pulled would keep a device open for a mix
// nobody wired, and on Windows an open WASAPI client is a device another app
// cannot take exclusive use of.
//
// The catalog of rows with an endpoint is deliberately NOT the speaker endpoint:
// that sink is the program's own sum, so it has no stream of its own to open.
void RoutingService::bindOutputAudio(const QVector<RowSpec> &specs,
                                     const QHash<QString, QVector<int>> &wiredAudio)
{
    auto &engine = bps::audio::AudioEngine::Instance();
    for (const RowSpec &spec : specs) {
        if (spec.nodeId.isEmpty())
            continue;
        const uint32_t device = AudioDeviceNumber(spec.audioDevice);
        const bool wanted = device != kNoAudioDevice && !wiredAudio.value(spec.nodeId).isEmpty();
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

        // The speaker endpoint is a SUM of edges, so wiring a bus onto it is one
        // edge and nothing else: the config holds a single bus and would have
        // thrown the previous one away. Unwiring names its bus (see unwireBus) —
        // "clear this side" has no meaning on a sink several buses feed.
        if (speakers) {
            if (audio && !busNode.isEmpty())
                (void)graph.Connect(busNode.toStdString(), kSpeakersNodeId, SignalType::Audio);
            vgr::EngineWorker::onInterfaceThread(this, [this] {
                ++revision_;
                emit routingChanged();
                refresh();
            });
            return;
        }

        if (busNode.isEmpty()) {
            // "Clear this side": every bus of that signal leaves the row, and BOTH
            // facts are cut — the EDGES (what the mixer and the renderer read) and
            // the config id that named one of them. One wire leaving is unwireBus's
            // job; this is the whole side, which is what the card menus offer.
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
            // ADDING A WIRE NEVER TAKES ONE AWAY. This row holds several buses per
            // side now, so the config (which holds ONE id per side) is written only
            // when that side has none yet — the new wire then becomes the one ON AIR
            // (video) / the first audio bus. An id that is already there stays, which
            // is what keeps the bus being shown on air while a second one is wired
            // beside it. AssignOutputBuses reads an empty id as "leave this side
            // alone", so only the side being created is passed.
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

            // THE EDGE — always, and only ever added. The config is the graph's own
            // validated field (persisted, what "which bus is on air" reads back);
            // the mixer and every mix tap keyed by an output node id read the EDGE
            // SET instead. Writing only the config is what made a wired audio bus
            // silent, so both are written here — and the second bus on a side is one
            // more edge, not a replacement for the first.
            //
            // An OUTPUT node takes ANY signal (ProductionGraph treats it as a sink;
            // what a row may carry is its caps, enforced by AssignOutputBuses and by
            // the Connect below), so both halves of a both-bus reach one node and the
            // sink's tap has something to sum.
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
            // A video wire just changed, so that output's PICTURE changes with
            // it — published here, not on the next refresh, so the frame that
            // follows the drop already shows the bus.
            publishRoutedVideoBuses();
            ++revision_;
            emit routingChanged();
            // The row's WIRED SETS come from the edges (a side holds several
            // buses), and the canvas draws every one of them, so a wire that only
            // reached `assignments_` would not appear as a wire at all until
            // something else triggered a mirror. This is exactly what happened on
            // the speaker endpoint, where the second bus drew nothing. Re-mirror on
            // the same gesture that changed the graph.
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

    // outputRow < 0 is the speaker endpoint: no roster row, so its node id is the
    // engine's own. Everything below treats it like any other output — one wire
    // leaving a sink is the same gesture whether the sink sums (speakers) or
    // shows (a picture).
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
        // The edge IS the wire: cut it and the mixer stops summing that bus and the
        // renderer stops naming it. (Disconnect errors when the edge is already
        // gone — a second click on a wire whose bus was deleted elsewhere — and that
        // is a no-op, not a failure worth reporting.)
        (void)graph.Disconnect(busNode.toStdString(), nodeId.toStdString());

        // If the config named the bus that just left, hand that field to a bus still
        // wired (or clear it). A row must never keep naming a bus it no longer
        // holds: that is a stale picture and a stale "on air" reading.
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

// SHOW ONE OF A ROW'S WIRED VIDEO BUSES.
//
// An output holds as many buses as are wired to it, but it sends ONE picture, so
// exactly one of them is on air. That choice is the config's videoBusId (which the
// renderer already reads through OutputListModel::setRoutedVideoBuses), and the
// WIRED SET is the allow-list: a bus that is not wired to this row cannot be shown
// here, and asking for one is refused out loud instead of silently showing the
// program slide or somebody else's feed.
//
// Audio has no equivalent: every wired audio bus is summed, so "which one is
// playing" is not a question with one answer.
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
        // THE ALLOW-LIST: the EDGE is the wire, so "is this bus wired here" is
        // answered by the graph itself rather than by the last snapshot this
        // service published. A gesture can land before a refresh, and the answer
        // must still be the engine's.
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
