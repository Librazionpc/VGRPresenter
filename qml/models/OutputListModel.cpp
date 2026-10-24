#include "OutputListModel.h"
#include "FamilySlots.h"
#include "StyleListModel.h"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/SettingsService.h"
#include "services/ShowConverter.h"
#include "services/StyleSpecBuilder.h"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/SlideRules.hpp"
#include "modules/project/OutputStore.hpp"

#include <QGuiApplication>
#include <QJSEngine>
#include <QRegularExpression>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QTimerEvent>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace {
// The one output every show needs — deleting it would leave the app with no
// way to put anything on the primary display, so it's protected everywhere
// (Settings · Outputs hides its Delete affordance; removeOutput refuses).
constexpr auto kMainOutputName = "Main Output";
// The Edit dialog's item-kind keys, in OutputContentToggle display order —
// the same order the engine's OutputStore persists the toggles in.
constexpr const char *kContentKeys[] = { "text", "camera", "media", "clock", "timer", "shape" };

// Parses the "res" role's "1920×1080" / "1920 × 1080" display string into a
// (width, height) pair, or {0,0} when absent/unparseable — the pushed-spec
// side's signal for "fall back to the engine's own default" (SetLiveOutputStyles'
// own contract). A screen-bound row keeps this snapped to the display's real
// mode (setScreenName/screensChangedNow); any OTHER row (NDI included) only
// ever gets a value here from the Edit dialog's own Resolution picker.
std::pair<int, int> parseResolution(const QString &res)
{
    const QStringList parts = res.split(QChar(0x00D7));   // '×'
    if (parts.size() != 2)
        return { 0, 0 };
    bool okW = false, okH = false;
    const int w = parts.at(0).trimmed().toInt(&okW);
    const int h = parts.at(1).trimmed().toInt(&okH);
    return (okW && okH && w > 0 && h > 0) ? std::make_pair(w, h) : std::make_pair(0, 0);
}
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
QPointer<StyleListModel> OutputListModel::s_adoptedStyleRoster = nullptr;

// The one instance, for C++ AND QML — see the header for why this is eager,
// and why it is the CONSTRUCTED object rather than a private static.
OutputListModel *OutputListModel::instance()
{
    // Whichever object exists first IS the roster: the constructor below
    // registers itself here, so this returns the one QML built when QML got
    // there first (it always does — the singleton is constructed during Main.qml
    // load) instead of building a second roster beside it. The static below is
    // then only the pre-QML bootstrap (a C++ reader that runs before the
    // interface loads), and once QML's object exists the hand-over in
    // adoptEngineInstance points every later reader at it.
    if (s_instance)
        return s_instance;
    static OutputListModel s;
    return &s;
}

// The deliberate half of the rule above (main.cpp calls it after
// loadFromModule): the engine's own object becomes the roster every C++ reader
// resolves, even if a reader built the static here first. Two rosters is the
// shape of "an output I just created never sends NDI until I restart", so the
// join is made once, explicitly, instead of depending on construction order.
void OutputListModel::adoptEngineInstance(OutputListModel *model)
{
    if (!model)
        return;
    if (s_instance == model)
        return;
    s_instance = model;
    qInfo("OutputListModel: adopted the QML engine's roster object %p (rows=%d)",
          static_cast<void *>(model), model->rowCount());
}

OutputListModel *OutputListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    // CppOwnership: this object outlives the QML engine and must never be
    // deleted by it (same contract every service singleton follows).
    OutputListModel *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}


