#include "OutputListModel.h"
#include "StyleListModel.h"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/SettingsService.h"
#include "services/ShowConverter.h"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/project/OutputStore.hpp"

#include <QGuiApplication>
#include <QJSEngine>
#include <QScreen>
#include <QTimer>
#include <QFile>
#include <QTimerEvent>
#include <algorithm>

namespace {
// The one output every show needs — deleting it would leave the app with no
// way to put anything on the primary display, so it's protected everywhere
// (Settings · Outputs hides its Delete affordance; removeOutput refuses).
constexpr auto kMainOutputName = "Main Output";
// The Edit dialog's item-kind keys, in OutputContentToggle display order —
// the same order the engine's OutputStore persists the toggles in.
constexpr const char *kContentKeys[] = { "text", "camera", "media", "clock", "timer", "shape" };
}

namespace {
// Only PHYSICAL-SCREEN outputs go to the audience-facing screen window
// (OutputWindowManager → OutputWindow) and win the live loop's screen
// priority. NDI and other network rows carry ONLY their own transport —
// this is the transport mutual-exclusion contract in one place: one enabled
// HDMI row starts the screen pipeline, one enabled NDI row starts the NDI
// feed, never both from a single row. Falls back to kind=="HDMI" for rows
// persisted before onAirOnly existed.
bool isScreenTransport(const OutputItem &o)
{
    return !o.onAirOnly && o.kind == QLatin1String("HDMI");
}
}

QPointer<OutputListModel> OutputListModel::s_instance = nullptr;

// The one instance, for C++ AND QML — see the header for why this is eager.
OutputListModel *OutputListModel::instance()
{
    static OutputListModel s;
    return &s;
}

OutputListModel *OutputListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    OutputListModel *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}


OutputListModel::OutputListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    Q_ASSERT_X(!s_instance || s_instance == this, "OutputListModel",
               "a second OutputListModel was constructed — instance() would "
               "silently rebind and every C++ reader would hold the wrong one");
    s_instance = this;

    // THE BOOT GAP (found live: "my saved styles/outputs don't show after a
    // rebuild"): this singleton is constructed at QML load, BEFORE the
    // deferred boot has opened kernel.json — the hydrate below read an EMPTY
    // OutputStore every launch (the persisted roster was on disk the whole
    // time) and the first post-boot mutation flushed the default roster over
    // the real one. reloadFromStore() hydrates here (already-booted case) and
    // AGAIN on bootedChanged — see connectToEngineBoot().
    reloadFromStore();

    // A style edit renames/re-keys the theme every output's styleName shows —
    // re-resolve the derived roles when the roster changes. The engine spec
    // IS pushed here too (see the rosterChanged lambda): an edited style that
    // is on air must restyle the live output immediately.
    //
    // QML singleton construction order is undefined — StyleListModel (and the
    // TemplateLibraryService the spec's baked template blocks come from) may
    // not exist yet when this constructor runs — so retry once the event loop
    // starts (by then the QML load has created every singleton).
    //
    // StyleListModel is NO LONGER part of that retry dance: as of 2026-10-01
    // it is an eager C++ singleton (create() hands QML the same static
    // instance), so connectToStyleRoster() below always finds it on the first
    // call. The other two still need the retry — TemplateLibraryService and
    // the engine boot genuinely are not ready at construction.
    connectToStyleRoster();
    connectToTemplateLibrary();
    connectToEngineBoot();
    QTimer::singleShot(0, this, [this]() {
        connectToStyleRoster();
        connectToTemplateLibrary();
        connectToEngineBoot();
    });

    // ---- Monitor hot-plug (found live: plugging an HDMI monitor while the
    // app was running never updated the display picker — displays() re-reads
    // QGuiApplication::screens() on demand, but nothing told the QML side to
    // re-read). This wires the set changes (screenAdded/screenRemoved) and
    // each screen's mode changes (resolution/refresh) into screensChanged().
    trackScreens();
}

// Re-reads the roster from the engine's OutputStore — at construction AND on
// EngineBridge::bootedChanged: this singleton is built at QML load, BEFORE the
// deferred boot has opened kernel.json, so the FIRST read is always empty
// (this function existing is what makes the user's saved outputs come back).
// Wraps in begin/endResetModel: rows are replaced wholesale.
void OutputListModel::reloadFromStore()
{
    beginResetModel();
    m_outputs.clear();

    const QList<OutputContentToggle> content = defaultContent();

    // ---- HYDRATE the persisted roster (the engine's OutputStore, the
    // sibling of StyleStore). This is what makes a style ASSIGNMENT survive
    // a restart: without it the roster rebuilt itself fresh every launch
    // ("Main Output", styleId "") and the boot-time style push carried an
    // empty spec no matter what was saved in Styles. A missing document
    // (first run) or a corrupt one reads as an empty roster — the Main
    // Output default below takes over, never a failed boot.
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary && !screens.isEmpty())
        primary = screens.first();

    bool sawActive = false;
    for (const bps::project::StoredOutput &so : bps::project::OutputStore::Instance().Get()) {
        OutputItem item;
        item.name = QString::fromStdString(so.name);
        item.badge = QString::fromStdString(so.badge);
        item.kind = QString::fromStdString(so.kind);
        item.res = QString::fromStdString(so.res);
        item.refresh = QString::fromStdString(so.refresh);
        item.testPattern = QString::fromStdString(so.testPattern);
        item.screenName = QString::fromStdString(so.screenName);
        item.boundsLocked = so.boundsLocked;
        item.stayOnTop = so.stayOnTop;
        item.fullscreenOutput = so.fullscreenOutput;
        item.isEnabled = so.enabled;
        item.styleId = QString::fromStdString(so.styleId);
        // Legacy rows (before onAirOnly was a thing): an NDI row never wears
        // a screen window — enforce the transport rule on hydrate so pre-
        // existing rosters get the fix without a settings round-trip.
        item.onAirOnly = so.onAirOnly || item.kind == QLatin1String("NDI");
        // Legacy repair: an NDI row persisted before the transport rule kept
        // its old display binding — scrub it so the store and the edit form
        // never show a screen assignment an NDI output cannot use.
        if (item.onAirOnly && !item.screenName.isEmpty()) {
            item.screenName.clear();
            item.res.clear();
            item.refresh.clear();
        }
        item.content = content;   // the six defaults, then the persisted toggles on top
        for (int i = 0; i < item.content.size() && i < 6; ++i)
            item.content[i].enabled = so.contentToggles[i];
        // A persisted screenName may name a display that is no longer there
        // (unplugged monitor): keep the row but drop the dead assignment —
        // the output shows "unassigned" instead of silently pointing at
        // nothing. Resolution still snaps on the next setScreenName.
        if (!item.screenName.isEmpty()) {
            bool screenThere = false;
            for (const QScreen *s : screens)
                if (s->name() == item.screenName) { screenThere = true; break; }
            if (!screenThere) {
                item.screenName.clear();
                item.res.clear();
                item.refresh.clear();
            }
        }
        sawActive = sawActive || item.active;
        m_outputs.append(item);
    }

    // First-run seed only: a brand-new (EMPTY) roster starts with the
    // primary display so the Outputs screen isn't empty. A DELETED Main
    // Output stays deleted — no re-seed on a non-empty roster (the user's
    // roster is the truth), and removeOutput no longer refuses it.
    if (m_outputs.isEmpty()) {
        OutputItem main;
        main.name = QStringLiteral("Main Output");
        main.badge = QStringLiteral("LIVE 1");
        main.kind = QStringLiteral("HDMI");
        if (primary) {
            const QSize mode = primary->size();
            const qreal refresh = primary->refreshRate();
            main.res = QStringLiteral("%1×%2").arg(mode.width()).arg(mode.height());
            main.refresh = refresh > 0 ? QStringLiteral("%1 Hz").arg(qRound(refresh))
                                       : QStringLiteral("60 Hz");
            main.screenName = primary->name();
        } else {
            main.res = QStringLiteral("1920×1080");
            main.refresh = QStringLiteral("60 Hz");
        }
        main.content = content;
        m_outputs.prepend(main);
    }
    // Exactly one active output per session (a crashed run can save none).
    if (!sawActive && !m_outputs.isEmpty())
        m_outputs.first().active = true;

    endResetModel();
    saveRoster();
}

