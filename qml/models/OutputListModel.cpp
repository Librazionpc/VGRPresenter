#include "OutputListModel.h"
#include "BusListModel.h"
#include "FamilySlots.h"
#include "StyleListModel.h"
#include "services/DesignLibraryService.h"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/LiveOutputService.h"
#include "services/RoutingService.h"
#include "services/SettingsService.h"
#include "services/ShowConverter.h"
#include "services/StyleSpecBuilder.h"
#include "modules/presentation/PresentationTypes.hpp"
#include "modules/presentation/CompositorState.hpp"
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
constexpr auto kMainOutputName = "Main Output";
// The Edit dialog's item-kind keys, in OutputContentToggle display order —
// the same order the engine's OutputStore persists the toggles in.
constexpr const char *kContentKeys[] = { "text", "camera", "media", "clock", "timer", "shape" };

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
    if (s_instance)
        return s_instance;
    static OutputListModel s;
    return &s;
}

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
    if (qEnvironmentVariableIsSet("VGR_STYLEBG_TEST")) {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StyleBgId"),
                            QStringLiteral("OutputListModel ctor this=%1 s_instance=%2")
                                .arg(quintptr(this)).arg(quintptr(s_instance.data())));
    }
    if (s_instance && s_instance != this)
        qWarning("OutputListModel: a second roster object was constructed (%p, existing %p) — "
                 "instance() now follows the new one; main.cpp adopts the QML-side object explicitly",
                 static_cast<void *>(this), static_cast<void *>(s_instance.data()));
    s_instance = this;

    reloadFromStore();

    connectToStyleRoster();
    connectToTemplateLibrary();
    connectToEngineBoot();
    QTimer::singleShot(0, this, [this]() {
        connectToStyleRoster();
        connectToTemplateLibrary();
        connectToEngineBoot();
    });

    // ---- Monitor hot-plug (found live: plugging an HDMI monitor while the
    trackScreens();
}

