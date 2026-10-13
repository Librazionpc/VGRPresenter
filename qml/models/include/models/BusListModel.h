#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <qqml.h>

#include "modules/production/ProductionTypes.hpp"

// Forward-declared: adoptRoutingModels() only hands their POINTERS over, and
// including the two roster headers here would make the three models' includes
// circular (they include this one).
class AudioInputListModel;
class VideoSourceListModel;

// One mix bus — a named routing target that audio inputs and/or video
// sources can be sent to (Settings · Audio & Video's middle "BUSES &
// ROUTING" column). THE ENGINE OWNS THE BUSES: every row is a live view of
// bus nodes in the kernel's production graph (docs/specs/27 —
// ProductionEngine/ProductionGraph), created through CreateBus-level graph
// primitives; this model only keeps the row ORDER and mirrors node state.
// A bus is a PAIR of graph nodes — one Audio plane + one Video plane — the
// same video-bus+audio-bus pairing the engine gives every output; the UI's
// `type` ("audio" | "video" | "both") is the routing POLICY on top (which
// rosters may route in), not the nodes' existence.
//
//   - Routing is stored as graph EDGES from stable per-source nodes
//     ("asrc:<AudioInputItem::id>" for audio rows, "vsrc:<VideoSourceItem::id>"
//     for video rows) into the bus's plane node — created on demand at first
//     route. Edges key on the source's STABLE ID, not its roster row, so
//     deleting a roster row can never shift another row's routes (the
//     removed row's own edges are cut by its model on removal).
struct BusItem
{
    // Stable identity = the engine node number ("bus:<id>#a"/"bus:<id>#v")
    // — survives row removals; the routing matrix stores THESE, not rows.
    int id = 0;
    QString name;
    // "audio" | "video" | "both" — which input roster(s) this bus ACCEPTS
    // routing from. The reference image only shows audio-only and
    // video-only buses; "both" is a real third option the Edit dialog
    // exposes. Policy only — both plane nodes always exist in the graph.
    QString type = QStringLiteral("audio");
    qreal level = 75;  // practical headroom by default, mapped to gainDb (-60..0 dB)
    bool muted = false;
    // PER-SIDE trim of the bus's own output, index 0 = L, 1 = R (1.0 = open,
    // 0.0 = that side closed). The same convention an audio INPUT row's
    // channelGains uses, so the L/R pills behave identically on a bus card
    // and on a source card, and 0 means "closed" for both.
    //
    // This is not UI-only state: it rides the stage node's meta
    // ("uiChannelGain<n>") exactly as a source row's trim does, and
    // AudioMixer reads it when it sums the bus plane — so closing a side
    // really silences that side of the bus's mix, and it persists with the
    // production document (which is why it is not a separate roster field).
    QList<qreal> channelGains;
    // The per-side TRIM each side of the bus's own output was closed with
    // (index 0 = L, 1 = R; 0/absent = nothing remembered). Closing writes gain
    // 0 to channelGains, which loses the level — this keeps it so reopening the
    // side gives the trim back instead of unity (models/ChannelTrim.h). Rides
    // the audio plane's meta like channelGains does, so it also survives a
    // restart.
    QList<qreal> channelTrims;
    QList<int> routedAudioInputs;
    QList<int> routedVideoSources;
    // Engine graph node ids: the Audio plane and the Video plane of this
    // bus. Always both set.
    QString engineId;
    QString engineIdVideo;
};

