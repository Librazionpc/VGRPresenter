#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVariantMap>
#include <QVariantList>
#include <qqml.h>

// One content toggle in an output's edit dialog — which canvas item kinds
// that output is allowed to show. Kinds mirror the Edit canvas's item types
// (text/camera/media/clock/timer/shape) — deliberately NOT FreeShow's
// shows/songs/etc., which are content-library concepts, not output concepts.
struct OutputContentToggle
{
    QString key;
    QString label;
    bool enabled = true;
};

struct OutputItem
{
    QString name;
    QString badge;
    // "HDMI" | "NDI" | "SDI" | "REC" | "STREAM" (legacy rows may carry
    // "Screen"/"Stream") — the Add dialog's Output type chips.
    QString kind;
    QString res;
    // Refresh rate as chosen in the Add/Edit form ("60 Hz"…). Display-only
    // today; a real output backend will need it.
    QString refresh;
    // "smpte" | "gradient" | "checker" | "solidred" | "none"
    QString testPattern = QStringLiteral("none");
    // Physical display this output renders on (QScreen::name, "" = unassigned
    // — windowed/NDI/REC outputs don't sit on a display). The form's placement
    // map assigns it; resolution derives from the display's current mode.
    QString screenName;
    // FreeShow's boundsLocked: a locked output keeps its display assignment;
    // the form's map greys out and the picker refuses reassignment.
    bool boundsLocked = false;
    bool active = false;
    // User-level on/off — a disabled screen is dimmed everywhere and can't
    // go live. Named for the role; QML's Item already owns "enabled".
    bool isEnabled = true;
    // Style applied from StyleListModel — index into that model, -1 = none.
    int styleIndex = 0;
    QList<OutputContentToggle> content;
};

// Mock CRUD backend for the app's output roster (screens/streams a show can
// be sent to). A QML singleton — not a per-screen instance — because both
// Settings · Outputs (OutputsScreen.qml) and the Edit screen's output-
// monitor grid (EditScreen.qml) render the SAME roster from two different,
// unrelated branches of the QML tree; a plain property couldn't be handed
// down to both without threading it through Main.qml. Adding/removing an
// output, renaming, restyling or toggling content now shows up in both
// places at once instead of two hand-typed arrays silently drifting apart.
class OutputListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        BadgeRole,
        KindRole,
        ResRole,
        RefreshRole,
        TestPatternRole,
        ScreenNameRole,
        BoundsLockedRole,
        ActiveRole,
        EnabledRole,
        StyleIndexRole,
        StyleNameRole,
        ContentRole,
    };
    Q_ENUM(Role)

    explicit OutputListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // type: "HDMI" | "NDI" | "SDI" | "REC" | "STREAM" — stored as the
    // item's kind and baked into the generated badge.
    Q_INVOKABLE void addScreen(const QString &name, const QString &type,
                               const QString &resolution, const QString &refresh,
                               const QString &testPattern = QStringLiteral("none"));
    Q_INVOKABLE void addOutput();
    Q_INVOKABLE void removeOutput(int index);
    Q_INVOKABLE void setActive(int index);
    Q_INVOKABLE void setEnabled(int index, bool on);
    Q_INVOKABLE void duplicateOutput(int index);
    Q_INVOKABLE void renameOutput(int index, const QString &name);
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setResolution(int index, const QString &res);
    Q_INVOKABLE void setRefresh(int index, const QString &refresh);
    Q_INVOKABLE void setTestPattern(int index, const QString &pattern);
    Q_INVOKABLE void setStyle(int index, int styleIndex);
    Q_INVOKABLE void toggleContent(int index, const QString &key);

    // Snapshot for edit dialogs — one QVariantMap per output with the same
    // keys as the role names, so QML editors don't keep parallel state.
    Q_INVOKABLE QVariantMap getOutput(int index) const;

    // ---- Physical displays (FreeShow's Screens.svelte: real displays, not
    // a mock map) ----
    // One entry per QScreen: { name, label, x, y, width, height, refresh } —
    // geometry in its native pixels so the form's mini-map can lay the tiles
    // out relative to each other exactly like the desktop.
    Q_INVOKABLE QVariantList displays() const;
    // The display an output is assigned to, as a display() entry ({} = none).
    Q_INVOKABLE QVariantMap displayFor(int index) const;
    // Assign/output-side setters. setScreen also snaps the output's res and
    // refresh to the display's current mode (resolution derives from the
    // display, like FreeShow's outputLabel).
    Q_INVOKABLE void setScreenName(int index, const QString &screenName);
    Q_INVOKABLE void setBoundsLocked(int index, bool locked);

private:
    QList<OutputItem> m_outputs;

    static QList<OutputContentToggle> defaultContent();
};