void OutputListModel::reloadFromStore()
{
    if (EngineBridge::instance().booted())
        rosterReady_ = true;

    beginResetModel();
    m_outputs.clear();

    const QList<OutputContentToggle> content = defaultContent();

    // ---- HYDRATE the persisted roster (the engine's OutputStore, the
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary && !screens.isEmpty())
        primary = screens.first();

    bool sawActive = false;
    for (const bps::project::StoredOutput &so : bps::project::OutputStore::Instance().Get()) {
        OutputItem item;
        item.id = QString::fromStdString(so.id);
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
        item.videoMode = so.videoMode == "compose" ? QStringLiteral("compose")
                                                    : QStringLiteral("direct");
        // This row's own gates (absent in an older roster = on, the default).
        item.showOverlay = so.showOverlay;
        item.showMedia = so.showMedia;
        item.mediaFit = so.mediaFit.empty() ? QStringLiteral("contain")
                                            : QString::fromStdString(so.mediaFit);
        // "" = the program stream (the default, and what every older row is).
        item.audioDevice = QString::fromStdString(so.audioDevice);
        item.presetId = QString::fromStdString(so.presetId);
        item.onAirOnly = so.onAirOnly || item.kind == QLatin1String("NDI");
        if (item.onAirOnly && !item.screenName.isEmpty()) {
            item.screenName.clear();
            item.res.clear();
            item.refresh.clear();
        }
        item.content = content;   // the six defaults, then the persisted toggles on top
        for (int i = 0; i < item.content.size() && i < 6; ++i)
            item.content[i].enabled = so.contentToggles[i];
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
        // A ROSTER SAVED BEFORE "ONE SCREEN, ONE OUTPUT" can carry two rows on the same display — the
        // state that opened two windows at one x/y and drew one picture over the other. The first claim
        // keeps the screen; a later row is released here instead of fighting it every session.
        if (!item.screenName.isEmpty() && rowHoldingScreen(item.screenName, -1) >= 0) {
            qInfo("OutputListModel: '%s' released screen '%s' (already an output's own)",
                  qUtf8Printable(item.name), qUtf8Printable(item.screenName));
            item.screenName.clear();
            item.res.clear();
            item.refresh.clear();
        }
        sawActive = sawActive || item.active;
        m_outputs.append(item);
    }

    for (int i = 0; i < m_outputs.size(); ++i) {
        const QString unique = uniqueOutputName(m_outputs.at(i).name, i);
        if (unique == m_outputs.at(i).name)
            continue;
        qWarning("OutputListModel: two outputs were saved as '%s' — renaming this one to '%s' "
                 "(a name is a roster key: two rows sharing it share one NDI sender)",
                 qUtf8Printable(m_outputs.at(i).name), qUtf8Printable(unique));
        m_outputs[i].name = unique;
    }

    QStringList seenIds;
    int healedId = 0;
    for (int i = 0; i < m_outputs.size(); ++i) {
        const QString id = m_outputs.at(i).id.trimmed();
        if (!id.isEmpty() && !seenIds.contains(id)) {
            seenIds.append(id);
            continue;
        }
        // nextStableId scans the WHOLE roster, so it cannot collide with a row
        // this loop has not healed yet either.
        m_outputs[i].id = nextStableId();
        seenIds.append(m_outputs[i].id);
        ++healedId;
    }
    if (healedId > 0)
        qInfo("OutputListModel: healed %d output row id(s) — a row without a stable id cannot own a routing wire",
              healedId);

    if (m_outputs.isEmpty()) {
        OutputItem main;
        main.id = nextStableId();
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
        so.id = item.id.toStdString();
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
        so.videoMode = item.videoMode.toStdString();
        so.showOverlay = item.showOverlay;
        so.showMedia = item.showMedia;
        so.mediaFit = item.mediaFit.toStdString();
        so.audioDevice = item.audioDevice.toStdString();
        so.presetId = item.presetId.toStdString();
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
    qInfo("OutputListModel: roster saved model=%p rows=%d enabledNdi=%d names=[%s] persisted=%d booted=%d",
          static_cast<void *>(this),
          m_outputs.size(), enabledNdi, qUtf8Printable(ndiNames.join(QStringLiteral(", "))),
          saveResult.ok() ? 1 : 0, EngineBridge::instance().booted() ? 1 : 0);
    if (!saveResult.ok())
        qWarning("OutputListModel: saving output roster failed: %s", saveResult.error().message.c_str());

    pushEngineStyle(activeStyleId());
    emit outputRosterChanged();
}

