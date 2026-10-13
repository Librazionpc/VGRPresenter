#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include <qqml.h>

class QTimer;

// One video source available for routing — a camera, a screen capture, a
// plain media file, or an NDI network feed. Settings · Audio & Video's
// right column ("VIDEO
// SOURCES"). Mirrors AudioInputListModel except for level: only rows whose
// feed carries audio (MEDIA files and NDI streams — NDI embeds audio with
// its video frames) carry a volume; camera/screen feeds are silent (their
// level lives on the bus they route into).
struct VideoSourceItem
{
    // Stable identity — survives row removals (rows after it shift; this
    // doesn't). BusListModel routes key graph edges on it, so removing a
    // row never shifts another row's routes. Assigned at add/duplicate
    // time; never persisted (the roster is session-owned) and never shown.
    QString id;
    QString name;
    // "camera" | "screen" | "media" | "ndi" — the source kinds (capture
    // devices, screen capture, plain media files, NDI network sources);
    // Add/Edit kind chips.
    QString kind = QStringLiteral("camera");
    // Display caption under the name ("PTZ · Wide", "HDMI in", "MP4 · 4K").
    QString sublabel;
    // Capture mode ("1080p60", "720p29.97", "4Kp30" — OBS-style device
    // modes). Empty = unset; the dialogs' preview pill falls back to a
    // nominal readout. A real device's mode list belongs to the engine's
    // video PAL once it exists; until then this is the user's pick from a
    // curated set.
    QString mode;
    // Mute is meaningful for MEDIA rows (they carry audio); camera/screen
    // feeds have no audio of their own to mute.
    bool muted = false;
    // Volume for rows whose feed carries audio — media files and NDI
    // streams (NDI embeds audio with its video), 0-100. Ignored (and shown
    // as no slider) for camera/screen kinds.
    qreal level = 0;   // silence by default — a new source starts muted-down
    // PER-SIDE trim of this row's audio, index 0 = L, 1 = R (1.0 = open,
    // 0.0 = that side closed). The SAME convention AudioInputListModel's
    // channelGains and a bus's use, so an L/R pill means the same thing on
    // every card in the app.
    //
    // WHY A ROW NEEDS IT AT ALL: the Media pane's video cards show the small
    // L/R pair over their state window, and both pills used to write the
    // row's single `muted` flag — clicking L closed BOTH sides and both pills
    // went red at once (the "if i click on the L or R it closes all"
    // report). A pill is a per-side control everywhere else in the app, so
    // it is one here too. Stored state like level/mute (a camera/screen feed
    // has no engine audio node to push a trim into yet).
    QList<qreal> channelGains;
    // The per-side TRIM each side was closed with (index 0 = L, 1 = R; 0/absent
    // = nothing remembered). Closing writes gain 0 to channelGains, which loses
    // the level — this keeps it, so reopening the side from the video card's
    // pill (or from the Edit Video Source dialog's knob) gives the trim back
    // instead of unity (models/ChannelTrim.h). Persisted with the roster.
    QList<qreal> channelTrims;
};

