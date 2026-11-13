#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVector>
#include <QMetaObject>
#include <QPointer>
#include <QVariantMap>
#include <QVariantList>
#include <qqml.h>
#include <string>
#include <utility>
#include <vector>
#include "modules/presentation/PresentationTypes.hpp"

// Fully defined, not forward-declared: adoptStyleRoster takes it by pointer and
// moc needs the complete type to build the metatype for that slot.
#include "models/StyleListModel.h"

namespace bps::presentation {
}

class QScreen;
class SettingsService;

struct OutputContentToggle
{
    QString key;
    QString label;
    bool enabled = true;
};

struct OutputItem
{
    QString id;
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
    QString screenName;
    // FreeShow's boundsLocked: a locked output keeps its display assignment;
    // the form's map greys out and the picker refuses reassignment.
    bool boundsLocked = false;
    bool stayOnTop = false;
    bool fullscreenOutput = true;
    bool active = false;
    // User-level on/off — a disabled screen is dimmed everywhere and can't
    // go live. Named for the role; QML's Item already owns "enabled".
    bool isEnabled = true;
    QString styleId;
    // Legacy persisted routing mode. GO LIVE always renders the on-air program;
    // this field is retained for compatibility with existing show files.
    QString videoMode = QStringLiteral("direct");
    bool showOverlay = false;
    bool showMedia = false;
    QString mediaFit = QStringLiteral("contain");
    QString audioDevice;
    // REC / STREAM rows: the stable id of the recording / streaming preset this
    // output uses (PresetListModel). "" = none picked.
    QString presetId;
    bool onAirOnly = false;
    QList<OutputContentToggle> content;
};

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
        StayOnTopRole,
        FullscreenOutputRole,
        ActiveRole,
        EnabledRole,
        StyleIdRole,
        StyleNameRole,
        OnAirOnlyRole,
        ContentRole,
        StyleBackgroundRole,
        FrameBufferRole,
        StyleLayoutRole,
        // This output's media fill choice ("contain" | "cover" | "fill") —
        // legacy, see OutputItem::mediaFit: reported, never composed from.
        MediaFitRole,
        // The row's stable id (see OutputItem::id). Settings · Routing keys its
        // output node on this, so a wire survives a rename.
        IdRole,
        AudioDeviceRole,
        // The preset a REC / STREAM row records / streams with (PresetListModel id).
        PresetIdRole,
        VideoModeRole,
        // This row's own Overlays / Media gates (see OutputItem::showOverlay).
        OverlayGateRole,
        MediaGateRole,
    };
    Q_ENUM(Role)

    explicit OutputListModel(QObject *parent = nullptr);
    static OutputListModel *instance();
    static void adoptEngineInstance(OutputListModel *model);
    // See StyleListModel::create's comment: the engine does not call this,
    // which is why the style roster is handed over explicitly at startup.
    static OutputListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // type: "HDMI" | "NDI" | "SDI" | "REC" | "STREAM" — stored as the
    // item's kind and baked into the generated badge.
    // ---- The style roster C++ reads -------------------------------------
    Q_INVOKABLE void adoptStyleRoster(StyleListModel *styles);
    static StyleListModel *adoptedStyleRoster();

    Q_INVOKABLE void addScreen(const QString &name, const QString &type,
                               const QString &resolution, const QString &refresh,
                               const QString &testPattern = QStringLiteral("none"));
    Q_INVOKABLE void addOutput();
    // Refuses for the Main Output (see kMainOutputName in the .cpp) — the
    // UI hides the Delete affordance for it; this is the model-side backstop.
    Q_INVOKABLE void removeOutput(int index);
    Q_INVOKABLE bool isMainOutput(int index) const;
    Q_INVOKABLE void setActive(int index);
    // Returns false when disabling this row would leave no enabled output.
    Q_INVOKABLE bool setEnabled(int index, bool on);
    Q_INVOKABLE void duplicateOutput(int index);
    Q_INVOKABLE void renameOutput(int index, const QString &name);
    QString uniqueOutputName(const QString &candidate, int skipIndex = -1) const;
    Q_INVOKABLE void setKind(int index, const QString &kind);
    Q_INVOKABLE void setResolution(int index, const QString &res);
    Q_INVOKABLE void setRefresh(int index, const QString &refresh);
    Q_INVOKABLE void setTestPattern(int index, const QString &pattern);
    // Assign a style by row (the picker's natural currency; "" = None).
    Q_INVOKABLE void setStyle(int index, const QString &styleId);
    Q_INVOKABLE void toggleContent(int index, const QString &key);

    // ---- Stable identity (see OutputItem::id) ----
    Q_INVOKABLE QString stableIdFor(int row) const;
    Q_INVOKABLE int rowForStableId(const QString &id) const;

    // Snapshot for edit dialogs — one QVariantMap per output with the same
    // keys as the role names, so QML editors don't keep parallel state.
    Q_INVOKABLE QVariantMap getOutput(int index) const;
    bool rosterReady() const { return rosterReady_; }
    // Row of the on-air (active) output; -1 when none is active.
    Q_INVOKABLE int activeIndex() const;

    // ---- Physical displays (FreeShow's Screens.svelte: real displays, not
    Q_PROPERTY(int screensRevision READ screensRevision NOTIFY screensChanged)
    int screensRevision() const;

    Q_INVOKABLE QVariantList displays() const;
    // The display an output is assigned to, as a display() entry ({} = none).
    Q_INVOKABLE QVariantMap displayFor(int index) const;
    Q_INVOKABLE void setScreenName(int index, const QString &screenName);
    Q_INVOKABLE void setBoundsLocked(int index, bool locked);
    Q_INVOKABLE void setStayOnTop(int index, bool stayOnTop);
    Q_INVOKABLE void setFullscreenOutput(int index, bool fullscreen);
    Q_INVOKABLE void setAudioDevice(int index, const QString &deviceId);
    // Wire a REC / STREAM row to a preset ("" = none). Persisted; re-asks Routing,
    // because a stream preset's target is part of the row's output node.
    Q_INVOKABLE void setPresetId(int index, const QString &presetId);
    // Rows wired to this preset, and the model-side backstop when it is deleted.
    int presetUsage(const QString &presetId) const;
    void detachPresetEverywhere(const QString &presetId);
    // A preset was edited: rows that wear it re-read.
    void presetChanged(const QString &presetId);
    Q_INVOKABLE void setVideoMode(int index, const QString &mode);
    Q_INVOKABLE void setShowOverlay(int index, bool on);
    Q_INVOKABLE void setShowMedia(int index, bool on);
    void setRoutedVideoBuses(const QVector<int> &busNumberByRow,
                             const QVector<QVector<int>> &wiredByRow);

    void detachStyleEverywhere(const QString &styleId);

    QList<int> outputsForScreens() const;

    // The ACTIVE output's style, re-resolved (by id) from the current
    // roster. Empty string = the on-air output has no style.
    QString activeStyleId() const;

    Q_INVOKABLE bool activeStyleAllows(const QString &contentType) const;

    Q_INVOKABLE QString activeStyleTemplateId(const QString &contentType) const;
    Q_INVOKABLE QString activeStyleShowCategoryTemplateId(const QString &category) const;

    Q_INVOKABLE QVariantMap styleBackground(int index) const;

