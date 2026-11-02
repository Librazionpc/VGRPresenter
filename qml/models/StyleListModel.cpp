#include "StyleListModel.h"

#include "FamilySlots.h"
#include "OutputListModel.h"
#include "services/EngineBridge.h"
#include "services/EngineWorker.h"
#include "modules/project/StyleStore.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QGuiApplication>
#include <QJSEngine>
#include <QScreen>
#include <QSet>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <cstdio>

QPointer<StyleListModel> StyleListModel::s_instance = nullptr;

StyleListModel *StyleListModel::instance()
{
    static StyleListModel s;
    return &s;
}

StyleListModel *StyleListModel::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    // CppOwnership: this object outlives the QML engine and must never be
    // deleted by it (same contract every service singleton follows).
    StyleListModel *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

namespace {
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
    if (qEnvironmentVariableIsSet("VGR_STYLEBG_TEST")) {
        EngineBridge::write(QStringLiteral("info"), QStringLiteral("StyleBgId"),
                            QStringLiteral("StyleListModel ctor this=%1 s_instance=%2")
                                .arg(quintptr(this)).arg(quintptr(s_instance.data())));
    }
    Q_ASSERT_X(!s_instance || s_instance == this, "StyleListModel",
               "a second StyleListModel was constructed — instance() would "
               "silently rebind and every C++ reader would hold the wrong one");
    s_instance = this;

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

    for (const bps::project::StoredStyle &s : bps::project::StyleStore::Instance().Get()) {
        StyleItem item;
        item.id = QString::fromStdString(s.id);
        item.name = QString::fromStdString(s.name);
        item.res = QString::fromStdString(s.res);
        item.contentType = QString::fromStdString(s.contentType);
        item.templateKey = QString::fromStdString(s.templateKey);
        item.backgroundColor = QString::fromStdString(s.backgroundColor);
        item.backgroundImage = QString::fromStdString(s.backgroundImage);
        // One stat per style, once per roster load — instead of one per read of
        // the styleBackground role (see StyleItem::backgroundImageExists).
        item.backgroundImageExists =
            !item.backgroundImage.isEmpty() && QFile::exists(item.backgroundImage);
        item.clearBackgroundOnText = s.clearBackgroundOnText;
        for (int slot = 0; slot < FamilySlots::kCount; ++slot) {
            item.setShowForSlot(slot, s.showTemplates[static_cast<std::size_t>(slot)]);
            item.familyTemplateKeys[slot] = QString::fromStdString(s.templateKeys[static_cast<std::size_t>(slot)]);
        }
        item.showOverlay = s.showTemplates[StyleItem::kOverlaySlot];
        item.category = QString::fromStdString(s.category);
        for (const auto &entry : s.showCategoryTemplates) {
            item.showCategoryTemplates.append(QVariantMap{
                { QStringLiteral("category"), QString::fromStdString(entry.category) },
                { QStringLiteral("templateKey"), QString::fromStdString(entry.templateKey) },
                { QStringLiteral("overrideEnabled"), entry.overrideEnabled },
            });
        }
        // Routing-board membership rides the roster (StoredStyle::onBoard).
        item.onBoard = s.onBoard;
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
    case ShowOverlayRole: return item.showOverlay;
    case CategoryRole: return item.category;
    case ShowCategoryTemplatesRole: return item.showCategoryTemplates;
    case OnBoardRole: return item.onBoard;
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
        { ShowOverlayRole, "showOverlay" },
        { CategoryRole, "category" },
        { ShowCategoryTemplatesRole, "showCategoryTemplates" },
        { OnBoardRole, "onBoard" },
    };
}

int StyleListModel::nextIdNumber() const
{
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
        for (int slot = 0; slot < FamilySlots::kCount; ++slot) {
            s.showTemplates[static_cast<std::size_t>(slot)] = item.showForSlot(slot);
            s.templateKeys[static_cast<std::size_t>(slot)] = item.familyTemplateKeys[slot].toStdString();
        }
        s.showTemplates[StyleItem::kOverlaySlot] = item.showOverlay;   // its own axis (see StyleItem)
        s.category = item.category.toStdString();
        for (const QVariant &value : item.showCategoryTemplates) {
            const QVariantMap entry = value.toMap();
            bps::project::StoredShowCategoryTemplate storedEntry;
            storedEntry.category = entry.value(QStringLiteral("category")).toString().trimmed().toStdString();
            storedEntry.templateKey = entry.value(QStringLiteral("templateKey")).toString().trimmed().toStdString();
            storedEntry.overrideEnabled = entry.value(QStringLiteral("overrideEnabled")).toBool();
            if (!storedEntry.category.empty())
                s.showCategoryTemplates.push_back(std::move(storedEntry));
        }
        s.onBoard = item.onBoard;
        stored.append(s);
    }
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

