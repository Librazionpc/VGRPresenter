#include "StyleListModel.h"

#include "OutputListModel.h"
#include "services/EngineBridge.h"
#include "modules/project/StyleStore.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <algorithm>

QPointer<StyleListModel> StyleListModel::s_instance = nullptr;

namespace {

// Formats the new style's resolution from the primary screen the same way
// OutputListModel seeds the Main Output's res — a new style describes the
// screen it will most likely be shown on, not a mock 1920×1080.
QString defaultRes()
{
    if (const QScreen *screen = QGuiApplication::primaryScreen())
        return QStringLiteral("%1×%2").arg(screen->size().width()).arg(screen->size().height());
    return QStringLiteral("1920×1080");
}

} // namespace

StyleListModel::StyleListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    s_instance = this;

    // THE BOOT GAP (found live: "my styles don't show after a rebuild"): the
    // kernel's DatabaseManager only OPENS <dataDir>/kernel.json during Boot,
    // which the app defers until after the QML load — but this singleton is
    // constructed AT QML load, so the hydrate below read an EMPTY store every
    // launch (the styles were saved the whole time; the UI just never saw
    // them again) and the first post-boot mutation flushed the empty roster
    // over the real one. So: hydrate now for the already-booted case, and
    // RE-HYDRATE the moment boot completes (TheTableService::loadLibrary's
    // contract).
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this] {
        if (EngineBridge::instance().booted())
            reloadFromStore();
    });
    reloadFromStore();
}

void StyleListModel::reloadFromStore()
{
    beginResetModel();
    m_styles.clear();

    // A missing document (first run) or a corrupt one reads as an empty
    // roster — the app starts fresh, never a failed boot (same contract as
    // the production graph's restore).
    for (const bps::project::StoredStyle &s : bps::project::StyleStore::Instance().Get()) {
        StyleItem item;
        item.id = QString::fromStdString(s.id);
        item.name = QString::fromStdString(s.name);
        item.res = QString::fromStdString(s.res);
        item.contentType = QString::fromStdString(s.contentType);
        item.templateKey = QString::fromStdString(s.templateKey);
        item.backgroundColor = QString::fromStdString(s.backgroundColor);
        item.backgroundImage = QString::fromStdString(s.backgroundImage);
        item.clearBackgroundOnText = s.clearBackgroundOnText;
        item.showShows = s.showTemplates[0];
        item.showMedia = s.showTemplates[1];
        item.showScripture = s.showTemplates[2];
        item.showTable = s.showTemplates[3];
        item.familyTemplateKeys[0] = QString::fromStdString(s.templateKeys[0]);
        item.familyTemplateKeys[1] = QString::fromStdString(s.templateKeys[1]);
        item.familyTemplateKeys[2] = QString::fromStdString(s.templateKeys[2]);
        item.familyTemplateKeys[3] = QString::fromStdString(s.templateKeys[3]);
        item.category = QString::fromStdString(s.category);
        m_styles.append(item);
    }
    endResetModel();
    emit rosterChanged();
}

StyleListModel::~StyleListModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

int StyleListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_styles.size();
}

QVariant StyleListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_styles.size())
        return {};

    const StyleItem &item = m_styles.at(index.row());
    switch (role) {
    case IdRole: return item.id;
    case NameRole: return item.name;
    case ResRole: return item.res;
    case ContentTypeRole: return item.contentType;
    case TemplateKeyRole: return item.templateKey;
    case BackgroundColorRole: return item.backgroundColor;
    case BackgroundImageRole: return item.backgroundImage;
    case ClearBackgroundOnTextRole: return item.clearBackgroundOnText;
    case ShowShowsRole: return item.showShows;
    case ShowMediaRole: return item.showMedia;
    case ShowScriptureRole: return item.showScripture;
    case ShowTableRole: return item.showTable;
    case CategoryRole: return item.category;
    default: return {};
    }
}

QHash<int, QByteArray> StyleListModel::roleNames() const
{
    return {
        { IdRole, "styleId" },
        { NameRole, "name" },
        { ResRole, "res" },
        { ContentTypeRole, "contentType" },
        { TemplateKeyRole, "templateKey" },
        { BackgroundColorRole, "backgroundColor" },
        { BackgroundImageRole, "backgroundImage" },
        { ClearBackgroundOnTextRole, "clearBackgroundOnText" },
        { ShowShowsRole, "showShows" },
        { ShowMediaRole, "showMedia" },
        { ShowScriptureRole, "showScripture" },
        { ShowTableRole, "showTable" },
        { CategoryRole, "category" },
    };
}

