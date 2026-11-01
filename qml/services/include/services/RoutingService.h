#pragma once

// RoutingService — the bridge that makes Settings · Routing's wires REAL.
//
// THE PROBLEM IT SOLVES. Settings · Outputs keeps its roster in OutputStore
// (rows: name, kind HDMI/NDI/SDI/REC/STREAM, style id, display binding), while
// the engine's production graph knows a different thing: OUTPUT NODES, each
// carrying an OutputConfig — one video bus + one audio bus (docs/specs/27
// §Outputs). Nothing joined the two. Only the live-stream node was ever created
// in the graph (the old shared "out:stream"), so every other output had no
// node, no bus pair, and no place for a routing wire to live. The Routing
// canvas could therefore only draw wires it remembered in QML, which is exactly
// what a wire must not be.
//
// WHAT IT DOES. It mirrors EVERY roster row as a graph output node —
// "out:<stable row id>" — and reads/writes that node's OutputConfig. A wire on
// the canvas is then an assignment the ENGINE stores, validates
// (ProductionGraph::AssignOutputBuses refuses a bus that is not a bus of that
// signal type, and refuses audio into a video-only output), and persists (the
// graph's autosave writes the whole production document, configs included).
// Nothing here keeps its own idea of the routing: the graph is the truth.
//
// THREADING. The graph is the engine's, so every read and write of it happens
// on the engine worker (vgr::EngineWorker) — the QML entry points below never
// touch it. What they DO touch is a published snapshot: `rows` is a plain list
// rebuilt on the interface thread from the roster plus the last published
// assignments, so a QML binding reads what this thread already owns. That is
// the EngineWorker "ANSWERS" shape, and it is also what keeps the interface
// thread out of a graph mutation's save flush (each assignment writes the
// production document).
//
// IDENTITY. A row's node id comes from its STABLE id (OutputListModel's
// `outputId`), never its name or its position: renaming an output must not cut
// its wires, and deleting one must not hand them to whatever takes its row.
// One node is not this service's to create: the SPEAKER row's "out:speakers", owned
// by AudioEngine (made with exactly the caps the mixer needs) — read and assigned,
// never mirrored. A STREAM row is mirrored like any other, and its node carries the
// RTMP target of the preset the row is wired to (PresetListModel).
//
// AN OUTPUT HOLDS SEVERAL BUSES PER SIDE. A wire is an EDGE, and an output's
// edges are a SET: drop three buses on one card and it carries three, on each
// side independently. OutputConfig still holds ONE id per side (it is the
// graph's own validated field and every single-bus reader uses it), so the two
// facts are split like this:
//
//   edges                       what is WIRED — the allow-list
//   OutputConfig.videoBusId     which of them is ON AIR (exactly one picture)
//   OutputConfig.audioBusId     the first wired bus, for single-bus readers
//
// That split is what makes "show this bus here" a question with an answer: a bus
// can only be shown on an output it is wired to (showBusOn checks the edge set and
// refuses anything else out loud). Nothing prunes a side down to one bus any more;
// unwiring names the bus it takes away (unwireBus).
//
// The speaker endpoint was the first sink to carry several (AudioMixer::MixSelected
// walks the EDGES, not the config); its row publishes a LIST (`audioBuses`) for the
// same reason every other row now does.

#include <QHash>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class RoutingService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // One entry per output row, plus the system output:
    //   { row, id, name, kind, styleId, enabled, screen,
    //     acceptsVideo, acceptsAudio, videoBus, videoBuses, audioBus, audioBuses,
    //     screenFeeds, wired, system }
    // `videoBus` / `audioBus` are BUS ROWS (-1 = unwired), already translated
    // from the engine's node ids, so the canvas draws a wire by comparing
    // numbers instead of parsing ids. `wired` is "something feeds this row".
    // `videoBuses` / `audioBuses` are the WIRED SETS — every bus of that signal
    // the row's edges reach, in the graph's order — because one side can hold
    // several now. `videoBus` is the one of them ON AIR (an output sends one
    // picture) and `audioBus` the first, for readers that only want one bus.
    // `screenFeeds` lists the ROSTER ROWS of the screens whose picture is sent
    // into this row (empty for a screen and for the speaker endpoint).
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    // Bumped after every published change (an assignment, a mirror, a roster
    // edit). The canvas depends on it so its wire geometry re-derives even when
    // a row's own fields did not change.
    Q_PROPERTY(int revision READ revision NOTIFY routingChanged)