void StyleListModel::setOnBoard(int index, bool onBoard)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].onBoard == onBoard)
        return;
    m_styles[index].onBoard = onBoard;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { OnBoardRole });
    save();
}

void StyleListModel::removeStyle(int index)
{
    if (index < 0 || index >= m_styles.size())
        return;

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
    m_styles[index].backgroundImageExists = !normalized.isEmpty();
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { BackgroundImageRole });
    save();

    if (normalized.isEmpty())
        return;   // "no background" is already the whole truth
    const int row = index;
    vgr::EngineWorker::instance().runOffThread([this, row, normalized] {
        const bool exists = QFile::exists(normalized);
        vgr::EngineWorker::onInterfaceThread(this, [this, row, normalized, exists] {
            if (row < 0 || row >= m_styles.size())
                return;
            if (m_styles.at(row).backgroundImage != normalized)
                return;
            if (m_styles.at(row).backgroundImageExists == exists)
                return;
            m_styles[row].backgroundImageExists = exists;
            const QModelIndex rowChanged = this->index(row);
            emit dataChanged(rowChanged, rowChanged, { BackgroundImageRole });
        });
    });
}

void StyleListModel::setShowTemplate(int index, const QString &contentType, bool on)
{
    if (index < 0 || index >= m_styles.size())
        return;
    StyleItem &style = m_styles[index];
    if (contentType == QLatin1String("overlay")) {
        if (style.showOverlay == on)
            return;
        style.showOverlay = on;
    } else {
        const int slot = FamilySlots::SlotFor(contentType);
        if (slot < 0 || style.showForSlot(slot) == on)
            return;
        style.setShowForSlot(slot, on);
    }
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed,
                      { ShowShowsRole, ShowMediaRole, ShowScriptureRole, ShowTableRole, ShowOverlayRole });
    save();
}

void StyleListModel::setFamilyTemplateKey(int index, const QString &contentType,
                                           const QString &templateKey)
{
    if (index < 0 || index >= m_styles.size())
        return;
    const int slot = FamilySlots::SlotFor(contentType);
    if (slot < 0)
        return;
    QString &key = m_styles[index].familyTemplateKeys[slot];
    if (key == templateKey.trimmed())
        return;

    key = templateKey.trimmed();
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

void StyleListModel::setShowCategoryTemplates(int index, const QVariantList &entries)
{
    if (index < 0 || index >= m_styles.size())
        return;
    QVariantList normalized;
    QSet<QString> seen;
    for (const QVariant &value : entries) {
        const QVariantMap entry = value.toMap();
        const QString category = entry.value(QStringLiteral("category")).toString().trimmed();
        if (category.isEmpty() || seen.contains(category))
            continue;
        seen.insert(category);
        normalized.append(QVariantMap{
            { QStringLiteral("category"), category },
            { QStringLiteral("templateKey"), entry.value(QStringLiteral("templateKey")).toString().trimmed() },
            { QStringLiteral("overrideEnabled"), entry.value(QStringLiteral("overrideEnabled")).toBool() },
        });
    }
    if (m_styles[index].showCategoryTemplates == normalized)
        return;
    m_styles[index].showCategoryTemplates = normalized;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ShowCategoryTemplatesRole });
    save();
}

QVariantMap StyleListModel::getStyle(int index) const
{
    if (index < 0 || index >= m_styles.size())
        return {};

    const StyleItem &item = m_styles.at(index);
    QVariantMap out{
        { "id", item.id },
        { "name", item.name },
        { "res", item.res },
        { "contentType", item.contentType },
        { "templateKey", item.templateKey },
        { "backgroundColor", item.backgroundColor },
        { "backgroundImage", item.backgroundImage },
        // The cached stat, so a reader never touches the disk to decide this
        // (StyleItem::backgroundImageExists).
        { "hasImage", item.backgroundImageExists },
        { "clearBackgroundOnText", item.clearBackgroundOnText },
        { "showOverlay", item.showOverlay },   // its own axis, not a family slot
        { "category", item.category },
        { "showCategoryTemplates", item.showCategoryTemplates },
        // Whether this style has a card on the Routing board.
        { "onBoard", item.onBoard },
    };
    for (int slot = 0; slot < FamilySlots::kCount; ++slot) {
        const FamilySlots::Family &fam = FamilySlots::Rows()[static_cast<std::size_t>(slot)];
        out.insert(QString::fromLatin1(fam.showProperty), item.showForSlot(slot));
        out.insert(QString::fromLatin1(fam.templateProperty), item.familyTemplateKeys[slot]);
    }
    return out;
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
        { "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)" });
    return r.ok() && r.value() ? QString::fromStdString(*r.value()) : QString();
}