// The whole roster back into the engine's OutputStore (StyleListModel::save
// is the same shape for styles). Every mutator funnels through here.// Only PHYSICAL-SCREEN outputs go to the audience-facing screen window
// (OutputWindowManager → OutputWindow) and win the live loop's screen
// priority. NDI and other network rows carry ONLY their own transport —
// this is the transport mutual exclusion in one place: one enabled HDMI row
// starts the screen pipeline, one enabled NDI row starts the NDI feed, never
// both from a single row. Falls back to kind=="HDMI" for rows persisted
// before onAirOnly existed.
QList<int> OutputListModel::outputsForScreens() const
{
    QList<int> rows;
    for (int i = 0; i < m_outputs.size(); ++i)
        if (m_outputs[i].isEnabled && isScreenTransport(m_outputs[i]))
            rows.append(i);
    return rows;
}

void OutputListModel::saveRoster()
{
    QList<bps::project::StoredOutput> stored;
    stored.reserve(m_outputs.size());
    for (const OutputItem &item : m_outputs) {
        bps::project::StoredOutput so;
        so.id = QStringLiteral("out-%1").arg(qHash(item.name)).toStdString();
        so.name = item.name.toStdString();
        so.badge = item.badge.toStdString();
        so.kind = item.kind.toStdString();
        so.res = item.res.toStdString();
        so.refresh = item.refresh.toStdString();
        so.testPattern = item.testPattern.toStdString();
        so.screenName = item.screenName.toStdString();
        so.boundsLocked = item.boundsLocked;
        so.stayOnTop = item.stayOnTop;
        so.fullscreenOutput = item.fullscreenOutput;
        so.active = item.active;
        so.enabled = item.isEnabled;
        so.styleId = item.styleId.toStdString();
        so.onAirOnly = item.onAirOnly;
        for (int i = 0; i < item.content.size() && i < 6; ++i)
            so.contentToggles[i] = item.content.at(i).enabled;
        stored.append(so);
    }
    (void)bps::project::OutputStore::Instance().Save(
        std::vector<bps::project::StoredOutput>(stored.cbegin(), stored.cend()));

    // EVERY roster change re-pushes the engine's per-output style set (and
    // the active style). saveRoster is the one door every mutator already
    // walks through, so this is what makes a mid-live roster edit take
    // effect on the NEXT FRAME instead of at the next GO LIVE: a New Screen
    // enabled or styled while the show is live starts feeding immediately
    // (the NDI push reads the same buffers this set registers — a buffer
    // missing from the set kept the feed on "waiting for buffer" until
    // restart), a removed/renamed output stops feeding under its old name.
    // Both pushes are idempotent and cheap (SetLiveOutputStyles stores and
    // the loop re-reads per frame; SetActiveOutputStyle no-ops on equality).
    pushEngineStyle(activeStyleId());
}

void OutputListModel::connectToEngineBoot()
{
    // THE BOOT GAP: the active output's saved style ("black bg + tpl-table")
    // must be in the engine BEFORE the first go-live of a session. The spec
    // is otherwise only pushed on roster edits / output re-selection — none
    // of which fire between an app start and the user putting something on
    // air, so the output rendered unstyled (default bg, no template) no
    // matter what was saved. Two ordering traps: the engine may still be
    // booting when the singleShot(0) runs (pushes are dropped pre-boot by
    // SettingsService), and SettingsService::setActiveOutputStyle guards on
    // booted() itself — so this retries on bootedChanged and pushes once.
    //
    // A THIRD trap (found live: a family template picked in Settings never
    // rendered until the style was reassigned): bakeFamilyTemplateBlocks
    // needs TemplateLibraryService's catalog loaded, but that library ALSO
    // only adopts on this same bootedChanged signal (DesignLibraryService::
    // open), via a connection whose relative ORDER against this one is
    // undefined — a push that races ahead of the still-empty catalog bakes
    // nothing and nothing ever re-pushes afterward (the catalog's own
    // adoption emits changed(), but only once, before OutputListModel's
    // matching connectToTemplateLibrary() connection may even exist yet).
    // Deferring the push itself by one event-loop tick (QTimer::singleShot
    // (0, ...), not calling it straight from the signal) lets every other
    // DIRECT bootedChanged listener — including the template library's own
    // — run first within the same synchronous emit, so by the time this
    // fires the catalog is guaranteed populated.
    if (engineBootConnected_)
        return;
    engineBootConnected_ = true;
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this]() {
        if (EngineBridge::instance().booted()) {
            // The constructor's hydrate ran pre-boot against an empty
            // DatabaseManager (kernel.json is only Open()ed during boot) —
            // re-read the persisted roster now, THEN push the active style.
            reloadFromStore();
            QTimer::singleShot(200, this, [this]() { pushActiveEngineStyle(); });
        }
    });
    if (EngineBridge::instance().booted())
        QTimer::singleShot(200, this, [this]() { pushActiveEngineStyle(); });
}