void OutputListModel::connectToEngineBoot()
{
    if (engineBootConnected_)
        return;
    engineBootConnected_ = true;
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this]() {
        if (EngineBridge::instance().booted()) {
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
    StyleListModel *styles = styleRoster();
    if (!styles)
        return;
    styleRosterConnected_ = true;
    connect(styles, &StyleListModel::rosterChanged, this, [this]() {
        if (m_outputs.isEmpty())
            return;
        const QModelIndex first = index(0);
        const QModelIndex last = index(m_outputs.size() - 1);
        emit dataChanged(first, last, { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole, StyleLayoutRole });
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
    case IdRole: return item.id;
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
    case AudioDeviceRole: return item.audioDevice;
    case PresetIdRole: return item.presetId;
    case VideoModeRole: return item.videoMode;
    case OverlayGateRole: return item.showOverlay;
    case MediaGateRole: return item.showMedia;
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
        return hasOwnPass(index.row())
                   ? QStringLiteral("__out_%1__").arg(qHash(item.name))
                   : QString();
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
        { AudioDeviceRole, "audioDevice" },
        { PresetIdRole, "presetId" },
        { VideoModeRole, "videoMode" },
        { OverlayGateRole, "showOverlay" },
        { MediaGateRole, "showMedia" },
        { StyleNameRole, "styleName" },
        { OnAirOnlyRole, "onAirOnly" },
        { ContentRole, "content" },
        { StyleBackgroundRole, "styleBackground" },
        { FrameBufferRole, "frameBuffer" },
        { StyleLayoutRole, "styleLayout" },
        { IdRole, "outputId" },
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

    static const QRegularExpression autoName(
        QStringLiteral("^(New Screen|Output)\\s+(\\d+)$"));
    const QRegularExpressionMatch m = autoName.match(base);
    const QString prefix = m.hasMatch() ? m.captured(1) : QString();
    if (!prefix.isEmpty()) {
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

    const QString wanted = trimmed.isEmpty()
        ? QStringLiteral("New Screen %1").arg(m_outputs.size() + 1)
        : trimmed;

    beginInsertRows(QModelIndex(), m_outputs.size(), m_outputs.size());
    OutputItem item;
    item.id = nextStableId();
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
    // An HDMI row is a window on a PHYSICAL SCREEN, and two windows on one screen draw over each other.
    // A new row takes the first screen no other row holds, so a second HDMI output is a second picture.
    if (!item.onAirOnly)
        item.screenName = freeScreenName(-1, QString());
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
    copy.id = nextStableId();   // a copy is a NEW output — never the same identity as its source
    copy.name = QStringLiteral("%1 (copy)").arg(copy.name);
    copy.badge = QStringLiteral("OUT %1").arg(m_outputs.size() + 1);
    copy.active = false; // a duplicate starts dark, never fighting the original
    copy.isEnabled = true; // a duplicate always starts usable
    // A DUPLICATE IS A SECOND OUTPUT, NOT A SECOND WINDOW ON THE SAME SCREEN. It used to inherit the
    // source's screenName, so both windows opened at the same x/y and size and their pictures drew over
    // each other ("Main Output" and "Main Output 2" showing one HDMI). A copy takes the first screen
    // nothing else claims, or none at all — the operator assigns one in Settings · Outputs.
    copy.screenName = copy.kind == QLatin1String("NDI") ? QString()
                                                        : freeScreenName(-1, QString());
    if (copy.screenName.isEmpty()) {
        copy.res.clear();
        copy.refresh.clear();
    }
    beginInsertRows(QModelIndex(), index + 1, index + 1);
    m_outputs.insert(index + 1, copy);
    endInsertRows();
    saveRoster();
}

void OutputListModel::removeOutput(int index)
{
    if (index < 0 || index >= m_outputs.size())
        return;

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
    pushEngineStyle(m_outputs[index].styleId);
    emit activeStyleChanged();
}

bool OutputListModel::setEnabled(int index, bool on)
{
    if (index < 0 || index >= m_outputs.size())
        return false;
    if (m_outputs[index].isEnabled == on)
        return true;

    if (!on) {
        int enabledCount = 0;
        for (const OutputItem &item : m_outputs)
            if (item.isEnabled)
                ++enabledCount;
        if (enabledCount <= 1)
            return false;
    }

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
    return true;
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
    connect(screen, &QScreen::geometryChanged, this, [this](const QRect &) {
        screensChangedNow();
    });
    connect(screen, &QScreen::refreshRateChanged, this, [this](qreal) {
        screensChangedNow();
    });
}

void OutputListModel::screensChangedNow()
{
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

QRect OutputListModel::screenRect(const QString &screenName) const
{
    if (screenName.isEmpty() || !qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return {};
    for (const QScreen *screen : QGuiApplication::screens())
        if (screen && screen->name() == screenName)
            return screen->geometry();
    return {};
}

// A display is HELD by a row that names it, or names a screen with the very same geometry (a mirrored monitor is two
// screens in one place — see mirroredBy). Either way a second window there would draw over the first.
int OutputListModel::rowHoldingScreen(const QString &screenName, int exceptRow) const
{
    if (screenName.isEmpty())
        return -1;
    const QRect wanted = screenRect(screenName);
    for (int i = 0; i < m_outputs.size(); ++i) {
        if (i == exceptRow)
            continue;
        const QString held = m_outputs.at(i).screenName;
        if (held == screenName)
            return i;
        if (!wanted.isNull() && !held.isEmpty() && screenRect(held) == wanted)
            return i;
    }
    return -1;
}

int OutputListModel::mirroredBy(int index) const
{
    if (index < 0 || index >= m_outputs.size())
        return -1;
    const OutputItem &me = m_outputs.at(index);
    if (me.onAirOnly || me.screenName.isEmpty())
        return -1;
    const QRect mine = screenRect(me.screenName);
    if (mine.isNull())
        return -1;
    for (int i = 0; i < index; ++i) {
        const OutputItem &other = m_outputs.at(i);
        if (other.onAirOnly || !other.isEnabled || other.screenName.isEmpty())
            continue;
        if (screenRect(other.screenName) == mine)
            return i;
    }
    return -1;
}

QString OutputListModel::freeScreenName(int exceptRow, const QString &preferred) const
{
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return QString();   // no GUI: there are no screens to place a window on
    if (!preferred.isEmpty() && rowHoldingScreen(preferred, exceptRow) < 0)
        return preferred;
    for (const QScreen *screen : QGuiApplication::screens()) {
        if (screen && rowHoldingScreen(screen->name(), exceptRow) < 0)
            return screen->name();
    }
    return QString();   // every screen is already an output: no window at all beats two on one screen
}

void OutputListModel::setScreenName(int index, const QString &screenName)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (!screenName.isEmpty() && m_outputs[index].onAirOnly)
        return;
    if (m_outputs[index].screenName == screenName)
        return;

    // ONE SCREEN, ONE OUTPUT: assigning a display MOVES it to this output. Without this, two rows could
    // hold the same screenName and both windows opened fullscreen at the same x/y — one picture drawn
    // over the other, which reads as "both outputs are going to the same HDMI".
    if (const int holder = rowHoldingScreen(screenName, index); holder >= 0) {
        const QString holderName = m_outputs.at(holder).name;
        m_outputs[holder].screenName.clear();
        m_outputs[holder].res.clear();
        m_outputs[holder].refresh.clear();
        m_outputs[holder].active = false;   // a window with no screen is not on air
        const QModelIndex released = this->index(holder);
        emit dataChanged(released, released,
                         { ScreenNameRole, ResRole, RefreshRole, ActiveRole });
        qInfo("OutputListModel: screen '%s' moved from '%s' to '%s' — a display shows one output",
              qUtf8Printable(screenName), qUtf8Printable(holderName),
              qUtf8Printable(m_outputs[index].name));
    }

    m_outputs[index].screenName = screenName;

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

void OutputListModel::setAudioDevice(int index, const QString &deviceId)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].audioDevice == deviceId)
        return;

    m_outputs[index].audioDevice = deviceId;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { AudioDeviceRole });
    saveRoster();
    if (RoutingService *routing = RoutingService::instance())
        routing->refresh();
}

void OutputListModel::setVideoMode(int index, const QString &mode)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    const QString normalized = mode == QStringLiteral("compose")
        ? QStringLiteral("compose") : QStringLiteral("direct");
    if (m_outputs[index].videoMode == normalized)
        return;

    m_outputs[index].videoMode = normalized;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { VideoModeRole, FrameBufferRole });
    saveRoster();
    pushActiveEngineStyle();
    emit activeStyleChanged();
}