int StyleListModel::nextIdNumber() const
{
    // "s<n>" — n is one past the highest seen, and never reused within the
    // session even after removals (deleting the last style then adding must
    // not resurrect the old id: a persisted output could still reference it).
    int max = 0;
    for (const StyleItem &item : m_styles) {
        if (!item.id.startsWith(QLatin1String("s")))
            continue;
        bool ok = false;
        const int n = item.id.mid(1).toInt(&ok);
        if (ok)
            max = std::max(max, n);
    }
    return max + 1;
}

void StyleListModel::save()
{
    QList<bps::project::StoredStyle> stored;
    stored.reserve(m_styles.size());
    for (const StyleItem &item : m_styles) {
        bps::project::StoredStyle s;
        s.id = item.id.toStdString();
        s.name = item.name.toStdString();
        s.res = item.res.toStdString();
        s.contentType = item.contentType.toStdString();
        s.templateKey = item.templateKey.toStdString();
        s.backgroundColor = item.backgroundColor.toStdString();
        s.backgroundImage = item.backgroundImage.toStdString();
        s.clearBackgroundOnText = item.clearBackgroundOnText;
        s.showTemplates[0] = item.showShows;
        s.showTemplates[1] = item.showMedia;
        s.showTemplates[2] = item.showScripture;
        s.showTemplates[3] = item.showTable;
        s.templateKeys[0] = item.familyTemplateKeys[0].toStdString();
        s.templateKeys[1] = item.familyTemplateKeys[1].toStdString();
        s.templateKeys[2] = item.familyTemplateKeys[2].toStdString();
        s.templateKeys[3] = item.familyTemplateKeys[3].toStdString();
        s.category = item.category.toStdString();
        stored.append(s);
    }
    // The kernel's store owns the vector-based API here; a failed flush (disk
    // full, permissions) logs through StyleStore's own path — the in-memory
    // roster stays authoritative for the session either way.
    (void)bps::project::StyleStore::Instance().Save(
        std::vector<bps::project::StoredStyle>(stored.cbegin(), stored.cend()));
    emit rosterChanged();
}

void StyleListModel::addStyle()
{
    const int row = m_styles.size();
    beginInsertRows(QModelIndex(), row, row);
    StyleItem item;
    item.id = QStringLiteral("s%1").arg(nextIdNumber());
    item.name = QStringLiteral("New Style %1").arg(row + 1);
    item.res = defaultRes();
    m_styles.append(item);
    endInsertRows();
    save();
}

void StyleListModel::removeStyle(int index)
{
    if (index < 0 || index >= m_styles.size())
        return;

    // Model-side backstop: every output that pointed here goes to "None"
    // (empty id) first, so a stale UI caller can't orphan a reference. (The
    // Styles screen hides Delete only while an output still uses the style;
    // this keeps the invariant without trusting the UI.)
    const QString doomedId = m_styles.at(index).id;
    OutputListModel *outputs = OutputListModel::instance();
    if (outputs)
        outputs->detachStyleEverywhere(doomedId);

    beginRemoveRows(QModelIndex(), index, index);
    m_styles.removeAt(index);
    endRemoveRows();
    save();
}

void StyleListModel::duplicateStyle(int index)
{
    if (index < 0 || index >= m_styles.size())
        return;

    StyleItem copy = m_styles.at(index);
    copy.id = QStringLiteral("s%1").arg(nextIdNumber());
    copy.name = QStringLiteral("%1 (copy)").arg(copy.name);

    beginInsertRows(QModelIndex(), index + 1, index + 1);
    m_styles.insert(index + 1, copy);
    endInsertRows();
    save();
}

void StyleListModel::renameStyle(int index, const QString &name)
{
    if (index < 0 || index >= m_styles.size())
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_styles[index].name == trimmed)
        return;

    m_styles[index].name = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole });
    save();
}

void StyleListModel::setResolution(int index, const QString &res)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].res == res)
        return;

    m_styles[index].res = res;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ResRole });
    save();
}