// QML singleton backing Settings · Audio & Video's Video Sources column and
// the routing a BusListModel bus stores against it.
//
// ROUTING KEYS ON STABLE IDS, NOT ROWS: BusListModel's graph edges are cut
// to "vsrc:<id>" nodes (one per row, created on demand). Row indices exist
// only at the QML boundary — toggleVideoRoute takes a row, the models
// translate to ids internally, so deleting a row can never shift anyone
// else's routes. Static accessors let the models translate across each
// other (all QML-singleton instances; created before any routing call runs).
class VideoSourceListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // C++ access to the singleton.
    //
    // EAGER, not lazy (fixed 2026-10-01) — see AudioInputListModel::Instance()'s
    // comment: the static row/stable-id entry points below are reached from
    // BusListModel's construction-time rebuild, before any QML screen exists.
    static VideoSourceListModel *Instance();
    static VideoSourceListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);
    // THE ONE OBJECT — the video half of AudioInputListModel::adoptInstance's
    // contract, adopted from the engine by main.cpp once the module is imported,
    // because the engine default-constructs its own QML_SINGLETON instance (see
    // StyleListModel::create's note). The displaced instance is deactivated: it
    // may not write the roster or push a row's state into the graph any more.
    static void adoptInstance(VideoSourceListModel *roster);

    // Row <-> stable id. rowForStableId returns -1 when the id belongs to a
    // removed row — callers treat that as "no such route".
    static QString stableIdForRow(int row);
    static int rowForStableId(const QString &id);

    // The row's stable id, HEALING a row that has none (see
    // AudioInputListModel::idForRow — the video half of the same guarantee:
    // a row the board can see can always be routed).
    Q_INVOKABLE QString idForRow(int row);
    enum Role {
        NameRole = Qt::UserRole + 1,
        KindRole,
        SublabelRole,
        ModeRole,
        MutedRole,
        LevelRole,
        ChannelGainsRole,
    };
    Q_ENUM(Role)

    explicit VideoSourceListModel(QObject *parent = nullptr);
    ~VideoSourceListModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addSource();
    Q_INVOKABLE void addSourceWith(const QString &name, const QString &kind,
                                   const QString &sublabel, qreal level, bool muted);
    Q_INVOKABLE void removeSource(int index);
    // Insert a copy of row `index` (name suffixed " · copy", mute NOT
    // copied); returns the new row's index, -1 on a bad index. Same
    // convention as OutputListModel::duplicateOutput.
    Q_INVOKABLE int duplicateSource(int index);
    Q_INVOKABLE void renameSource(int index, const QString &name);
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setSublabel(int index, const QString &sublabel);
    // Native video-file picker (mp4/mov/mkv/...) for the Media-kind Select
    // Media picker's "Browse this PC..." option — see
    // AudioInputListModel::pickAudioFile.
    Q_INVOKABLE QString pickVideoFile() const;
    // Capture mode (OBS-style "1080p60"…) — the video dialogs' Resolution
    // picker; empty string = unset.
    Q_INVOKABLE void setMode(int index, const QString &mode);
    Q_INVOKABLE void setMuted(int index, bool muted);
    Q_INVOKABLE void setLevel(int index, qreal level);
    // One side of this row's audio trim — the video card's L/R pill.
    // Mirrors AudioInputListModel::channelGain/setChannelGain exactly
    // (absent = unity, and 0 IS "this side is closed").
    Q_INVOKABLE qreal channelGain(int index, int channel) const;
    Q_INVOKABLE void setChannelGain(int index, int channel, qreal gain);
    // The video card's L/R pill write — the same one reopen policy the source
    // and bus cards use (models/ChannelTrim.h).
    Q_INVOKABLE void toggleChannelGain(int index, int channel);
    // The gain this side should come back at (never the 0 that "closed" stores).
    Q_INVOKABLE qreal channelTrim(int index, int channel) const;
    Q_INVOKABLE void setChannelTrim(int index, int channel, qreal trim);

    // Snapshot for the Edit Video Source dialog.
    Q_INVOKABLE QVariantMap getSource(int index) const;

    // Has the store ANSWERED this roster yet? Same contract as
    // AudioInputListModel::restoreDone: BusListModel's ghost sweep
    // (pruneOrphanSourceEdges) treats a roster as the authority for which
    // "vsrc:<id>" nodes are still live, and an empty roster that simply has not
    // loaded yet must never be read as "every video source is an orphan".
    static bool restoreDone();

    // REAL PUSH — this row's audio state onto its PRODUCTION GRAPH SOURCE NODE
    // ("vsrc:<id>"), the same way AudioInputListModel::pushEffectsToGraph
    // writes an input row's. Level/mute become the node's VolumeControl and the
    // per-side trims its "uiChannelGain<n>" meta, which is exactly what the
    // playout mixer (AudioMixer::SumSource) reads when a bus sums this row.
    //
    // WHY IT DID NOT EXIST: every video row's volume, mute and L/R trims were
    // STORED state only (the roster JSON) — nothing ever reached the graph, so a
    // media/NDI row fed into a bus played at unity whatever its card said, and
    // clicking its L/R pill changed nothing audible. No-op when the row is
    // unrouted (no node = nothing to feed) or the engine is not booted; the
    // route-creating call re-pushes, so a saved fader survives being patched
    // later.
    void pushAudioToGraph(int index);

private:
    static inline VideoSourceListModel *s_instance = nullptr;

    // False once this instance has been displaced by an adopted one (see
    // adoptInstance).
    bool m_active = true;
    void deactivate();

    // The single write of the trim list (the public faces above only decide WHEN
    // a trim is worth remembering). true when it changed anything.
    bool writeChannelTrim(int index, int channel, qreal trim);

    // ---- Roster persistence (session.videoRoster via the settings store,
    // the SAME contract as AudioInputListModel's session.audioRoster): every
    // mutation schedules a debounced write; the constructor retries the
    // restore until the settings singleton is reachable. Ids persist WITH
    // the rows, so bus routing (vsrc:<id> nodes) survives a restart too.
    void scheduleSave();
    QString serializeRoster() const;
    void queueRosterWrite();
    void saveRosterNow();
    int restoreRoster();
    QTimer *saveTimer_ = nullptr;

    QList<VideoSourceItem> m_sources;
    int m_nextId = 1;   // stable-id counter ("v<n>"): renumbered past the restored max at restore time
    bool m_restoreDone = false;   // the store has answered once (see restoreDone)
};