// QML singleton backing Settings · Audio & Video's Buses & Routing column.
class BusListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // C++ access to the singleton.
    //
    // EAGER, not lazy (fixed 2026-10-01) — see AudioInputListModel::Instance()'s
    // comment. This one matters most in practice: its constructor rebuilds the
    // bus rows from the production graph while the Audio & Video screen may
    // never have been opened, and the static bus-number translation below is
    // reached from other models' construction.
    static BusListModel *Instance();
    static BusListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);
    // ONE OBJECT PER MODEL. The QML engine DEFAULT-CONSTRUCTS a QML_SINGLETON
    // C++ type (measured; see StyleListModel::create's note) — so the model the
    // board holds is not the object Instance() returns, and the two drift. Two
    // BusListModels means the board's cached route rows are never re-derived
    // from the roster the operator is editing: a line keeps pointing at a row
    // that is gone ("the lines should remove with source card"). Two rosters
    // mean a row can be resolved against a list that never saw it ("That source
    // can no longer be routed" for a source created a second ago) while both
    // views keep writing the same settings blob. main.cpp resolves the engine's
    // own instances once the module is imported and calls this on the board's
    // model; the displaced ones are deactivated (write paths no-op).
    // (Called from main.cpp — not Q_INVOKABLE: the two roster types are
    // forward-declared here, and a Q_INVOKABLE signature would need them fully
    // defined for the meta-object.)
    void adoptRoutingModels(AudioInputListModel *audioRoster,
                            VideoSourceListModel *videoRoster);

    // Bus row <-> stable bus number (BusItem::id). rowForBusNumber returns
    // -1 when the bus was removed — callers treat that as "no such route".
    static int busNumberAt(int row);
    static int rowForBusNumber(int busNumber);

    enum Role {
        NameRole = Qt::UserRole + 1,
        TypeRole,
        LevelRole,
        MutedRole,
        ChannelGainsRole,
        RoutedAudioInputsRole,
        RoutedVideoSourcesRole,
    };
    Q_ENUM(Role)

    explicit BusListModel(QObject *parent = nullptr);
    ~BusListModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addBus(const QString &name, const QString &type);
    Q_INVOKABLE void removeBus(int index);
    // Insert a copy of row `index` (name suffixed " · copy", mute NOT
    // copied, routing NOT copied — a new bus starts unrouted); returns the
    // new row's index, -1 on a bad index. Same convention as
    // OutputListModel::duplicateOutput.
    Q_INVOKABLE int duplicateBus(int index);
    Q_INVOKABLE void renameBus(int index, const QString &name);
    Q_INVOKABLE void setType(int index, const QString &type);
    Q_INVOKABLE void setLevel(int index, qreal level);
    Q_INVOKABLE void setMuted(int index, bool muted);
    // One side of the bus's own output trim (0 = L, 1 = R), the bus-card
    // L/R pill's write. 0 closes that side, 1 is open; absent = unity.
    // Mirrors AudioInputListModel::channelGain/setChannelGain exactly, which
    // is the whole point: a bus card's pills and a source card's pills are
    // the same control doing the same thing.
    Q_INVOKABLE qreal channelGain(int index, int channel) const;
    Q_INVOKABLE void setChannelGain(int index, int channel, qreal gain);
    // The bus card's L/R pill write — the SAME reopen policy as a source row's
    // (models/ChannelTrim.h): a closed side comes back at the trim it was closed
    // with, never at unity-by-accident.
    Q_INVOKABLE void toggleChannelGain(int index, int channel);
    // The gain this side should come back at (never the 0 that "closed" stores).
    Q_INVOKABLE qreal channelTrim(int index, int channel) const;
    Q_INVOKABLE void setChannelTrim(int index, int channel, qreal trim);
    Q_INVOKABLE void toggleAudioRoute(int busIndex, int inputIndex);
    Q_INVOKABLE void toggleVideoRoute(int busIndex, int sourceIndex);
    // REMOVE-ONLY forms of the two toggles, and what deleting the source ROW
    // calls before the row leaves the roster. The toggle is right for a line's
    // own click (an edge is there, so it comes off) but wrong here: it would
    // ADD a route if the cache and the graph ever disagreed. These cut only an
    // edge that is really there and answer whether one was cut. They have to
    // run while the row still exists — a route is keyed on the row's STABLE id,
    // and a vanished row has no id, so the same call made after the removal is
    // a silent no-op, which is exactly how a line used to outlive its card
    // (the click that removes a line, made too late, did nothing).
    Q_INVOKABLE bool disconnectAudioRoute(int busIndex, int inputIndex);
    Q_INVOKABLE bool disconnectVideoRoute(int busIndex, int sourceIndex);
    // ADD-ONLY, IDEMPOTENT forms of the two toggles, and what the board's
    // drag-to-connect gesture calls. The gesture's contract on this board is
    // "a drag adds a route; right-click a line (or untick it in the bus
    // dialog) removes one" — but the drop used to call the TOGGLE, so a route
    // that was really in the graph answered "already there" only if the
    // model's cached row list knew about it. When that cache was empty (a
    // board built before the rosters restored), the drag's first press on a
    // connected pair DELETED the saved connection instead of doing nothing —
    // then the next drag added it back, which is what "it doesn't connect
    // easily, and my connections don't survive" looks like from the board.
    // Connect-only cannot do that: an existing edge answers true and stays.
    Q_INVOKABLE bool connectAudioRoute(int busIndex, int inputIndex);
    Q_INVOKABLE bool connectVideoRoute(int busIndex, int sourceIndex);
    // THE engine's one validity answer for optimistic drop feedback: the
    // QML board no longer keeps its own copy of the routing rules. `kind`
    // is "audio"|"video" (the drag's payload); audio-only buses reject
    // video, and each bus video plane accepts one provider. A source may
    // fan out to multiple buses.
    Q_INVOKABLE bool canConnect(const QString &kind, int sourceRow, int busIndex) const;
    // WHY a route would be refused — "" when it is allowed. This is the one
    // policy answer behind the board's red card, its dashed ghost line AND its
    // toast: canConnect() is literally `connectRefusal(...).isEmpty()`, so the
    // colour a card wears and the sentence the operator reads cannot disagree.
    // Codes — AudioVideoScreen owns the wording, this owns the decision:
    //   "no-source-row"     that roster row is gone/unknown
    //   "audio-only-bus"    a video source has no video plane to feed here
    //   "already-connected" this exact route is already in the graph
    //   "bus-has-video"     ONE VIDEO PROVIDER PER BUS — it already carries a
    //                       different video source (see connectVideoRoute)
    //   "engine-refused"    the graph itself said no (signal type, a cycle)
    Q_INVOKABLE QString connectRefusal(const QString &kind, int sourceRow, int busIndex) const;

    // Snapshot for the Edit Bus dialog — routedAudioInputs/routedVideoSources
    // surfaced as QVariantList so QML can read them directly.
    Q_INVOKABLE QVariantMap getBus(int index) const;