OutputListModel::OutputListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    // TEMPORARY, env-gated — the StyleListModel twin of this line.
    if (qEnvironmentVariableIsSet("VGR_STYLEBG_TEST"))
        fprintf(stderr, "[SELFTEST-STYLEBG-ID] OutputListModel ctor this=%p s_instance=%p\n",
                static_cast<void *>(this), static_cast<void *>(s_instance.data()));
    // A SECOND object is possible and no longer fatal: QML default-constructs
    // the registered singleton, so if a C++ reader built the static here first
    // (or the QML side is constructed after one), this logs it and instance()
    // follows the newest object — the adoption in main.cpp then makes it
    // deliberate. It used to be a silent split: QML edited one roster while
    // every reader resolved the other.
    if (s_instance && s_instance != this)
        qWarning("OutputListModel: a second roster object was constructed (%p, existing %p) — "
                 "instance() now follows the new one; main.cpp adopts the QML-side object explicitly",
                 static_cast<void *>(this), static_cast<void *>(s_instance.data()));
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
        item.mediaFit = so.mediaFit.empty() ? QStringLiteral("contain")
                                            : QString::fromStdString(so.mediaFit);
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

    // HEAL a roster saved before the one-name-per-row rule (or written by the
    // old add path, whose auto name was the row count and could therefore repeat
    // an existing "New Screen N"). Two rows with one name are one output to
    // every reader — one NDI provider, one send clock, one state entry, one
    // frame buffer — so the LATER row could never be fed and nothing could
    // report it. Renamed, never dropped: nothing the operator configured is
    // thrown away, and saveRoster() below persists the repair.
    for (int i = 0; i < m_outputs.size(); ++i) {
        const QString unique = uniqueOutputName(m_outputs.at(i).name, i);
        if (unique == m_outputs.at(i).name)
            continue;
        qWarning("OutputListModel: two outputs were saved as '%s' — renaming this one to '%s' "
                 "(a name is a roster key: two rows sharing it share one NDI sender)",
                 qUtf8Printable(m_outputs.at(i).name), qUtf8Printable(unique));
        m_outputs[i].name = unique;
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
        so.mediaFit = item.mediaFit.toStdString();
        so.onAirOnly = item.onAirOnly;
        for (int i = 0; i < item.content.size() && i < 6; ++i)
            so.contentToggles[i] = item.content.at(i).enabled;
        stored.append(so);
    }
    const auto saveResult = bps::project::OutputStore::Instance().Save(
        std::vector<bps::project::StoredOutput>(stored.cbegin(), stored.cend()));
    int enabledNdi = 0;
    QStringList ndiNames;
    for (const OutputItem &item : m_outputs) {
        if (item.kind == QLatin1String("NDI") && item.isEnabled) {
            ++enabledNdi;
            ndiNames.append(item.name);
        }
    }
    // (`model=` is the object the write landed on. Do NOT resolve instance()
    // here: reloadFromStore() calls this function, and reloadFromStore() runs
    // from the singleton's own constructor — a re-entrant instance() call hits
    // the static-init guard and deadlocks the GUI thread at QML load. That is
    // how a diagnostic line froze the whole startup.)
    qInfo("OutputListModel: roster saved model=%p rows=%d enabledNdi=%d names=[%s] persisted=%d booted=%d",
          static_cast<void *>(this),
          m_outputs.size(), enabledNdi, qUtf8Printable(ndiNames.join(QStringLiteral(", "))),
          saveResult.ok() ? 1 : 0, EngineBridge::instance().booted() ? 1 : 0);
    if (!saveResult.ok())
        qWarning("OutputListModel: saving output roster failed: %s", saveResult.error().message.c_str());

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
    emit outputRosterChanged();
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
    // rendered until the style was reassigned): the per-family template bake
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
    // styleRoster(): the roster Main.qml handed over when it already ran
    // (adoptStyleRoster re-aims this relay itself), else the C++ singleton.
    StyleListModel *styles = styleRoster();
    if (!styles)
        return;
    styleRosterConnected_ = true;
    connect(styles, &StyleListModel::rosterChanged, this, [this]() {
        if (m_outputs.isEmpty())
            return;
        const QModelIndex first = index(0);
        const QModelIndex last = index(m_outputs.size() - 1);
        // StyleBackgroundRole rides along: a Save Changes on a style any
        // output wears (colour or background image) must repaint that
        // output's monitor tile the moment the dialog closes.
        emit dataChanged(first, last, { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole, StyleLayoutRole });
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
    case MediaFitRole: return item.mediaFit;
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
    case StyleLayoutRole:
        return styleLayoutNote(item.styleId);
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
        { MediaFitRole, "mediaFit" },
        { StyleNameRole, "styleName" },
        { OnAirOnlyRole, "onAirOnly" },
        { ContentRole, "content" },
        { StyleBackgroundRole, "styleBackground" },
        { FrameBufferRole, "frameBuffer" },
        { StyleLayoutRole, "styleLayout" },
    };
}