void OutputListModel::connectToStyleRoster()
{
    if (styleRosterConnected_)
        return;
    // Eager singleton since 2026-10-01 — this can only be null if something
    // constructed a SECOND OutputListModel (the Q_ASSERT in the constructor
    // catches that in debug builds). The guard stays because a null
    // dereference here would be a silent style-less render, not a crash.
    StyleListModel *styles = StyleListModel::instance();
    if (!styles)
        return;
    styleRosterConnected_ = true;
    styleRosterConnected_ = true;
    connect(styles, &StyleListModel::rosterChanged, this, [this]() {
        if (m_outputs.isEmpty())
            return;
        const QModelIndex first = index(0);
        const QModelIndex last = index(m_outputs.size() - 1);
        // StyleBackgroundRole rides along: a Save Changes on a style any
        // output wears (colour or background image) must repaint that
        // output's monitor tile the moment the dialog closes.
        emit dataChanged(first, last, { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole });
        // Save Changes on a style that is ON AIR must reach the engine NOW
        // (FreeShow's reactive output.style — the live render loop picks the
        // new spec up within a frame). pushEngineStyle only ran on output
        // selection/assignment before, so editing a live style did nothing
        // until the output was re-selected — the edit looked disconnected
        // from the engine. An empty activeStyleId pushes an empty spec,
        // which is exactly "no style".
        pushActiveEngineStyle();
        emit activeStyleChanged();
    });
}

QList<OutputContentToggle> OutputListModel::defaultContent()
{
    // Mirrors the Edit canvas's item kinds. Order = display order.
    return {
        { QStringLiteral("text"),   QStringLiteral("Text"),       true },
        { QStringLiteral("camera"), QStringLiteral("Camera"),     true },
        { QStringLiteral("media"),  QStringLiteral("Media"),      true },
        { QStringLiteral("clock"),  QStringLiteral("Clock"),      true },
        { QStringLiteral("timer"),  QStringLiteral("Timer"),      true },
        { QStringLiteral("shape"),  QStringLiteral("Shape"),      true },
    };
}

int OutputListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_outputs.size();
}

QVariant OutputListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_outputs.size())
        return {};

    const OutputItem &item = m_outputs.at(index.row());
    switch (role) {
    case NameRole: return item.name;
    case BadgeRole: return item.badge;
    case KindRole: return item.kind;
    case ResRole: return item.res;
    case RefreshRole: return item.refresh;
    case TestPatternRole: return item.testPattern;
    case ScreenNameRole: return item.screenName;
    case OnAirOnlyRole: return item.onAirOnly;
    case BoundsLockedRole: return item.boundsLocked;
    case StayOnTopRole: return item.stayOnTop;
    case FullscreenOutputRole: return item.fullscreenOutput;
    case ActiveRole: return item.active;
    case EnabledRole: return item.isEnabled;
    case StyleIdRole: return item.styleId;
    case StyleNameRole: {
        // Resolved through StyleListModel by id; "" / unknown ids render as
        // "None" (a style removed under us reads as None, never a stale name).
        const int row = styleRowForId(item.styleId);
        if (row < 0)
            return QStringLiteral("None");
        return styleNameAt(row);
    }
    case ContentRole: {
        QVariantList list;
        for (const OutputContentToggle &toggle : item.content)
            list.append(QVariantMap{ { "key", toggle.key }, { "label", toggle.label }, { "enabled", toggle.enabled } });
        return list;
    }
    case StyleBackgroundRole:
        return styleBackground(index.row());
    case FrameBufferRole:
        // Keyed exactly like the engine-side buffer push (saveRoster's id
        // keying): a styled output has its own gated buffer, an unstyled one
        // mirrors the shared preview feed.
        return item.styleId.isEmpty()
                   ? QString()
                   : QStringLiteral("__out_%1__").arg(qHash(item.name));
    default: return {};
    }
}

QHash<int, QByteArray> OutputListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { BadgeRole, "badge" },
        { KindRole, "kind" },
        { ResRole, "res" },
        { RefreshRole, "refresh" },
        { TestPatternRole, "testPattern" },
        { ScreenNameRole, "screenName" },
        { BoundsLockedRole, "boundsLocked" },
        { StayOnTopRole, "stayOnTop" },
        { FullscreenOutputRole, "fullscreenOutput" },
        { ActiveRole, "active" },
        { EnabledRole, "isEnabled" },
        { StyleIdRole, "styleId" },
        { StyleNameRole, "styleName" },
        { OnAirOnlyRole, "onAirOnly" },
        { ContentRole, "content" },
        { StyleBackgroundRole, "styleBackground" },
        { FrameBufferRole, "frameBuffer" },
    };
}

void OutputListModel::addScreen(const QString &name, const QString &type,
                                const QString &resolution, const QString &refresh,
                                const QString &testPattern)
{
    const QString trimmed = name.trimmed();
    const QString res = resolution.trimmed();

    beginInsertRows(QModelIndex(), m_outputs.size(), m_outputs.size());
    OutputItem item;
    item.name = trimmed.isEmpty() ? QStringLiteral("New Screen %1").arg(m_outputs.size() + 1)
                                  : trimmed;
    item.kind = type;
    item.res = res.isEmpty() ? QStringLiteral("1920×1080") : res;
    item.refresh = refresh.trimmed().isEmpty() ? QStringLiteral("60 Hz") : refresh.trimmed();
    item.testPattern = testPattern.trimmed().isEmpty() ? QStringLiteral("none") : testPattern.trimmed();
    // Badge: short type tag + sequence (HDMI 3, NDI 2, …) like the
    // reference's "LIVE 1"/"STAGE 1" pattern.
    const QString prefix = type.isEmpty() ? QStringLiteral("OUT") : type;
    item.badge = QStringLiteral("%1 %2").arg(prefix).arg(m_outputs.size() + 1);
    item.active = false;
    item.isEnabled = true;
    item.styleId = QStringLiteral();
    item.onAirOnly = type == QLatin1String("NDI");
    item.content = defaultContent();
    m_outputs.append(item);
    endInsertRows();
    saveRoster();
}

