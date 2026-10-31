#include "GroupListModel.h"

#include "models/OutputListModel.h"
#include "services/EngineBridge.h"
#include "modules/project/GroupStore.hpp"

#include <QJSEngine>
#include <algorithm>

QPointer<GroupListModel> GroupListModel::s_instance = nullptr;

GroupListModel *GroupListModel::instance()
{
    static GroupListModel s;
    return &s;
}

GroupListModel *GroupListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    GroupListModel *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

GroupListModel::GroupListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    Q_ASSERT_X(!s_instance || s_instance == this, "GroupListModel",
               "a second GroupListModel was constructed — instance() would "
               "silently rebind and every C++ reader would hold the wrong one");
    s_instance = this;

    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this] {
        if (EngineBridge::instance().booted())
            reloadFromStore();
    });
    reloadFromStore();
}

GroupListModel::~GroupListModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

void GroupListModel::seedDefaults()
{
    // FreeShow's own eight (src/electron/data/defaults.ts) — same names,
    // same colours, same shortcuts — so a user coming from FreeShow finds
    // the identical starter set.
    struct Seed { const char *name; const char *color; const char *shortcut; };
    static constexpr Seed kDefaults[] = {
        { "break",      "#f5255e", "" },
        { "bridge",     "#f52598", "B" },
        { "chorus",     "#f525d2", "C" },
        { "intro",      "#d525f5", "" },
        { "outro",      "#a525f5", "" },
        { "pre_chorus", "#8825f5", "" },
        { "tag",        "#7525f5", "" },
        { "verse",      "#5825f5", "V" },
    };
    m_groups.clear();
    int n = 1;
    for (const Seed &s : kDefaults) {
        GroupItem item;
        item.id = QStringLiteral("g%1").arg(n++);
        item.name = QString::fromUtf8(s.name);
        item.color = QString::fromUtf8(s.color);
        item.shortcut = QString::fromUtf8(s.shortcut);
        item.isDefault = true;
        m_groups.append(item);
    }
}

void GroupListModel::reloadFromStore()
{
    beginResetModel();

    const std::vector<bps::project::StoredGroup> stored = bps::project::GroupStore::Instance().Get();
    if (stored.empty()) {
        // First run (or a document that was never saved): seed the eight
        // built-ins and save them once, so the roster this app shows matches
        // what GroupStore::Get() returns on every later read (reloadFromStore
        // on a FRESH boot would otherwise seed again every launch — the save
        // here is what makes the seed a one-time event).
        seedDefaults();
        endResetModel();
        save();
        emit rosterChanged();
        return;
    }

    m_groups.clear();
    for (const bps::project::StoredGroup &g : stored) {
        GroupItem item;
        item.id = QString::fromStdString(g.id);
        item.name = QString::fromStdString(g.name);
        item.color = QString::fromStdString(g.color);
        item.shortcut = QString::fromStdString(g.shortcut);
        item.templateId = QString::fromStdString(g.templateId);
        item.isDefault = g.isDefault;
        m_groups.append(item);
    }
    endResetModel();
    emit rosterChanged();
}

void GroupListModel::save()
{
    std::vector<bps::project::StoredGroup> stored;
    stored.reserve(m_groups.size());
    for (const GroupItem &item : m_groups) {
        bps::project::StoredGroup g;
        g.id = item.id.toStdString();
        g.name = item.name.toStdString();
        g.color = item.color.toStdString();
        g.shortcut = item.shortcut.toStdString();
        g.templateId = item.templateId.toStdString();
        g.isDefault = item.isDefault;
        stored.push_back(std::move(g));
    }
    (void)bps::project::GroupStore::Instance().Save(stored);
    emit rosterChanged();
}

int GroupListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_groups.size();
}

QVariant GroupListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_groups.size())
        return {};

    const GroupItem &item = m_groups.at(index.row());
    switch (role) {
    case IdRole: return item.id;
    case NameRole: return item.name;
    case ColorRole: return item.color;
    case ShortcutRole: return item.shortcut;
    case TemplateRole: return item.templateId;
    case DefaultRole: return item.isDefault;
    default: return {};
    }
}

QHash<int, QByteArray> GroupListModel::roleNames() const
{
    return {
        { IdRole, "id" },
        { NameRole, "name" },
        { ColorRole, "color" },
        { ShortcutRole, "shortcut" },
        { TemplateRole, "templateId" },
        { DefaultRole, "isDefault" },
    };
}