public:
    static RoutingService *instance();
    static RoutingService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~RoutingService() override;

    QVariantList rows() const { return rows_; }
    int revision() const { return revision_; }

    // The bus ROW feeding `outputRow` for `kind` ("audio" | "video"); -1 when
    // nothing is wired. Answered from the published snapshot.
    Q_INVOKABLE int busRowForOutput(int outputRow, const QString &kind) const;

    // WIRE one bus onto one side of one row (busRow >= 0) — it ADDS to the side's
    // wired set rather than replacing it, so a row can hold several buses at once;
    // a bus already wired there is a no-op. busRow == -1 clears the whole side
    // (every bus of that signal leaves the row, edges and config both). false =
    // refused here and now (a bad kind, a row that is gone, a signal the row does
    // not accept); true = accepted and handed to the engine worker, whose verdict
    // arrives as routingChanged (the snapshot only changes if the engine took it).
    //
    // `outputRow` is -1 for the SPEAKER endpoint (the system row has no roster
    // row). That sink takes audio only, and -1 does nothing there — removing one
    // bus from any row needs unwireBus(), which names it.
    Q_INVOKABLE bool assignBus(int outputRow, const QString &kind, int busRow);

    // Remove ONE bus from one side of one row. Every row can carry several buses
    // per side now, so "clear this side" (assignBus(row, kind, -1)) is the wrong
    // verb for a single wire: this names the bus it takes away and leaves the
    // rest exactly as they were.
    Q_INVOKABLE bool unwireBus(int outputRow, const QString &kind, int busRow);

    // SHOW one of a row's wired video buses: several can be wired at once, but an
    // output sends ONE picture, so exactly one of them is on air. `busRow` must be
    // in that row's wired set — the wires ARE the allow-list — and anything else is
    // refused out loud rather than silently showing a bus the output was never
    // wired to. Audio has no "on air" bus: every wired bus is summed.
    Q_INVOKABLE bool showBusOn(int outputRow, int busRow);

    // A screen's picture feeding a sink: a video edge from the screen's node
    // into the sink's node. `linked` = false cuts it. Both rows must be a screen
    // and a non-screen sink that takes video; the verdict is the same as
    // assignBus (accepted here, applied by the engine worker).
    Q_INVOKABLE bool linkScreen(int screenRow, int sinkRow, bool linked);

    // The program-speaker endpoint is not part of the output roster, so its
    // enabled state is stored on its graph node rather than OutputListModel.
    Q_INVOKABLE void setSpeakersEnabled(bool enabled);

    // Does this row carry that signal at all? A physical screen (HDMI) takes
    // video only — its window has no audio path — while NDI/SDI/REC/STREAM
    // carry both. The canvas asks before it offers a drop.
    Q_INVOKABLE bool acceptsSignal(int outputRow, const QString &kind) const;

    // Re-mirror the roster into the graph and re-read every assignment. Called
    // once on construction and after any roster/bus change; also safe to call
    // from QML (the canvas's own refresh affordance).
    Q_INVOKABLE void refresh();

signals:
    void rowsChanged();
    void routingChanged();
    // The engine's verdict, said out loud. Until this existed a refused wire was
    // SILENT: the canvas dropped the gesture, the wire simply never appeared, and
    // nothing said whether the bus was wrong, the output's signal type was wrong,
    // or the mirror had not created the node yet — the exact "I wired it and
    // nothing happened" that cannot be debugged from the UI. Emitted on the
    // interface thread, once per refused gesture.
    void refused(const QString &reason);