// See the header: one name per row, because the name is the roster's key.
QString OutputListModel::uniqueOutputName(const QString &candidate, int skipIndex) const
{
    auto taken = [this, skipIndex](const QString &name) {
        for (int i = 0; i < m_outputs.size(); ++i)
            if (i != skipIndex && m_outputs.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };

    const QString base = candidate.trimmed();
    if (base.isEmpty() || !taken(base))
        return base;

    // An auto name ("New Screen 7") keeps its FAMILY: take the first free
    // number, so the result reads like a name the app would have chosen, not
    // "New Screen 7 2".
    static const QRegularExpression autoName(
        QStringLiteral("^(New Screen|Output)\\s+(\\d+)$"));
    const QRegularExpressionMatch m = autoName.match(base);
    const QString prefix = m.hasMatch() ? m.captured(1) : QString();
    if (!prefix.isEmpty()) {
        // Continue the family ABOVE this name's own number ("New Screen 4" …
        // "New Screen 8"), so a healed roster still reads in order; only if
        // nothing above is free does it wrap to the low numbers.
        const int own = m.captured(2).toInt();
        for (int pass = 0; pass < 2; ++pass) {
            for (int n = pass == 0 ? own + 1 : 1; n < 100000; ++n) {
                if (pass == 1 && n > own)
                    break;
                const QString next = QStringLiteral("%1 %2").arg(prefix).arg(n);
                if (!taken(next))
                    return next;
            }
        }
        return base;
    }
    for (int n = 2; n < 100000; ++n) {
        const QString next = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(next))
            return next;
    }
    return base;
}

void OutputListModel::addScreen(const QString &name, const QString &type,
                                const QString &resolution, const QString &refresh,
                                const QString &testPattern)
{
    const QString trimmed = name.trimmed();
    const QString res = resolution.trimmed();

    // The auto name is a NUMBER IN A FAMILY, not the row count: a roster of
    // {Main Output, New Screen 2, New Screen 4} used to mint a second
    // "New Screen 4" for the next add, and a duplicated name is a duplicated
    // KEY — the new row then shared the existing one's NDI provider, send
    // clock, state and frame buffer, so it never sent a frame of its own.
    // uniqueOutputName takes the first free number instead.
    const QString wanted = trimmed.isEmpty()
        ? QStringLiteral("New Screen %1").arg(m_outputs.size() + 1)
        : trimmed;

    beginInsertRows(QModelIndex(), m_outputs.size(), m_outputs.size());
    OutputItem item;
    item.name = uniqueOutputName(wanted);
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
    if (trimmed.isEmpty())
        return;
    // A rename must not collide either: two rows with one name are one output
    // to every reader (see uniqueOutputName).
    const QString finalName = uniqueOutputName(trimmed, index);
    if (m_outputs[index].name == finalName)
        return;

    m_outputs[index].name = finalName;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole });
    saveRoster();
}

// THERE IS NO setMediaFit ANY MORE. A take's shape is the PICTURE's (the file's
// own fit, else contain — see SceneBuilder::AddCompositorLayers), so a per-output
// Fill could only ever make two outputs show one file differently. The roster
// field is still loaded and saved (an older roster keeps its value), and nothing
// composes from it.

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
    emit dataChanged(changed, changed, { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole, StyleLayoutRole });
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

StyleListModel *OutputListModel::styleRoster() const
{
    // The roster the UI edits — read through the STATIC hand-over first, not
    // just this object's own copy: QML builds its own OutputListModel too (the
    // engine default-constructs the registered singleton instead of calling
    // create()), so instance() and the object Main.qml hands the roster to are
    // two different objects. A reader addressing the wrong one would fall back
    // to StyleListModel::instance() — a second roster holding styles as they
    // were at boot, which is how a monitor tile came to compose a preview from
    // a stale template/background while the real output rendered the edit.
    if (StyleListModel *adopted = adoptedStyleRoster())
        return adopted;
    // The C++ singleton is only the fallback for a run where QML never handed
    // one over (a headless test, or a read before the hand-over).
    return styleRoster_ ? styleRoster_.data() : StyleListModel::instance();
}

StyleListModel *OutputListModel::adoptedStyleRoster()
{
    return s_adoptedStyleRoster.data();
}

void OutputListModel::styleRosterChanged()
{
    if (m_outputs.isEmpty())
        return;
    // StyleBackgroundRole rides along: a Save Changes on a style any output
    // wears (colour or background image) must repaint that output's monitor
    // tile the moment the dialog closes.
    emit dataChanged(index(0), index(m_outputs.size() - 1),
                     { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole, StyleLayoutRole });
    // An edit to a style that is ON AIR must reach the engine NOW: the live
    // render loop picks the new spec up on its next frame.
    pushActiveEngineStyle();
    emit activeStyleChanged();
}