void StyleListModel::setContentType(int index, const QString &contentType)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].contentType == contentType)
        return;

    m_styles[index].contentType = contentType;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ContentTypeRole });
    save();
}

void StyleListModel::setTemplateKey(int index, const QString &templateKey)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].templateKey == templateKey)
        return;

    m_styles[index].templateKey = templateKey;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { TemplateKeyRole });
    save();
}

void StyleListModel::setBackgroundColor(int index, const QString &color)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].backgroundColor == color)
        return;

    m_styles[index].backgroundColor = color;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { BackgroundColorRole });
    save();
}

void StyleListModel::setClearBackgroundOnText(int index, bool on)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].clearBackgroundOnText == on)
        return;

    m_styles[index].clearBackgroundOnText = on;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ClearBackgroundOnTextRole });
    save();
}

void StyleListModel::setBackgroundImage(int index, const QString &path)
{
    if (index < 0 || index >= m_styles.size())
        return;
    const QString normalized = QDir::fromNativeSeparators(path);
    if (m_styles[index].backgroundImage == normalized)
        return;

    m_styles[index].backgroundImage = normalized;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { BackgroundImageRole });
    save();
}

void StyleListModel::setShowTemplate(int index, const QString &contentType, bool on)
{
    if (index < 0 || index >= m_styles.size())
        return;
    bool *field = contentType == QLatin1String("shows") ? &m_styles[index].showShows
                : contentType == QLatin1String("media") ? &m_styles[index].showMedia
                : contentType == QLatin1String("scripture") ? &m_styles[index].showScripture
                : contentType == QLatin1String("table") ? &m_styles[index].showTable
                : nullptr;
    if (!field || *field == on)
        return;

    *field = on;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ShowShowsRole, ShowMediaRole, ShowScriptureRole, ShowTableRole });
    save();
}

void StyleListModel::setFamilyTemplateKey(int index, const QString &contentType,
                                           const QString &templateKey)
{
    if (index < 0 || index >= m_styles.size())
        return;
    QString *key = contentType == QLatin1String("shows")     ? &m_styles[index].familyTemplateKeys[0]
                 : contentType == QLatin1String("media")     ? &m_styles[index].familyTemplateKeys[1]
                 : contentType == QLatin1String("scripture") ? &m_styles[index].familyTemplateKeys[2]
                 : contentType == QLatin1String("table")     ? &m_styles[index].familyTemplateKeys[3]
                 : nullptr;
    if (!key || *key == templateKey.trimmed())
        return;

    *key = templateKey.trimmed();
    save();
}

void StyleListModel::setCategory(int index, const QString &category)
{
    if (index < 0 || index >= m_styles.size())
        return;
    const QString trimmed = category.trimmed();
    if (m_styles[index].category == trimmed)
        return;

    m_styles[index].category = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { CategoryRole });
    save();
}

QVariantMap StyleListModel::getStyle(int index) const
{
    if (index < 0 || index >= m_styles.size())
        return {};

    const StyleItem &item = m_styles.at(index);
    return {
        { "id", item.id },
        { "name", item.name },
        { "res", item.res },
        { "contentType", item.contentType },
        { "templateKey", item.templateKey },
        { "backgroundColor", item.backgroundColor },
        { "backgroundImage", item.backgroundImage },
        { "clearBackgroundOnText", item.clearBackgroundOnText },
        { "showShows", item.showShows },
        { "showMedia", item.showMedia },
        { "showScripture", item.showScripture },
        { "showTable", item.showTable },
        { "familyTemplateShows", item.familyTemplateKeys[0] },
        { "familyTemplateMedia", item.familyTemplateKeys[1] },
        { "familyTemplateScripture", item.familyTemplateKeys[2] },
        { "familyTemplateTable", item.familyTemplateKeys[3] },
        { "category", item.category },
    };
}

int StyleListModel::rowForId(const QString &id) const
{
    for (int i = 0; i < m_styles.size(); ++i)
        if (m_styles.at(i).id == id)
            return i;
    return -1;
}

QString StyleListModel::pickImageFile() const
{
    if (!EngineBridge::instance().booted())
        return {};
    auto r = bps::platform::PlatformAccessor::Get().Dialogs().OpenFileDialog(
        "Pick a background image",
        { "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)", "All files (*.*)" });
    return r.ok() && r.value() ? QString::fromStdString(*r.value()) : QString();
}