private:
    explicit RoutingService(QObject *parent = nullptr);

    // One row, as the worker needs it: everything it will need about an output
    // WITHOUT touching the model (a QAbstractListModel is the interface
    // thread's, and the worker must never read one).
    struct RowSpec
    {
        int row = -1;
        QString nodeId;     // "out:<stableId>"
        QString name;       // engine display name
        QString kind;       // HDMI | NDI | SDI | REC | STREAM
        bool screen = false;
        bool enabled = true;
        // The endpoint this row's audio plays on, as the audio roster's id
        // ("waveout:<n>"). Empty = the program stream. Read on the interface
        // thread, because the worker must never touch the roster model.
        QString audioDevice;
        // STREAM rows: where the row's preset pushes (rtmp://server/key, "" =
        // no preset / no key yet) and whether it asks for a hardware encoder.
        QString networkTarget;
        bool hardwareEncoder = true;
        // REC / STREAM rows: the name of the row's OWN rendered picture, which the recording
        // engine captures instead of the single on-air frame (node meta "recordBuffer").
        QString recordBuffer;
    };

    // Build the specs here, on the interface thread, from the roster.
    QVector<RowSpec> rowSpecs() const;
    // The published list, from the roster + assignments_ (interface thread).
    void rebuildRows();
    // Hand the ENGINE's video wire for each roster row to OutputListModel (bus
    // NUMBER per row, 0 = unwired). That model is what builds the live output
    // spec, so this is the link that makes a drawn wire change a rendered
    // pixel: an output with a video bus composes that bus's feed instead of the
    // program slide (OutputStyleSpec::videoBus → SceneBuilder::BuildBusFeedScene).
    // Interface thread, after every refresh.
    void publishRoutedVideoBuses();
    // Start/stop each row's OWN audio stream so it follows its wires (engine
    // worker). A row with an endpoint and at least one wired audio bus plays its
    // own mix there; every other row is stopped — an unwired stream is closed,
    // not left running on silence, and a row with no endpoint stays in the
    // program mix.
    void bindOutputAudio(const QVector<RowSpec> &specs, const QHash<QString, QVector<int>> &wiredAudio);
    // Say why an engine write was refused (see the `refused` signal): logged with
    // the service's name and emitted on the interface thread, from whatever thread
    // the write was attempted on.
    void reportRefused(const QString &reason);

    static QString nodeIdFor(const QString &stableId, const QString &kind);
    // "bus:<n>#a" / "bus:<n>#v" → the bus ROW, -1 when it is not a bus of the
    // roster any more (a deleted bus leaves assignments behind that must read
    // as unwired).
    static int busRowFromNode(const QString &nodeId);
    static QString busNodeFor(int busRow, bool audio);

    // FEED HEALTH, from the render engine's own per-output counters (see
    // refreshFeedHealth). A card says what the engine DELIVERED — frames and how
    // long since the last one — instead of inferring "live" from being wired.
    struct FeedSample
    {
        bool live = false;
        qint64 ageMs = -1;
        qint64 frames = 0;
        bool operator==(const FeedSample &o) const
        {
            return live == o.live && ageMs == o.ageMs && frames == o.frames;
        }
    };
    // roster row → sample. Missing = never published a frame (or no own pass).
    QHash<int, FeedSample> feed_;
    QTimer feedTimer_;
    // Reads OutputFeedAges on the worker (atomic counters: no device, no frame
    // copy) and re-publishes the rows when anything moved.
    void refreshFeedHealth();
    // How stale a frame may be and still read as live. Below a heartbeat, above
    // one dropped frame at any sane output rate.
    static constexpr qint64 kFeedLiveWindowMs = 1500;

    // nodeId → (video bus node, audio bus node), published by the worker.
    QHash<QString, QPair<QString, QString>> assignments_;
    // sink nodeId → the SCREEN nodes whose video edge reaches it, published by the
    // worker. A screen is an Output node too, so its feed is read from the edges.
    QHash<QString, QStringList> screenFeeds_;
    // WHAT IS WIRED, per output node and per side, from the graph's EDGES (the
    // renderer's and the mixer's own truth — see refresh()). Bus ROWS, in the order
    // the graph reports them. The speaker endpoint is in here under its own node id
    // like any other sink: one shape for every card, no special case.
    QHash<QString, QVector<int>> videoBuses_;
    QHash<QString, QVector<int>> audioBuses_;
    // node id → the refusal already said about that row's own audio stream (see
    // bindOutputAudio). Engine worker only: it is written on the pass that reads
    // the wires, so a device that stays unavailable says its piece once instead of
    // on every routing change.
    QHash<QString, QString> audioTrouble_;
    QVariantList rows_;
    int revision_ = 0;
    bool loggedFirstSync_ = false;
    bool speakersEnabled_ = true;
    // One warning per process for a mirror that ran before the roster had been
    // read (see refresh): the condition is a boot race, not a user-visible state.
    bool warnedRosterNotReady_ = false;

    static QPointer<RoutingService> s_instance;

public:
    // The QML engine builds a registered singleton ITSELF (it does not call
    // create()), so the constructor claims the slot when it is free: whichever
    // object exists first is the one every later caller gets, instead of a
    // second service whose snapshot nobody refreshes.
    static void adoptEngineInstance(RoutingService *service);
};