void GroupListModel::addGroup()
{
    const int row = m_groups.size();
    beginInsertRows(QModelIndex(), row, row);
    GroupItem item;
    // "g<n>" never reused within the session, same convention StyleListModel's
    // nextIdNumber() follows — the highest numeric suffix seen, plus one.
    int max = 0;
    for (const GroupItem &g : m_groups) {
        if (!g.id.startsWith(QLatin1String("g")))
            continue;
        bool ok = false;
        const int n = g.id.mid(1).toInt(&ok);
        if (ok)
            max = std::max(max, n);
    }
    item.id = QStringLiteral("g%1").arg(max + 1);
    item.name = QStringLiteral("New group");
    item.color = QStringLiteral("#ffffff");
    m_groups.append(item);
    endInsertRows();
    save();
}

void GroupListModel::removeGroup(int index)
{
    if (index < 0 || index >= m_groups.size())
        return;
    beginRemoveRows(QModelIndex(), index, index);
    m_groups.removeAt(index);
    endRemoveRows();
    save();
}

void GroupListModel::renameGroup(int index, const QString &name)
{
    if (index < 0 || index >= m_groups.size())
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_groups[index].name == trimmed)
        return;

    m_groups[index].name = trimmed;
    // Renaming a default detaches it (FreeShow's own rule: changeGroup()
    // deletes the `default` tag the moment the name changes) — a default's
    // name is meant to stay the translation key it was seeded with.
    m_groups[index].isDefault = false;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole, DefaultRole });
    save();
}

void GroupListModel::setColor(int index, const QString &color)
{
    if (index < 0 || index >= m_groups.size())
        return;
    if (m_groups[index].color == color)
        return;
    m_groups[index].color = color;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ColorRole });
    save();
}

void GroupListModel::setShortcut(int index, const QString &shortcut)
{
    if (index < 0 || index >= m_groups.size())
        return;
    const QString trimmed = shortcut.trimmed().toUpper();
    // One shortcut, one group: refuse a letter another group already owns
    // instead of silently stealing it out from under it.
    for (int i = 0; i < m_groups.size(); ++i) {
        if (i != index && !trimmed.isEmpty() && m_groups.at(i).shortcut == trimmed)
            return;
    }
    if (m_groups[index].shortcut == trimmed)
        return;
    m_groups[index].shortcut = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ShortcutRole });
    save();
}

void GroupListModel::setTemplate(int index, const QString &templateId)
{
    if (index < 0 || index >= m_groups.size())
        return;
    const QString trimmed = templateId.trimmed();
    if (m_groups[index].templateId == trimmed)
        return;
    m_groups[index].templateId = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { TemplateRole });
    save();
    // The composition has to re-compose the slides this group owns (the tag
    // turns into a template in StyleSpecBuilder), and the outputs' own spec may
    // be the one carrying it — the same "a roster edit lands on the next frame"
    // contract every other roster mutation follows.
    if (OutputListModel *outputs = OutputListModel::instance())
        outputs->repushActiveStyleForTag(m_groups.at(index).name);
}

QString GroupListModel::templateForTag(const QString &tag) const
{
    if (tag.isEmpty())
        return {};
    for (const GroupItem &item : m_groups) {
        if (item.name == tag)
            return item.templateId;
    }
    return {};
}

void GroupListModel::resetToDefaults()
{
    beginResetModel();
    seedDefaults();
    endResetModel();
    save();
}

QVariantMap GroupListModel::getGroup(int index) const
{
    if (index < 0 || index >= m_groups.size())
        return {};
    const GroupItem &item = m_groups.at(index);
    return {
        { "id", item.id },
        { "name", item.name },
        { "color", item.color },
        { "shortcut", item.shortcut },
        { "templateId", item.templateId },
        { "isDefault", item.isDefault },
    };
}

int GroupListModel::rowForId(const QString &id) const
{
    for (int i = 0; i < m_groups.size(); ++i)
        if (m_groups.at(i).id == id)
            return i;
    return -1;
}

QVariantMap GroupListModel::shortcutMap() const
{
    QVariantMap out;
    for (const GroupItem &item : m_groups)
        if (!item.shortcut.isEmpty())
            out.insert(item.shortcut, item.name);
    return out;
}

QVariantList GroupListModel::previewEntries(int limit) const
{
    QVariantList out;
    for (int i = 0; i < m_groups.size() && (limit <= 0 || i < limit); ++i) {
        const GroupItem &item = m_groups.at(i);
        out.append(QVariantMap{ { "name", item.name }, { "color", item.color } });
    }
    return out;
}