void OutputListModel::setShowOverlay(int index, bool on)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].showOverlay == on)
        return;
    m_outputs[index].showOverlay = on;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { OverlayGateRole });
    saveRoster();
    pushActiveEngineStyle();
    if (RoutingService *routing = RoutingService::instance())
        routing->refresh();
}

void OutputListModel::setShowMedia(int index, bool on)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].showMedia == on)
        return;
    m_outputs[index].showMedia = on;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { MediaGateRole });
    saveRoster();
    pushActiveEngineStyle();
    // Same contract as setShowOverlay: the flag IS the wire, so the canvas is told.
    if (RoutingService *routing = RoutingService::instance())
        routing->refresh();
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
    styleLayoutCache_.clear();
    if (m_outputs.isEmpty())
        return;
    emit dataChanged(index(0), index(m_outputs.size() - 1),
                     { StyleIdRole, StyleNameRole, StyleBackgroundRole, FrameBufferRole, StyleLayoutRole });
    // An edit to a style that is ON AIR must reach the engine NOW: the live
    // render loop picks the new spec up on its next frame.
    pushActiveEngineStyle();
    emit activeStyleChanged();
}

void OutputListModel::adoptStyleRoster(StyleListModel *styles)
{
    if (qEnvironmentVariableIsSet("VGR_STYLEBG_TEST")) {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StyleBgId"),
                            QStringLiteral("adopt(%1) cpp=%2 same=%3")
                                .arg(quintptr(styles))
                                .arg(quintptr(StyleListModel::instance()))
                                .arg(styles == StyleListModel::instance() ? 1 : 0));
    }
    s_adoptedStyleRoster = styles;
    styleLayoutCache_.clear();
    if (styles && styleRoster_ == styles)
        return;
    if (styleRosterConn_)
        QObject::disconnect(styleRosterConn_);
    styleRoster_ = styles;
    if (styles) {
        styleRosterConn_ = connect(styles, &StyleListModel::rosterChanged, this,
                                   [this]() { styleRosterChanged(); });
    }
    styleRosterChanged();
}