void OutputListModel::addOutput()
{
    // Legacy no-arg path — kept for the search-index entry points that add
    // a generic screen without opening the dialog.
    addScreen(QStringLiteral(""), QStringLiteral("HDMI"), QStringLiteral(""), QStringLiteral(""));
}

void OutputListModel::duplicateOutput(int index)
{
    if (index < 0 || index >= m_outputs.size())
        return;

    OutputItem copy = m_outputs.at(index);
    copy.name = QStringLiteral("%1 (copy)").arg(copy.name);
    copy.badge = QStringLiteral("OUT %1").arg(m_outputs.size() + 1);
    copy.active = false; // a duplicate starts dark, never fighting the original
    copy.isEnabled = true; // a duplicate always starts usable
    beginInsertRows(QModelIndex(), index + 1, index + 1);
    m_outputs.insert(index + 1, copy);
    endInsertRows();
    saveRoster();
}

void OutputListModel::removeOutput(int index)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    // Main Output is deletable like any other row — the roster may even go
    // empty (the user re-adds what they need). The seeded default only
    // exists so a FIRST-RUN roster isn't blank; it is not mandatory.

    // If the removed output was the on-air one, the engine must drop its
    // style with it (the next setActive pushes the new output's own).
    const bool wasActive = m_outputs.at(index).active;
    const QString wasStyleId = m_outputs.at(index).styleId;

    beginRemoveRows(QModelIndex(), index, index);
    m_outputs.removeAt(index);
    endRemoveRows();
    saveRoster();

    if (wasActive)
        pushEngineStyle(QString());
    else
        (void)wasStyleId;
}

bool OutputListModel::isMainOutput(int index) const
{
    return index >= 0 && index < m_outputs.size()
        && m_outputs.at(index).name == QLatin1String(kMainOutputName);
}

void OutputListModel::setActive(int index)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    // A disabled screen can't be sent live — that's what disabled means.
    if (!m_outputs[index].isEnabled)
        return;

    for (int i = 0; i < m_outputs.size(); ++i) {
        const bool shouldBeActive = (i == index);
        if (m_outputs[i].active != shouldBeActive) {
            m_outputs[i].active = shouldBeActive;
            saveRoster();
            const QModelIndex changed = this->index(i);
            emit dataChanged(changed, changed, { ActiveRole });
        }
    }
    // FreeShow's output.style: the on-air output's style drives the
    // composition. Going live with a different output swaps the engine's
    // spec to that output's own (or clears it when the new output has none).
    pushEngineStyle(m_outputs[index].styleId);
    emit activeStyleChanged();
}

void OutputListModel::setEnabled(int index, bool on)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].isEnabled == on)
        return;

    m_outputs[index].isEnabled = on;
    saveRoster();
    // Disabling also pulls the screen off air — a disabled screen must not
    // keep rendering as LIVE anywhere.
    if (!on && m_outputs[index].active) {
        m_outputs[index].active = false;
        saveRoster();
        const QModelIndex changed = this->index(index);
        emit dataChanged(changed, changed, { EnabledRole, ActiveRole });
        pushEngineStyle(QString());
        emit activeStyleChanged();
    } else {
        const QModelIndex changed = this->index(index);
        emit dataChanged(changed, changed, { EnabledRole });
    }
}

void OutputListModel::renameOutput(int index, const QString &name)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_outputs[index].name == trimmed)
        return;

    m_outputs[index].name = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole });
    saveRoster();
}

void OutputListModel::setResolution(int index, const QString &res)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].res == res)
        return;

    m_outputs[index].res = res;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ResRole });
    saveRoster();
}

void OutputListModel::setKind(int index, const QString &kind)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].kind == kind)
        return;

    m_outputs[index].kind = kind;
    // A display binding only means something for a PHYSICAL-SCREEN output
    // (HDMI). Leaving HDMI with the binding intact used to leave a zombie
    // assignment: OutputWindowManager instantiates a real per-row window for
    // every row with a screen, and its visibility keys on
    // outputEnabled && hasDisplay && live — so a Main Output switched
    // HDMI→NDI still popped the FULLSCREEN SCREEN WINDOW at GO LIVE while
    // the NDI feed sent too (the reported go-live hang: both transports up
    // at once). One output, one transport: NDI/SDI/REC/STREAM rows keep
    // no display. (Going back to HDMI re-picks a screen in the edit form.)
    if (kind != QLatin1String("HDMI") && !m_outputs[index].screenName.isEmpty()) {
        m_outputs[index].screenName.clear();
        m_outputs[index].res.clear();
        m_outputs[index].refresh.clear();
    }
    m_outputs[index].onAirOnly = kind == QLatin1String("NDI");
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { KindRole, ScreenNameRole, ResRole, RefreshRole, OnAirOnlyRole });
    saveRoster();
}

void OutputListModel::setRefresh(int index, const QString &refresh)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    const QString trimmed = refresh.trimmed();
    if (m_outputs[index].refresh == trimmed)
        return;

    const QString previous = m_outputs[index].refresh;
    m_outputs[index].refresh = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { RefreshRole });
    saveRoster();
    notifyFrameRateChanged(index, previous, trimmed);
}

void OutputListModel::setTestPattern(int index, const QString &pattern)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].testPattern == pattern)
        return;

    m_outputs[index].testPattern = pattern;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { TestPatternRole });
    saveRoster();
}

void OutputListModel::setStyle(int index, const QString &styleId)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    const QString normalized = styleId.trimmed();
    // Unknown ids are refused — a typo'd id would render as "None" in the
    // UI while carrying a dangling reference (the row lookup is the truth).
    if (!normalized.isEmpty() && styleRowForId(normalized) < 0)
        return;
    if (m_outputs[index].styleId == normalized)
        return;

    m_outputs[index].styleId = normalized;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole });
    saveRoster();

    // Restyling the output that is on air takes effect immediately —
    // FreeShow's output.style swap (the new style lands on the next frame).
    if (m_outputs[index].active) {
        pushEngineStyle(normalized);
        emit activeStyleChanged();
    }
}

