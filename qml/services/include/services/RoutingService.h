#pragma once

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

    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(int revision READ revision NOTIFY routingChanged)

public:
    static RoutingService *instance();
    static RoutingService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~RoutingService() override;

    QVariantList rows() const { return rows_; }
    int revision() const { return revision_; }

    Q_INVOKABLE bool assignBus(int outputRow, const QString &kind, int busRow);

    Q_INVOKABLE bool unwireBus(int outputRow, const QString &kind, int busRow);

    Q_INVOKABLE bool showBusOn(int outputRow, int busRow);

    Q_INVOKABLE bool linkScreen(int screenRow, int sinkRow, bool linked);

    // The program-speaker endpoint is not part of the output roster, so its
    // enabled state is stored on its graph node rather than OutputListModel.
    Q_INVOKABLE void setSpeakersEnabled(bool enabled);
    // Whether the Speakers output is switched on (the same state the Routing screen's Speakers card shows).
    bool speakersEnabled() const { return speakersEnabled_; }
    // False until the first sync with the engine's graph: before that speakersEnabled() is only the default.
    bool speakersKnown() const { return loggedFirstSync_; }


    // Whether the program's MEDIA can reach this output's picture, answered from the published
    // rows: "ok" when the output's own Media layer is on, else "media". A bus is NOT part of this
    // rule — a bus needs a source AND the media layer, an output needs only the media layer. THE one
    // place this rule lives: the monitor
    // surfaces gate on it and the take notice names what it reports, so the picture on screen and
    // the words in the toast cannot disagree. A row that is not a video output, or is switched
    // off, answers "ok" — nothing to gate and nothing to report.
    Q_INVOKABLE QString mediaRouteState(int outputRow) const;
    // THE SAME RULE, ASKED BY OUTPUT NAME. A monitor tile's model index and the board's row are not the
    // same key in every model, and matching on the index left a genuinely routed tile asking about a
    // row that was not its own — the gate then reported "not routed" forever and the tile stayed black.
    // The name is the identity every surface already has.
    Q_INVOKABLE QString mediaRouteStateForName(const QString &name) const;
    Q_INVOKABLE bool outputShowsOverlaysForName(const QString &name) const;

    // Is there at least one enabled video output on the board at all? An app with nothing to route to
    // (a library-only session) must still be able to play a file as a preview.
    Q_INVOKABLE bool hasEnabledVideoOutput() const;
    // Can the media layer reach ANY enabled video output right now? Asked BEFORE a take: a file nothing
    // can receive is refused up front instead of being started and pulled back down.
    Q_INVOKABLE bool mediaCanReachAnyOutput() const;

    Q_INVOKABLE void refresh();

signals:
    void rowsChanged();
    void routingChanged();
    void refused(const QString &reason);

private:
    explicit RoutingService(QObject *parent = nullptr);

    // ---- C++-ONLY, DELIBERATELY NOT QML ------------------------------------
    // The QML surface of this service is what QML actually asks: the gates are asked BY NAME
    // (mediaRouteStateForName / outputShowsOverlaysForName — the pair OutputGateState gives the monitor
    // tile and the output window), and the setters the Routing screen calls. acceptsSignal is the
    // row-keyed half the wiring code below checks before it writes an edge (three call sites, in
    // assignBus / unwireBus / linkScreen), so it is not part of the invokable surface either.
    bool acceptsSignal(int outputRow, const QString &kind) const;

    struct RowSpec
    {
        int row = -1;
        QString nodeId;     // "out:<stableId>"
        QString name;       // engine display name
        QString kind;       // HDMI | NDI | SDI | REC | STREAM
        bool screen = false;
        bool enabled = true;
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
    void publishRoutedVideoBuses();
    void bindOutputAudio(const QVector<RowSpec> &specs, const QHash<QString, QVector<int>> &wiredAudio);
    void reportRefused(const QString &reason);

    static QString nodeIdFor(const QString &stableId, const QString &kind);
    static int busRowFromNode(const QString &nodeId);
    static QString busNodeFor(int busRow, bool audio);

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
    QHash<QString, QVector<int>> videoBuses_;
    QHash<QString, QVector<int>> audioBuses_;
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
    static void adoptEngineInstance(RoutingService *service);
};