QString OutputListModel::styleLayoutNote(const QString &styleId) const
{
    const int row = styleRowForId(styleId);
    if (row < 0)
        return {};   // no style on this output: there is no layout to describe
    if (const auto hit = styleLayoutCache_.constFind(styleId); hit != styleLayoutCache_.constEnd())
        return hit.value();
    const QString note = styleLayoutNoteUncached(row);
    styleLayoutCache_.insert(styleId, note);
    return note;
}

QString OutputListModel::styleLayoutNoteUncached(int row) const
{
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
    const auto spec = StyleSpecBuilder::specForStyleId(styleId);
    const std::size_t slot =
        bps::presentation::SlideRules::FamilyTemplateSlot(contentType.toStdString());
    if (slot >= bps::presentation::kFamilySlotCount)
        return {};
    return QString::fromStdString(spec.familyTemplateKeys[slot]);
}

QString OutputListModel::activeStyleShowCategoryTemplateId(const QString &category) const
{
    const QString wanted = category.trimmed();
    if (wanted.isEmpty())
        return {};
    const QString styleId = activeStyleId();
    if (styleId.isEmpty() || styleRowForId(styleId) < 0)
        return {};
    const auto spec = StyleSpecBuilder::specForStyleId(styleId);
    for (const auto& entry : spec.showCategoryTemplates)
        if (entry.overrideEnabled && entry.category == wanted && !entry.blocks.empty())
            return QString::fromStdString(entry.templateKey);
    return {};
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

void OutputListModel::pushEngineStyle(const QString &styleId, const QString &slideTag)
{
    bps::presentation::OutputStyleSpec spec =
        StyleSpecBuilder::specForStyleRow(styleRowForId(styleId), styleRoster(), slideTag);
    spec.gateVideoBuses = true;
    spec.allowedVideoBuses.clear();
    // ...and Scroll's solved type size for the active style (0 outside Scroll), so the program
    // frame the unstyled outputs mirror is drawn at the same size the surfaces use.
    if (!styleId.isEmpty()) {
        spec.readingFontPx = static_cast<float>(
            LiveOutputService::instance().readingColumnPxFor(styleId));
        spec.readingStyleKey = styleId.toStdString();
    }
    const int activeRow = activeIndex();
    if (activeRow >= 0 && activeRow < m_outputs.size()) {
        spec.showOverlay = spec.showOverlay && m_outputs.at(activeRow).showOverlay;
        spec.showMedia = spec.showMedia && m_outputs.at(activeRow).showMedia;
    }
    std::vector<bps::presentation::OutputStyleSpec> specs;
    std::vector<std::string> buffers;
    std::vector<std::pair<int, int>> sizes;
    for (int i = 0; i < m_outputs.size(); ++i) {
        const OutputItem &item = m_outputs.at(i);
        if (!hasOwnPass(i))
            continue;
        const int styleRow = item.styleId.isEmpty() ? -1 : styleRowForId(item.styleId);
        bps::presentation::OutputStyleSpec outSpec = styleRow >= 0
            ? StyleSpecBuilder::specForStyleRow(styleRow, styleRoster(), slideTag)
            : bps::presentation::OutputStyleSpec{};
        // SCROLL'S TYPE SIZE FOR THIS ROW'S STYLE (0 outside Scroll): the engine's own scene for
        // this output wraps and sizes the passage exactly as the monitor surface does.
        if (styleRow >= 0) {
            outSpec.readingFontPx = static_cast<float>(
                LiveOutputService::instance().readingColumnPxFor(item.styleId));
            // ...and the key its line breaks travel under on the slide (Slide::flowBreaks).
            outSpec.readingStyleKey = item.styleId.toStdString();
        }
        // Per-output routing must not replace or filter the on-air program.
        // Match the monitor's program pass: explicit bus blocks on the slide
        // remain governed by the active program style.
        outSpec.gateVideoBuses = spec.gateVideoBuses;
        outSpec.allowedVideoBuses.clear();
        outSpec.showOverlay = outSpec.showOverlay && item.showOverlay;
        outSpec.showMedia = outSpec.showMedia && item.showMedia;
        specs.push_back(std::move(outSpec));
        buffers.push_back(QStringLiteral("__out_%1__").arg(qHash(item.name)).toStdString());
        sizes.push_back(parseResolution(item.res));
    }
    // THE SAME PICTURE AGAIN IS NOT A PUSH: the engine rebuilds scenes on a style change, so an identical
    // spec set (the common case in a burst) must not reach it. (1153 pushes in one session, mostly repeats.)
    const bool bootedNow = EngineBridge::instance().booted();
    if (m_hasPushed && bootedNow == m_lastBooted && spec == m_lastActiveSpec && specs == m_lastSpecs
            && buffers == m_lastBuffers
            && sizes == m_lastSizes)
        return;
    if (SettingsService *settings = SettingsService::instancePtr())
        settings->setActiveOutputStyle(spec);
    if (SettingsService *settings2 = SettingsService::instancePtr())
        settings2->setLiveOutputStyles(specs, buffers, sizes);
    m_hasPushed = true;
    m_lastBooted = bootedNow;
    m_lastActiveSpec = spec;
    m_lastSpecs = specs;
    m_lastBuffers = buffers;
    m_lastSizes = sizes;
    qInfo("OutputListModel: pushed engine output styles specs=%d buffers=%d activeStyle='%s' booted=%d",
          static_cast<int>(specs.size()), static_cast<int>(buffers.size()),
          qUtf8Printable(styleId), EngineBridge::instance().booted() ? 1 : 0);
}

void OutputListModel::pushActiveEngineStyle()
{
    if (!styleRosterConnected_)
        return;
    // A burst of refreshes (a column seat per tile, roster and route changes) is ONE push, next turn.
    if (m_pushQueued)
        return;
    m_pushQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_pushQueued = false;
        if (styleRosterConnected_)
            pushEngineStyle(activeStyleId());
    });
}

