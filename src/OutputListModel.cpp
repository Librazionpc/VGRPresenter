#include "OutputListModel.h"
#include "StyleListModel.h"

#include <QGuiApplication>
#include <QScreen>
#include <algorithm>

namespace {
// The one output every show needs — deleting it would leave the app with no
// way to put anything on the primary display, so it's protected everywhere
// (Settings · Outputs hides its Delete affordance; removeOutput refuses).
constexpr auto kMainOutputName = "Main Output";
}


OutputListModel::OutputListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    const QList<OutputContentToggle> content = defaultContent();

    // The ONE mandatory output: the primary display — every show needs a
    // way to put content on the main screen. Everything else (stage,
    // nursery, stream overlays…) is user-added; no more hardcoded demo
    // roster. Its res/refresh read from the actual primary screen so the
    // card shows the machine's real mode, not a mock 1920×1080@60.
    const QList<QScreen *> screens = QGuiApplication::screens();
    const QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary && !screens.isEmpty())
        primary = screens.first();

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
    main.active = true;
    main.styleIndex = 0;
    main.content = content;
    m_outputs.append(main);
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
    case BoundsLockedRole: return item.boundsLocked;
    case ActiveRole: return item.active;
    case EnabledRole: return item.isEnabled;
    case StyleIndexRole: return item.styleIndex;
    case StyleNameRole: {
        // StyleListModel is a lazily-created QML singleton — it can
        // legitimately not exist yet when a screen/binding that shows a
        // style name builds first. A missing roster renders as "None",
        // never a null deref.
        const StyleListModel *styles = StyleListModel::instance();
        if (!styles || item.styleIndex < 0 || item.styleIndex >= styles->rowCount())
            return QStringLiteral("None");
        return styles->data(styles->index(item.styleIndex), StyleListModel::NameRole).toString();
    }
    case ContentRole: {
        QVariantList list;
        for (const OutputContentToggle &toggle : item.content)
            list.append(QVariantMap{ { "key", toggle.key }, { "label", toggle.label }, { "enabled", toggle.enabled } });
        return list;
    }
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
        { ActiveRole, "active" },
        { EnabledRole, "isEnabled" },
        { StyleIndexRole, "styleIndex" },
        { StyleNameRole, "styleName" },
        { ContentRole, "content" },
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
    item.styleIndex = 0;
    item.content = defaultContent();
    m_outputs.append(item);
    endInsertRows();
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
}

void OutputListModel::removeOutput(int index)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    // Main Output is load-bearing — every show needs a primary destination.
    if (m_outputs.at(index).name == QLatin1String(kMainOutputName))
        return;

    beginRemoveRows(QModelIndex(), index, index);
    m_outputs.removeAt(index);
    endRemoveRows();
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
            const QModelIndex changed = this->index(i);
            emit dataChanged(changed, changed, { ActiveRole });
        }
    }
}

void OutputListModel::setEnabled(int index, bool on)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].isEnabled == on)
        return;

    m_outputs[index].isEnabled = on;
    // Disabling also pulls the screen off air — a disabled screen must not
    // keep rendering as LIVE anywhere.
    if (!on && m_outputs[index].active) {
        m_outputs[index].active = false;
        const QModelIndex changed = this->index(index);
        emit dataChanged(changed, changed, { EnabledRole, ActiveRole });
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
}

void OutputListModel::setKind(int index, const QString &kind)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].kind == kind)
        return;

    m_outputs[index].kind = kind;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { KindRole });
}

void OutputListModel::setRefresh(int index, const QString &refresh)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    const QString trimmed = refresh.trimmed();
    if (m_outputs[index].refresh == trimmed)
        return;

    m_outputs[index].refresh = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { RefreshRole });
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
}

void OutputListModel::setStyle(int index, int styleIndex)
{
    if (index < 0 || index >= m_outputs.size())
        return;
    if (m_outputs[index].styleIndex == styleIndex)
        return;

    m_outputs[index].styleIndex = styleIndex;
    const QModelIndex changed = this->index(index);
    // StyleNameRole derives from StyleListModel, so repaint both.
    emit dataChanged(changed, changed, { StyleIndexRole, StyleNameRole });
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

void OutputListModel::setScreenName(int index, const QString &screenName)
{
    if (index < 0 || index >= m_outputs.size())
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
            m_outputs[index].res = QStringLiteral("%1×%2").arg(g.width()).arg(g.height());
            m_outputs[index].refresh = QStringLiteral("%1 Hz").arg(qRound(screen->refreshRate()));
            const QModelIndex changed = this->index(index);
            emit dataChanged(changed, changed, { ScreenNameRole, ResRole, RefreshRole });
            return;
        }
    }

    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ScreenNameRole });
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

    const StyleListModel *styles = StyleListModel::instance();
    QString styleName = QStringLiteral("None");
    if (styles && item.styleIndex >= 0 && item.styleIndex < styles->rowCount())
        styleName = styles->data(styles->index(item.styleIndex), StyleListModel::NameRole).toString();

    return {
        { "name", item.name },
        { "badge", item.badge },
        { "kind", item.kind },
        { "res", item.res },
        { "refresh", item.refresh },
        { "testPattern", item.testPattern },
        { "screenName", item.screenName },
        { "boundsLocked", item.boundsLocked },
        { "active", item.active },
        { "isEnabled", item.isEnabled },
        { "styleIndex", item.styleIndex },
        { "styleName", styleName },
        { "content", content },
    };
}