void OutputListModel::adoptStyleRoster(StyleListModel *styles)
{
    // TEMPORARY, env-gated: which object QML actually hands over, next to the
    // C++ singleton, so a mismatch (or a null hand-over) is visible instead of
    // guessed at.
    if (qEnvironmentVariableIsSet("VGR_STYLEBG_TEST"))
        fprintf(stderr, "[SELFTEST-STYLEBG-ID] adopt(%p) cpp=%p same=%d\n",
                static_cast<void *>(styles),
                static_cast<void *>(StyleListModel::instance()),
                styles == StyleListModel::instance() ? 1 : 0);
    // FIRST, before the early return below: every OTHER OutputListModel object
    // (the C++ static instance() among them) resolves styles through this, so
    // the hand-over has to be visible beyond the object QML happened to call.
    s_adoptedStyleRoster = styles;
    if (styles && styleRoster_ == styles)
        return;
    if (styleRosterConn_)
        QObject::disconnect(styleRosterConn_);
    styleRoster_ = styles;
    if (styles) {
        styleRosterConn_ = connect(styles, &StyleListModel::rosterChanged, this,
                                   [this]() { styleRosterChanged(); });
    }
    // Re-resolve everything the roster feeds — the tiles' style roles and the
    // engine's spec — against the roster that is now authoritative. This is the
    // half that makes an in-session style edit visible without a restart.
    styleRosterChanged();
}

QString OutputListModel::styleLayoutNote(const QString &styleId) const
{
    const int row = styleRowForId(styleId);
    if (row < 0)
        return {};   // no style on this output: there is no layout to describe
    // THE SPEC THIS OUTPUT ACTUALLY PUSHES, not a re-reading of the style map:
    // its per-family template keys, its baked blocks and its content type are
    // exactly what the engine composes with, so the answer below is the
    // engine's own.
    const bps::presentation::OutputStyleSpec spec =
        StyleSpecBuilder::specForStyleRow(row, styleRoster());
    switch (bps::presentation::SlideRules::LayoutScope(spec)) {
    case bps::presentation::SlideRules::StyleLayoutScope::Shows:
        return QStringLiteral("Lays out shows too — the Shows template replaces a show\u2019s own layout");
    case bps::presentation::SlideRules::StyleLayoutScope::ContentFamilies:
        return QStringLiteral("Restyles Scripture and The Table — a show keeps its own layout");
    case bps::presentation::SlideRules::StyleLayoutScope::None:
        break;
    }
    return QStringLiteral("No layout of its own — background and placement only");
}

int OutputListModel::styleRowForId(const QString &styleId) const
{
    const StyleListModel *styles = styleRoster();
    if (!styles || styleId.isEmpty())
        return -1;
    return styles->rowForId(styleId);
}

bool OutputListModel::activeStyleAllows(const QString &contentType) const
{
    const QString styleId = activeStyleId();
    if (styleId.isEmpty() || styleRowForId(styleId) < 0)
        return true;   // no style on the active output: everything is allowed
    // THE GATE'S ANSWER IS THE ENGINE'S, not a second copy of the rule kept
    // here. This used to test the style's own show* boolean and answer
    // differently from SlideRules::StyleAdmits in the one state where the two
    // could part: ALL FOUR GATES OFF is "unconfigured, admit everything" to the
    // engine, but read as four refusals here — so the reference pane warned
    // "switched off in this output's style" and never sent the verse, while the
    // engine would have composed it (the same slide, two answers, one click
    // apart). The spec builder is the other half: a style's gates reach the
    // engine through THAT composition, so asking it here means the chip, the
    // warning and the frame cannot disagree.
    const auto spec = StyleSpecBuilder::specForStyleId(styleId);
    return bps::presentation::SlideRules::StyleAdmitsFamily(spec, contentType.toStdString());
}

QString OutputListModel::activeStyleTemplateId(const QString &contentType) const
{
    if (contentType.isEmpty())
        return {};
    const QString styleId = activeStyleId();
    if (styleId.isEmpty() || styleRowForId(styleId) < 0)
        return {};   // no style on the active output: the slides' own template governs
    // THE SPEC'S BAKED PICK — the same value SceneBuilder lays the family out
    // through (StyleSpecBuilder bakes a family's slot from the style row, and
    // the engine reads `familyTemplateKeys`). Reading the row's property here
    // was a second resolution of the same pick: it could not see the fallbacks
    // the bake applies, so the pane could name a template the output would not
    // render.
    const auto spec = StyleSpecBuilder::specForStyleId(styleId);
    const std::size_t slot =
        bps::presentation::SlideRules::FamilyTemplateSlot(contentType.toStdString());
    if (slot >= bps::presentation::kFamilySlotCount)
        return {};
    return QString::fromStdString(spec.familyTemplateKeys[slot]);
}