QString OutputListModel::frameBufferFor(int row) const
{
    if (!hasOwnPass(row))
        return QString();
    return QStringLiteral("__out_%1__").arg(qHash(m_outputs.at(row).name));
}

int OutputListModel::routedVideoBus(int row) const
{
    return row >= 0 && row < routedVideoBuses_.size() ? routedVideoBuses_.at(row) : 0;
}

bool OutputListModel::hasOwnPass(int row) const
{
    if (row < 0 || row >= m_outputs.size())
        return false;
    const OutputItem &item = m_outputs.at(row);
    if (!item.isEnabled)
        return false;   // not on air: the live loop skips it
    if (!item.styleId.isEmpty() && styleRowForId(item.styleId) >= 0)
        return true;
    if (item.videoMode == QStringLiteral("compose"))
        return true;
    if (item.kind == QLatin1String("REC") || item.kind == QLatin1String("STREAM"))
        return true;
    // Keep a per-output program pass for routed rows too; the route does not
    // choose the picture sent live.
    return (row < routedVideoBuses_.size() ? routedVideoBuses_.at(row) : 0) > 0;
}

void OutputListModel::setRoutedVideoBuses(const QVector<int> &busNumberByRow,
                                          const QVector<QVector<int>> &wiredByRow)
{
    if (routedVideoBuses_ == busNumberByRow && wiredVideoBuses_ == wiredByRow)
        return;
    routedVideoBuses_ = busNumberByRow;
    wiredVideoBuses_ = wiredByRow;
    if (styleRosterConnected_)
        pushActiveEngineStyle();
}