void OutputListModel::toggleContent(int index, const QString &key)
{
    if (index < 0 || index >= m_outputs.size())
        return;

    QList<OutputContentToggle> &content = m_outputs[index].content;
    for (int i = 0; i < content.size(); ++i) {
        if (content[i].key != key)
            continue;
        content[i].enabled = !content[i].enabled;
        const QModelIndex changed = this->index(index);
        emit dataChanged(changed, changed, { ContentRole });
        saveRoster();
        return;
    }
}

QVariantList OutputListModel::displays() const
{
    QVariantList list;
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *screen = screens.at(i);
        const QRect g = screen->geometry();
        list.append(QVariantMap{
            { "index", i },
            { "name", screen->name() },
            { "label", screen->model().isEmpty() ? screen->name() : screen->model() },
            { "x", g.x() },
            { "y", g.y() },
            { "width", g.width() },
            { "height", g.height() },
            { "refresh", qRound(screen->refreshRate()) },
        });
    }
    return list;
}

QVariantMap OutputListModel::displayFor(int index) const
{
    if (index < 0 || index >= m_outputs.size())
        return {};

    const QString target = m_outputs.at(index).screenName;
    if (target.isEmpty())
        return {};

    const QList<QScreen *> screens = QGuiApplication::screens();
    for (const QScreen *screen : screens) {
        if (screen->name() == target) {
            const QRect g = screen->geometry();
            return QVariantMap{
                { "index", static_cast<int>(screens.indexOf(screen)) },
                { "name", screen->name() },
                { "label", screen->model().isEmpty() ? screen->name() : screen->model() },
                { "x", g.x() },
                { "y", g.y() },
                { "width", g.width() },
                { "height", g.height() },
                { "refresh", qRound(screen->refreshRate()) },
            };
        }
    }
    return {};
}

int OutputListModel::screensRevision() const
{
    return m_screensRevision;
}

void OutputListModel::trackScreens()
{
    QGuiApplication *guiApp = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!guiApp)
        return;   // OutputListModel is a QML singleton — it only ever exists under a GUI app
    connect(guiApp, &QGuiApplication::screenAdded, this, [this](QScreen *s) {
        trackScreen(s);
        screensChangedNow();
    });
    connect(guiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *) {
        screensChangedNow();
    });
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *s : screens)
        trackScreen(s);
}

void OutputListModel::trackScreen(QScreen *screen)
{
    if (!screen)
        return;
    // The connection dies with the QScreen when it is unplugged; a re-plug
    // is a NEW QScreen object, wired in from screenAdded above. No
    // Qt::UniqueConnection here — that flag is only legal with member-
    // function-pointer slots, and these are lambdas (passing it anyway
    // makes Qt log "unique connections require a pointer to member
    // function of a QObject subclass" at every launch and then IGNORE it).
    // trackScreen() runs once per screen object, so duplicates can't occur.
    connect(screen, &QScreen::geometryChanged, this, [this](const QRect &) {
        screensChangedNow();
    });
    connect(screen, &QScreen::refreshRateChanged, this, [this](qreal) {
        screensChangedNow();
    });
}

void OutputListModel::screensChangedNow()
{
    // FreeShow's "resolution derives from the display": a bound row's
    // res/refresh re-snaps to its display's current mode whenever the
    // display set or any display's mode changes (a projector switched to 4K
    // mid-service, a monitor re-plugged at a different resolution).
    // screenName is KEPT when a display disappears (see the header comment)
    // — a row whose monitor is gone simply stops matching and keeps its
    // binding for when the monitor returns.
    for (int i = 0; i < m_outputs.size(); ++i) {
        const QString target = m_outputs.at(i).screenName;
        if (target.isEmpty() || m_outputs.at(i).onAirOnly)
            continue;
        for (const QScreen *screen : QGuiApplication::screens()) {
            if (screen->name() != target)
                continue;
            const QRect g = screen->geometry();
            const QString res = QStringLiteral("%1×%2").arg(g.width()).arg(g.height());
            const QString refresh = QStringLiteral("%1 Hz").arg(qRound(screen->refreshRate()));
            if (m_outputs.at(i).res == res && m_outputs.at(i).refresh == refresh)
                break;
            const QString previousRefresh = m_outputs.at(i).refresh;
            m_outputs[i].res = res;
            m_outputs[i].refresh = refresh;
            emit dataChanged(index(i), index(i), { ResRole, RefreshRole });
            saveRoster();
            notifyFrameRateChanged(i, previousRefresh, refresh);
            break;
        }
    }
    ++m_screensRevision;
    emit screensChanged();
}

void OutputListModel::setScreenName(int index, const QString &screenName)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    // An on-air-only output (NDI) has no display binding, period: the edit
    // dialog's stale snapshot (or a stray pick on the form's mini-map) must
    // not resurrect the screen transport on a network row — the guard that
    // actually keeps one output on one transport against every caller.
    if (!screenName.isEmpty() && m_outputs[index].onAirOnly)
        return;
    if (m_outputs[index].screenName == screenName)
        return;

    m_outputs[index].screenName = screenName;

    // Resolution derives from the display (FreeShow's outputLabel):
    // snapping res + refresh to the display's current mode keeps the
    // card honest about what it will actually render at.
    if (!screenName.isEmpty()) {
        const QList<QScreen *> screens = QGuiApplication::screens();
        for (const QScreen *screen : screens) {
            if (screen->name() != screenName)
                continue;
            const QRect g = screen->geometry();
            const QString previousRefresh = m_outputs[index].refresh;
            m_outputs[index].res = QStringLiteral("%1×%2").arg(g.width()).arg(g.height());
            m_outputs[index].refresh = QStringLiteral("%1 Hz").arg(qRound(screen->refreshRate()));
            const QModelIndex changed = this->index(index);
            emit dataChanged(changed, changed, { ScreenNameRole, ResRole, RefreshRole });
            saveRoster();
            notifyFrameRateChanged(index, previousRefresh, m_outputs[index].refresh);
            return;
        }
    }

    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ScreenNameRole });
    saveRoster();
}

void OutputListModel::setBoundsLocked(int index, bool locked)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].boundsLocked == locked)
        return;

    m_outputs[index].boundsLocked = locked;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { BoundsLockedRole });
    saveRoster();
}