signals:
    void activeStyleChanged();
    // Emitted after the roster is persisted and the engine's output styles
    // are pushed, so live output feeds can safely resolve newly added rows.
    void outputRosterChanged();
    void screensChanged();

private:
    void screensChangedNow();
    void trackScreens();
    void trackScreen(QScreen *screen);

    QString nextStableId() const;
    // ---- ONE SCREEN, ONE OUTPUT -------------------------------------------------
    // A physical display can show exactly one output: two rows carrying the same screenName opened two
    // windows at the same x/y and size and drew over each other ("Main Output" and "Main Output 2" on
    // one HDMI). These two keep that true: which row holds a screen, and the first screen nobody holds.
    int rowHoldingScreen(const QString &screenName, int exceptRow) const;
    QString freeScreenName(int exceptRow, const QString &preferred) const;

    int m_screensRevision = 0;

    // The adopted roster, or the C++ singleton as the fallback (null-safe).
    StyleListModel *styleRoster() const;
    void styleRosterChanged();

    // Style lookup helpers over styleRoster() (null-safe).
    QString styleIdAt(int row) const;
    QString styleNameAt(int row) const;
    QString styleLayoutNote(const QString &styleId) const;
    void pushEngineStyle(const QString &styleId, const QString &slideTag = QString());

public:
    void repushActiveStyleForTag(const QString &tag);

    Q_INVOKABLE bool hasOwnPass(int row) const;

    int routedVideoBus(int row) const;

    QString frameBufferFor(int row) const;

    void setRoutedVideoBuses(const QVector<int> &busNumberByRow);
    void repushEngineStyles() { pushActiveEngineStyle(); }

private:
    void pushActiveEngineStyle();
    void notifyFrameRateChanged(int row, const QString &oldRate, const QString &newRate);
    // Id → row through StyleListModel::rowForId (-1 = unknown id).
    int styleRowForId(const QString &styleId) const;

    // Wires the rosterChanged relay (see the constructor comment for why it
    // can't just be done once in the constructor). Idempotent.
    void connectToStyleRoster();
    void connectToTemplateLibrary();
    void connectToEngineBoot();

    QList<OutputItem> m_outputs;
    // See setRoutedVideoBuses: bus NUMBER per roster row, 0 = unwired. Empty
    // until the first routing refresh publishes.
    QVector<int> routedVideoBuses_;
    // Per row: every video bus wired to it, by bus NUMBER — the allow-list a
    // gated scene build is given (OutputStyleSpec::gateVideoBuses).
    QVector<QVector<int>> wiredVideoBuses_;
    // The roster Main.qml handed over (empty until then — see adoptStyleRoster).
    QPointer<StyleListModel> styleRoster_;
    // The same hand-over, visible to EVERY OutputListModel object (QML's and
    // the C++ static) — see adoptedStyleRoster().
    static QPointer<StyleListModel> s_adoptedStyleRoster;
    QMetaObject::Connection styleRosterConn_;
    bool styleRosterConnected_ = false;
    // The last style push, so an identical one is skipped, and a flag so a burst of "refresh the engine
    // styles" calls (column seats, roster changes, route changes) costs ONE rebuild on the next turn.
    bool m_pushQueued = false;
    bool m_hasPushed = false;
    bool m_lastBooted = false;
    bps::presentation::OutputStyleSpec m_lastActiveSpec;
    std::vector<bps::presentation::OutputStyleSpec> m_lastSpecs;
    std::vector<std::string> m_lastBuffers;
    std::vector<std::pair<int, int>> m_lastSizes;
    bool templateLibraryConnected_ = false;
    bool engineBootConnected_ = false;
    // See rosterReady(): set by the first hydrate that ran with the engine up.
    bool rosterReady_ = false;

    void saveRoster();
    void reloadFromStore();

    static QPointer<OutputListModel> s_instance;
    static QList<OutputContentToggle> defaultContent();
};