QVariantMap OutputListModel::styleBackground(int index) const
{
    const int row = styleRowForId(index >= 0 && index < m_outputs.size()
                                      ? m_outputs.at(index).styleId : QString());
    if (row < 0)
        return { { "color", QStringLiteral("transparent") },
                 { "image", QString() }, { "hasImage", false },
                 { "clearOnText", false } };
    const QVariantMap style = styleRoster()->getStyle(row);
    const QString image = style.value(QStringLiteral("backgroundImage")).toString();
    // `hasImage` is the roster's CACHED answer (it stats the file where the path
    // is loaded or changed — StyleListModel::reloadFromStore /
    // setBackgroundImage). This getter used to call QFile::exists() itself, which
    // put a filesystem stat on the interface thread behind every read of this
    // role.
    // `clearOnText` (freeShow's clearStyleBackgroundOnText) is reported here as
    // the MODEL reads it, beside the colour and the image. WHAT A SURFACE PAINTS
    // is no longer this role: the tile and the output window take the style's
    // facts — colour, image, and the step-aside the engine actually applies for
    // the slide on air — from the composition (LiveOutputService.
    // previewWithStyle -> SceneBuilder::ComposeSlide), so the image can no
    // longer be stepped aside on one surface and baked on the other. This role
    // is the baseline the self-test logs against the composed picture.
    return { { "color", style.value(QStringLiteral("backgroundColor")).toString() },
             { "image", image },
             { "hasImage", style.value(QStringLiteral("hasImage")).toBool() },
             { "clearOnText", style.value(QStringLiteral("clearBackgroundOnText")).toBool() } };
}

QString OutputListModel::styleIdAt(int row) const
{
    const StyleListModel *styles = styleRoster();
    if (!styles || row < 0 || row >= styles->rowCount())
        return QString();
    return styles->data(styles->index(row, 0), StyleListModel::IdRole).toString();
}

QString OutputListModel::styleNameAt(int row) const
{
    const StyleListModel *styles = styleRoster();
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
    // OutputStyleSpec): background + layout preset + the clear-on-text flag +
    // the per-family template blocks the engine's family branch gates on.
    // An empty/unknown styleId pushes an empty spec — the engine renders
    // unstyled. THE COMPOSITION IS NOT HERE (StyleSpecBuilder): the preview
    // composes the same spec from the same function, so a style cannot render
    // one way on an output and another way on a monitor tile.
    bps::presentation::OutputStyleSpec spec =
        StyleSpecBuilder::specForStyleRow(styleRowForId(styleId), styleRoster());
    if (SettingsService *settings = SettingsService::instancePtr())
        settings->setActiveOutputStyle(spec);
    // THE OTHER LIVE OUTPUTS: push the per-output style set so the engine's
    // loop renders one gated pass per style-wearing live output. EVERY live
    // output is included (the active one too — its entry is the same spec,
    // the loop renders it into its own named buffer for the tile); outputs
    // without a style push nothing (their tile mirrors the main pass).
    std::vector<bps::presentation::OutputStyleSpec> specs;
    std::vector<std::string> buffers;
    std::vector<std::pair<int, int>> sizes;
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
                bps::presentation::OutputStyleSpec outSpec =
                    StyleSpecBuilder::specForStyleRow(styleRow, styleRoster());
                // (NO mediaFit here: how a taken file sits is the FILE's own
                // choice, else contain — one picture, one shape on every output.
                // A per-output fill is what made the wall show one take two
                // ways, and the field it read could not even be corrected on the
                // active output: PresentationEngine::SetActiveOutputStyle did
                // not compare mediaFit, so the change was swallowed as a no-op.)
                // Buffer name keyed by output identity (qHash(name) — the same
                // keying saveRoster uses for StoredOutput ids).
                specs.push_back(std::move(outSpec));
                buffers.push_back(QStringLiteral("__out_%1__").arg(qHash(item.name)).toStdString());
                sizes.push_back(parseResolution(item.res));
            }
        }
    }
    if (SettingsService *settings2 = SettingsService::instancePtr())
        settings2->setLiveOutputStyles(specs, buffers, sizes);
    qInfo("OutputListModel: pushed engine output styles specs=%d buffers=%d activeStyle='%s' booted=%d",
          static_cast<int>(specs.size()), static_cast<int>(buffers.size()),
          qUtf8Printable(styleId), EngineBridge::instance().booted() ? 1 : 0);
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
        // Local monitor previews re-compose staged content from the selected
        // output's style template when this revision changes too.
        emit activeStyleChanged();
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
        emit dataChanged(changed, changed, { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole, StyleLayoutRole });
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
        { "mediaFit", item.mediaFit },
        { "onAirOnly", item.onAirOnly },
        { "styleName", styleName },
        { "content", content },
    };
}