void OutputListModel::setStayOnTop(int index, bool stayOnTop)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].stayOnTop == stayOnTop)
        return;

    m_outputs[index].stayOnTop = stayOnTop;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { StayOnTopRole });
    saveRoster();
}

void OutputListModel::setFullscreenOutput(int index, bool fullscreen)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].fullscreenOutput == fullscreen)
        return;

    m_outputs[index].fullscreenOutput = fullscreen;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { FullscreenOutputRole });
    saveRoster();
}

// A frame-rate change is worth telling the operator about: NDI/HDMI/SDI rows
// all carry a Refresh rate, and a mid-service move (or a projector re-snapping
// to a new mode) is exactly the kind of silent change this toast makes
// visible. Fires only on a real rate→rate move — a blank side is "no rate
// yet" (a fresh/unassigned row), not a change.
void OutputListModel::notifyFrameRateChanged(int row, const QString &oldRate, const QString &newRate)
{
    if (row < 0 || row >= m_outputs.size() || oldRate == newRate)
        return;
    if (oldRate.isEmpty() || newRate.isEmpty())
        return;
    const OutputItem &item = m_outputs.at(row);
    EventBus::instance().notify(
        QStringLiteral("%1 (%2) frame rate: %3 → %4")
            .arg(item.name, item.kind, oldRate, newRate),
        QStringLiteral("info"), QStringLiteral("Outputs"),
        QStringLiteral("outputs.framerate.changed"));
}

// ---------------------------------------------------------------------------
// Style plumbing
// ---------------------------------------------------------------------------

int OutputListModel::styleRowForId(const QString &styleId) const
{
    const StyleListModel *styles = StyleListModel::instance();
    if (!styles || styleId.isEmpty())
        return -1;
    return styles->rowForId(styleId);
}

bool OutputListModel::activeStyleAllows(const QString &contentType) const
{
    const int row = styleRowForId(activeStyleId());
    if (row < 0)
        return true;   // no style on the active output: everything is allowed
    const QVariantMap style = StyleListModel::instance()->getStyle(row);
    const QString key = QStringLiteral("show") + contentType.left(1).toUpper()
                        + contentType.mid(1);
    if (!style.contains(key))
        return true;   // unknown content type: not gated
    return style.value(key).toBool();
}

QVariantMap OutputListModel::styleBackground(int index) const
{
    const int row = styleRowForId(index >= 0 && index < m_outputs.size()
                                      ? m_outputs.at(index).styleId : QString());
    if (row < 0)
        return { { "color", QStringLiteral("transparent") },
                 { "image", QString() }, { "hasImage", false },
                 { "clearOnText", false } };
    const QVariantMap style = StyleListModel::instance()->getStyle(row);
    const QString image = style.value(QStringLiteral("backgroundImage")).toString();
    // clearBackgroundOnText (FreeShow's clearStyleBackgroundOnText) already
    // reaches the ENGINE's own render spec (see pushEngineStyle), which
    // presumably honors it for the real distributed frame — but this role
    // feeds the monitor tile's OWN QML-side background Rectangle, drawn
    // behind a locally-rendered DesignPreview slide (a text/scripture slide
    // doesn't go through the distributed-frame path — see OutputMonitorTile's
    // framePriority gate). Without this, a style's own baked-in background
    // image collided visually with live text on top of it, unconditionally.
    return { { "color", style.value(QStringLiteral("backgroundColor")).toString() },
             { "image", image },
             { "hasImage", !image.isEmpty() && QFile::exists(image) },
             { "clearOnText", style.value(QStringLiteral("clearBackgroundOnText")).toBool() } };
}

QString OutputListModel::styleIdAt(int row) const
{
    const StyleListModel *styles = StyleListModel::instance();
    if (!styles || row < 0 || row >= styles->rowCount())
        return QString();
    return styles->data(styles->index(row, 0), StyleListModel::IdRole).toString();
}

QString OutputListModel::styleNameAt(int row) const
{
    const StyleListModel *styles = StyleListModel::instance();
    if (!styles || row < 0 || row >= styles->rowCount())
        return QStringLiteral("None");
    return styles->data(styles->index(row, 0), StyleListModel::NameRole).toString();
}

QString OutputListModel::activeStyleId() const
{
    for (const OutputItem &item : m_outputs)
        if (item.active)
            return item.styleId;
    return QString();
}

int OutputListModel::activeIndex() const
{
    for (int i = 0; i < m_outputs.size(); ++i)
        if (m_outputs.at(i).active)
            return i;
    return -1;
}