private:
    static inline BusListModel *s_instance = nullptr;

    // The single write of the trim list (the public faces above only decide WHEN
    // a trim is worth remembering). true when it changed anything.
    bool writeChannelTrim(int index, int channel, qreal trim);

    // Engine-backed plumbing: create the graph nodes + cache row (returns
    // the new row); pull one row's name/level/muted/routes back from the
    // graph. The model starts EMPTY app-side; the constructor then rebuilds
    // rows from whatever the engine's graph holds (restored from the
    // kernel's persisted production document) — the graph is the only
    // truth, across restarts too.
    int createBusRow(const QString &name, const QString &type,
                     qreal level, bool muted);
    void pullRow(int row);
    void rebuildFromGraph();
    // One line per bus, once per session: what the board believes it is routing,
    // read from its CACHED rows — the numbers the cards draw ("n src").
    //
    // For a report of the shape "the bus refuses the connection saying it is
    // already connected, but it is not connected at all": the refusal reads the
    // GRAPH and the card reads the CACHE, so the two can disagree while both are
    // behaving as written, and only the log can say which one is wrong. Cheap,
    // one-shot, and it turns the next such report into a diff against the
    // graph's edges instead of a screenshot.
    void logBoardState() const;

public:
    // Re-read every row's routes from the graph. Routes are cached as roster
    // ROWS, so a roster removal (which shifts rows) must call this or every
    // bus keeps pointing at the old row numbers.
    void refreshRoutes();
    // Re-pull the cached routes ONLY while they disagree with the graph.
    //
    // A graph edge is a route only if a ROSTER ROW owns its source id, so the
    // cache is built through the rosters — and the rows are restored a couple of
    // seconds after this model is constructed. A cache filled before that holds
    // no rows at all: a bus that IS connected (the graph says so, and the drop
    // policy reads the graph) drew as "0 src", and the operator's next drop was
    // refused as "already connected" with nothing on screen to show for it.
    // Called on the same deferral as the edge sweeps below, which is where a
    // roster that reported late becomes visible; a no-op once the two agree.
    void refreshRoutesIfStale();
    // Full re-sync from the graph (rows added/removed behind this model's
    // back — engine facade, restore). Also fired by production.restored /
    // production.bus_changed engine events.
    Q_INVOKABLE void refresh();

    // Edge cleanup invoked by the roster models when a source row is
    // REMOVED: cuts every graph edge from that source's stable node(s)
    // (asrc:<id> / vsrc:<id>) and refreshes the affected rows. No-ops when
    // the source was never routed.
    void cutAudioSourceEdges(const QString &stableId);
    void cutVideoSourceEdges(const QString &stableId);

    // DROP THE GHOSTS. Every "asrc:<id>"/"vsrc:<id>" source node whose stable id
    // has no roster row is a leftover: the roster is the only list of ids that
    // may carry a route, and a freed id is handed out again on the next launch
    // (the counter restarts from the restored rows), so a ghost node is exactly
    // how a brand-new source inherits a deleted one's routes — and how a drawn
    // line outlives the source it was drawn for. Each kind is swept only once
    // its roster has answered (restoreDone), because before that an empty roster
    // would read as "everything is an orphan". Called on a short deferral after
    // construction, and a cheap no-op on a clean graph. Q_INVOKABLE because the
    // board calls it again after a source row is removed: the removal's own cut
    // is the precise instrument, this is the guarantee that a line cannot
    // outlive the card it was drawn for.
    Q_INVOKABLE void pruneOrphanSourceEdges();
    // ONE VIDEO PROVIDER PER BUS has to hold for what is already SAVED too, not
    // only for the next drop: a graph written under the old no-capacity rule can
    // carry two video sources on one bus, and the mixer and the preview only ever
    // use the first — the extras are dead routes the board still draws a line
    // for. Keeps the first (the one the engine reads) and drops the rest, the
    // same answer the board now gives a second source by hand. Runs with the
    // sweep above.
    Q_INVOKABLE void trimExtraVideoProviders();
    // Stable-id graph node id for a roster row ("asrc:<id>"), public for the
    // models that push their own state onto that node (the audio board's
    // effects rack → the node's VolumeControl/ProcessingStages). Empty on a
    // bad or not-yet-constructed roster.
    static QString audioSourceNodeId(int rosterRow);
    // Video's stable-id node id ("vsrc:<id>"); public for the SAME reason
    // (VideoSourceListModel pushes its row's audio state onto that node), and
    // empty on a bad or not-yet-constructed roster. The node may feed more
    // than one bus video plane.
    static QString videoSourceNodeId(int rosterRow);

private:
    // False once this instance has been displaced by an adopted one (see
    // adoptRoutingModels): a model nobody looks at must not re-derive rows or
    // rewrite the graph behind the board's back.
    bool m_active = true;
    void deactivate();
    // (Re-)aim the roster watch at the CURRENT roster singletons — called from
    // the constructor and again on adoption, because the instances to watch are
    // not necessarily the ones the constructor saw.
    void watchRosters();
    QList<QMetaObject::Connection> m_rosterConnections;

    QList<BusItem> m_buses;
    int m_nextBus = 1;   // stable bus number / engine node id counter
};