void OutputListModel::repushActiveStyleForTag(const QString &tag)
{
    if (!styleRosterConnected_)
        return;
    pushEngineStyle(activeStyleId(), tag);
}

void OutputListModel::connectToTemplateLibrary()
{
    if (templateLibraryConnected_)
        return;
    TemplateLibraryService *templates = &TemplateLibraryService::instance();
    if (!templates)
        return;
    templateLibraryConnected_ = true;
    connect(templates, &DesignLibraryService::changed, this, [this]() {
        pushActiveEngineStyle();
        // Local monitor previews re-compose staged content from the selected
        // output's style template when this revision changes too.
        emit activeStyleChanged();
    });
}

void OutputListModel::setPresetId(int index, const QString &presetId)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].presetId == presetId)
        return;

    m_outputs[index].presetId = presetId;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { PresetIdRole });
    saveRoster();
    if (RoutingService *routing = RoutingService::instance())
        routing->refresh();
}

int OutputListModel::presetUsage(const QString &presetId) const
{
    if (presetId.isEmpty())
        return 0;
    int n = 0;
    for (const OutputItem &item : m_outputs)
        if (item.presetId == presetId)
            ++n;
    return n;
}

void OutputListModel::detachPresetEverywhere(const QString &presetId)
{
    if (presetId.isEmpty())
        return;
    bool touched = false;
    for (int i = 0; i < m_outputs.size(); ++i) {
        if (m_outputs[i].presetId != presetId)
            continue;
        m_outputs[i].presetId.clear();
        const QModelIndex changed = index(i);
        emit dataChanged(changed, changed, { PresetIdRole });
        touched = true;
    }
    if (touched)
        saveRoster();
}

void OutputListModel::presetChanged(const QString &presetId)
{
    if (presetId.isEmpty())
        return;
    for (int i = 0; i < m_outputs.size(); ++i) {
        if (m_outputs[i].presetId != presetId)
            continue;
        const QModelIndex changed = index(i);
        emit dataChanged(changed, changed, { PresetIdRole });
    }
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
        { "id", item.id },
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
        { "videoMode", item.videoMode },
        { "showOverlay", item.showOverlay },
        { "showMedia", item.showMedia },
        { "mediaFit", item.mediaFit },
        { "audioDevice", item.audioDevice },
        { "presetId", item.presetId },
        { "onAirOnly", item.onAirOnly },
        { "styleName", styleName },
        // What the style does to a slide (the same line the StyleLayoutRole carries), for readers of the map.
        { "styleLayout", styleLayoutNote(item.styleId) },
        { "content", content },
    };
}

// ---------------------------------------------------------------------------
// Stable identity (see OutputItem::id)
// ---------------------------------------------------------------------------

QString OutputListModel::nextStableId() const
{
    qulonglong highest = 0;
    for (const OutputItem &item : m_outputs) {
        if (!item.id.startsWith(QLatin1String("out-")))
            continue;
        bool ok = false;
        const qulonglong n = item.id.mid(4).toULongLong(&ok);
        if (ok && n > highest)
            highest = n;
    }
    return QStringLiteral("out-%1").arg(highest + 1);
}

QString OutputListModel::stableIdFor(int row) const
{
    if (row < 0 || row >= m_outputs.size())
        return {};
    return m_outputs.at(row).id;
}

int OutputListModel::rowForStableId(const QString &id) const
{
    if (id.isEmpty())
        return -1;
    for (int i = 0; i < m_outputs.size(); ++i)
        if (m_outputs.at(i).id == id)
            return i;
    return -1;
}