void OutputListModel::pushEngineStyle(const QString &styleId)
{
    // Compose the engine's view of the style (PresentationTypes.hpp's
    // OutputStyleSpec): background + layout preset + the clear-on-text flag.
    // An empty styleId pushes an empty spec — the engine renders unstyled.
    bps::presentation::OutputStyleSpec spec;
    const int row = styleRowForId(styleId);
    if (row >= 0) {
        const StyleListModel *styles = StyleListModel::instance();
        const QVariantMap style = styles->getStyle(row);
        spec.name = style.value(QStringLiteral("name")).toString().toStdString();
        spec.contentType = style.value(QStringLiteral("contentType")).toString().toStdString();
        spec.templateKey = style.value(QStringLiteral("templateKey")).toString().toStdString();
        spec.backgroundColor = style.value(QStringLiteral("backgroundColor")).toString().toStdString();
        spec.backgroundImage = style.value(QStringLiteral("backgroundImage")).toString().toStdString();
        spec.clearBackgroundOnText =
            style.value(QStringLiteral("clearBackgroundOnText")).toBool();
        spec.showShows = style.value(QStringLiteral("showShows")).toBool();
        spec.showMedia = style.value(QStringLiteral("showMedia")).toBool();
        spec.showScripture = style.value(QStringLiteral("showScripture")).toBool();
        spec.showTable = style.value(QStringLiteral("showTable")).toBool();
        // PER-FAMILY template picks — the family keys ride the spec; the
        // bake below fills each family's blocks when the key names a design.
        spec.familyTemplateKeys[0] = style.value(QStringLiteral("familyTemplateShows")).toString().toStdString();
        spec.familyTemplateKeys[1] = style.value(QStringLiteral("familyTemplateMedia")).toString().toStdString();
        spec.familyTemplateKeys[2] = style.value(QStringLiteral("familyTemplateScripture")).toString().toStdString();
        spec.familyTemplateKeys[3] = style.value(QStringLiteral("familyTemplateTable")).toString().toStdString();
        spec.category = style.value(QStringLiteral("category")).toString().toStdString();
        bakeTemplateBlocks(spec);
        bakeFamilyTemplateBlocks(spec);
    }
    if (SettingsService *settings = SettingsService::instancePtr())
        settings->setActiveOutputStyle(spec);
    // THE OTHER LIVE OUTPUTS: push the per-output style set so the engine's
    // loop renders one gated pass per style-wearing live output. EVERY live
    // output is included (the active one too — its entry is the same spec,
    // the loop renders it into its own named buffer for the tile); outputs
    // without a style push nothing (their tile mirrors the main pass).
    std::vector<bps::presentation::OutputStyleSpec> specs;
    std::vector<std::string> buffers;
    for (int i = 0; i < m_outputs.size(); ++i) {
        const OutputItem &item = m_outputs.at(i);
        // ENABLED + styled only (the comment below is the contract): the old
        // `!isEnabled || hasStyle` De-Morgan slip also pushed specs for
        // DISABLED styled outputs — wasted gated passes per frame, and a
        // live output whose style was just removed kept its old spec pushed
        // (the tile went unstyled while the real output stayed styled).
        if (item.isEnabled && !item.styleId.isEmpty()) {
            const int styleRow = styleRowForId(item.styleId);
            if (styleRow >= 0) {
                bps::presentation::OutputStyleSpec outSpec;
                const QVariantMap outStyle = StyleListModel::instance()->getStyle(styleRow);
                outSpec.name = outStyle.value(QStringLiteral("name")).toString().toStdString();
                outSpec.contentType = outStyle.value(QStringLiteral("contentType")).toString().toStdString();
                outSpec.templateKey = outStyle.value(QStringLiteral("templateKey")).toString().toStdString();
                outSpec.backgroundColor = outStyle.value(QStringLiteral("backgroundColor")).toString().toStdString();
                outSpec.backgroundImage = outStyle.value(QStringLiteral("backgroundImage")).toString().toStdString();
                outSpec.clearBackgroundOnText = outStyle.value(QStringLiteral("clearBackgroundOnText")).toBool();
                outSpec.showShows = outStyle.value(QStringLiteral("showShows")).toBool();
                outSpec.showMedia = outStyle.value(QStringLiteral("showMedia")).toBool();
                outSpec.showScripture = outStyle.value(QStringLiteral("showScripture")).toBool();
                outSpec.showTable = outStyle.value(QStringLiteral("showTable")).toBool();
                outSpec.familyTemplateKeys[0] = outStyle.value(QStringLiteral("familyTemplateShows")).toString().toStdString();
                outSpec.familyTemplateKeys[1] = outStyle.value(QStringLiteral("familyTemplateMedia")).toString().toStdString();
                outSpec.familyTemplateKeys[2] = outStyle.value(QStringLiteral("familyTemplateScripture")).toString().toStdString();
                outSpec.familyTemplateKeys[3] = outStyle.value(QStringLiteral("familyTemplateTable")).toString().toStdString();
                bakeTemplateBlocks(outSpec);
                bakeFamilyTemplateBlocks(outSpec);
                // Buffer name keyed by output identity (qHash(name) — the same
                // keying saveRoster uses for StoredOutput ids).
                specs.push_back(std::move(outSpec));
                buffers.push_back(QStringLiteral("__out_%1__").arg(qHash(item.name)).toStdString());
            }
        }
    }
    if (SettingsService *settings2 = SettingsService::instancePtr())
        settings2->setLiveOutputStyles(specs, buffers);
}

void OutputListModel::bakeTemplateBlocks(bps::presentation::OutputStyleSpec &spec)
{
    // A legacy preset key ("lowerThird"...) rides StyleBuilder::LayoutFor —
    // nothing to bake. Anything else is an ENGINE TEMPLATE DESIGN id
    // ("tpl-…", picked from the Template library): copy that design's blocks
    // into the spec so the engine renders the template as the style's layout.
    // The engine never reads the library itself — the pushed spec is the
    // whole truth — so this is also what makes a template edit reach the
    // on-air output (this model re-pushes on TemplateLibraryService::changed).
    const QString key = QString::fromStdString(spec.templateKey);
    if (key.isEmpty() || key.startsWith(QLatin1String("tpl-")) == false)
        return;
    const QVariantMap design = TemplateLibraryService::instance().design(key);
    const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
    if (blocks.isEmpty())
        return;   // unknown/deleted id: keep the plain-layout fallback
    // THE TEMPLATE'S OWN BACKGROUND WINS when the style's colour is empty or
    // transparent: the template design IS the look the user picked (Big Bold
    // carries #000000) — a transparent style colour must not steamroll it.
    // The style's colour still wins when the style actually SETS one (and the
    // slide's own background beats both, in the engine's precedence).
    const QString templateBg = design.value(QStringLiteral("background")).toString();
    if (!templateBg.isEmpty() && templateBg != QLatin1String("transparent")
        && (spec.backgroundColor.empty() || spec.backgroundColor == "transparent"))
        spec.backgroundColor = templateBg.toStdString();
    spec.templateBlocks.reserve(blocks.size());
    for (const QVariant &b : blocks)
        spec.templateBlocks.push_back(ShowConverter::blockFromVariant(b.toMap()));
}

// NOTE: bakeTemplateBlocks bakes ONLY the whole-style template. The
// per-family slots bake here (their keys are already in spec.familyTemplateKeys).
// INHERIT: a family slot left on "None" rides the style's WHOLE-STYLE design
// (its documented contract — StyleStore.hpp: an empty key "inherits the
// style's whole-style templateKey"). The old no-inherit behaviour stranded
// exactly the families a style didn't name: with the style's contentType on
// "shows", SceneBuilder's family gate excluded the whole-style template from
// scripture/table slides, so the slide's own tab template (default font,
// default autoSize) rendered instead and the style's font rules never
// reached the output — the live "NDI doesn't see the font rules I set" and
// "scripture always shrinks" reports. Binding is family-agnostic ("text" =
// the content, "ref" = the reference — BindTemplateBlocks), so any design
// carries any family's content. SHOWS additionally keeps a Bible fallback
// (the Scripture design) for when even the whole-style template is unset or
// a legacy preset: show content (imported shows, Quick Lyrics — slides
// carry no family tag) must never silently fall back to the old plain
// layout. An explicit pick always wins over both.
void OutputListModel::bakeFamilyTemplateBlocks(bps::presentation::OutputStyleSpec &spec)
{
    for (size_t i = 0; i < 4; ++i) {
        const QString famKey = QString::fromStdString(spec.familyTemplateKeys[i]);
        if (famKey.isEmpty() || famKey.startsWith(QLatin1String("tpl-")) == false)
            continue;   // "" = none for this family; a preset key rides LayoutFor — no bake
        const QVariantMap design = TemplateLibraryService::instance().design(famKey);
        const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
        if (blocks.isEmpty())
            continue;   // unknown/deleted id: the family falls back per SceneBuilder
        // Same template-bg-wins rule as bakeTemplateBlocks: a family template
        // with its own background (the Table design's #000000) adopts into the
        // style when the style's colour is unset/transparent. This is what
        // made Table render black while Shows/Scripture (transparent-bg
        // designs) showed the engine's old tinted fallback instead.
        const QString famBg = design.value(QStringLiteral("background")).toString();
        if (!famBg.isEmpty() && famBg != QLatin1String("transparent")
            && (spec.backgroundColor.empty() || spec.backgroundColor == "transparent"))
            spec.backgroundColor = famBg.toStdString();
        spec.familyTemplateBlocks[i].reserve(blocks.size());
        for (const QVariant &b : blocks)
            spec.familyTemplateBlocks[i].push_back(ShowConverter::blockFromVariant(b.toMap()));
    }
    // The inherit pass: only when the whole-style template is an ENGINE
    // DESIGN (a legacy preset key can't be baked — the engine renders those
    // via LayoutFor with the family key, which is empty here by definition).
    const QString wholeKey = QString::fromStdString(spec.templateKey);
    if (wholeKey.startsWith(QLatin1String("tpl-"))) {
        const QVariantMap whole = TemplateLibraryService::instance().design(wholeKey);
        const QVariantList wholeBlocks = whole.value(QStringLiteral("blocks")).toList();
        if (!wholeBlocks.isEmpty()) {
            for (size_t i = 0; i < 4; ++i) {
                if (!spec.familyTemplateKeys[i].empty() || !spec.familyTemplateBlocks[i].empty())
                    continue;   // explicit pick (baked above) always wins
                spec.familyTemplateBlocks[i].reserve(wholeBlocks.size());
                for (const QVariant &b : wholeBlocks)
                    spec.familyTemplateBlocks[i].push_back(ShowConverter::blockFromVariant(b.toMap()));
            }
        }
    }
    // The Bible fallback for show content (see the function comment): the
    // baked blocks alone gate the engine's family branch — the shows key
    // stays "" so the roster still reads "None".
    if (spec.familyTemplateBlocks[0].empty()
        && QString::fromStdString(spec.familyTemplateKeys[0]).isEmpty()
        && !wholeKey.startsWith(QLatin1String("tpl-"))) {
        const QVariantMap design = TemplateLibraryService::instance().design(QStringLiteral("tpl-scripture"));
        const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
        if (!blocks.isEmpty()) {
            spec.familyTemplateBlocks[0].reserve(blocks.size());
            for (const QVariant &b : blocks)
                spec.familyTemplateBlocks[0].push_back(ShowConverter::blockFromVariant(b.toMap()));
        }
    }
}

// One push for the CURRENT active output's style — the single entry point
// every "something about the on-air look changed" relay funnels into. No-op
// when the style roster hasn't been adopted yet (see connectToStyleRoster).
void OutputListModel::pushActiveEngineStyle()
{
    if (!styleRosterConnected_)
        return;
    pushEngineStyle(activeStyleId());
}

void OutputListModel::connectToTemplateLibrary()
{
    if (templateLibraryConnected_)
        return;
    // The template library is another QML singleton — same lazy-construction
    // story as StyleListModel in the constructor — so this too may run before
    // it exists and is retried from the same singleShot(0).
    TemplateLibraryService *templates = &TemplateLibraryService::instance();
    if (!templates)
        return;
    templateLibraryConnected_ = true;
    // THE STYLE'S TEMPLATE WAS EDITED (Save Changes on a template the style
    // wears): the roster fields are untouched, so only this relay gets the
    // news. Re-bake + re-push; the engine sees a block-only spec change and
    // rebuilds the scenes (fingerprint hash moves).
    connect(templates, &DesignLibraryService::changed, this, [this]() {
        pushActiveEngineStyle();
    });
}

void OutputListModel::detachStyleEverywhere(const QString &styleId)
{
    if (styleId.isEmpty())
        return;
    bool touched = false;
    for (int i = 0; i < m_outputs.size(); ++i) {
        if (m_outputs[i].styleId != styleId)
            continue;
        m_outputs[i].styleId = QString();
        const QModelIndex changed = index(i);
        emit dataChanged(changed, changed, { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole });
        touched = true;
    }
    if (touched)
        saveRoster();
    // If the on-air output wore the removed style, the engine drops it now.
    if (touched && activeStyleId().isEmpty())
        pushEngineStyle(QString());
}

QVariantMap OutputListModel::getOutput(int index) const
{
    if (index < 0 || index >= m_outputs.size())
        return {};

    const OutputItem &item = m_outputs.at(index);
    QVariantList content;
    for (const OutputContentToggle &toggle : item.content)
        content.append(QVariantMap{ { "key", toggle.key },
                                    { "label", toggle.label },
                                    { "enabled", toggle.enabled } });

    const int row = styleRowForId(item.styleId);
    QString styleName = QStringLiteral("None");
    if (row >= 0)
        styleName = styleNameAt(row);

    return {
        { "name", item.name },
        { "badge", item.badge },
        { "kind", item.kind },
        { "res", item.res },
        { "refresh", item.refresh },
        { "testPattern", item.testPattern },
        { "screenName", item.screenName },
        { "boundsLocked", item.boundsLocked },
        { "stayOnTop", item.stayOnTop },
        { "fullscreenOutput", item.fullscreenOutput },
        { "active", item.active },
        { "isEnabled", item.isEnabled },
        { "styleId", item.styleId },
        { "onAirOnly", item.onAirOnly },
        { "styleName", styleName },
        { "content", content },
    };
}
