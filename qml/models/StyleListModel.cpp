#include "StyleListModel.h"

#include "FamilySlots.h"
#include "OutputListModel.h"
#include "services/EngineBridge.h"
#include "modules/project/StyleStore.hpp"
#include "platform/PlatformAccessor.hpp"

#include <QGuiApplication>
#include <QJSEngine>
#include <QScreen>
#include <QDir>
#include <algorithm>
#include <cstdio>

QPointer<StyleListModel> StyleListModel::s_instance = nullptr;

// The one instance, for C++ AND QML.
//
// QML_SINGLETON alone lets QML construct the object itself, LAZILY, on the
// first property access — which means a C++ reader that runs earlier gets
// null and quietly does nothing. That was the reported bug: nothing in the
// boot scene touched the Styles screen, StyleListModel::instance() stayed
// null for the whole session, and every style-resolving read returned "no
// style" (the monitor tile fell back to the transparency checkerboard and
// the engine rendered unstyled). A warm-up property in Main.qml papered over
// it; this removes the class of bug instead. create() below was meant to hand
// QML the SAME static object, but the engine does not call it (measured
// 2026-10-11: the generated module registration is
// qmlRegisterTypesAndRevisions<...>(), so QML default-constructs its own
// instance) — the Styles screen and every C++ reader therefore ended up on two
// different rosters. Main.qml now hands QML's instance to OutputListModel at
// startup (OutputListModel::adoptStyleRoster), which is the roster C++ uses.
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
    // TEMPORARY, env-gated (VGR_STYLEBG_TEST): one line per construction and
    // one per create() call, so a run can tell how many rosters exist and
    // whether QML reaches this class through its own factory. Qt's Q_ASSERT
    // is compiled out of this build (QT_NO_DEBUG), so a duplicate instance is
    // otherwise silent — the style-background scenario found the UI and the
    // output model resolving styles through different rosters, and this is
    // what names the mechanism. Remove once the duplicate is gone.
    if (qEnvironmentVariableIsSet("VGR_STYLEBG_TEST"))
        fprintf(stderr, "[SELFTEST-STYLEBG-ID] ctor this=%p s_instance=%p\n",
                static_cast<void *>(this), static_cast<void *>(s_instance.data()));
    Q_ASSERT_X(!s_instance || s_instance == this, "StyleListModel",
               "a second StyleListModel was constructed — instance() would "
               "silently rebind and every C++ reader would hold the wrong one");
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
        // Walked through the ONE family table (FamilySlots.h): the engine's slot
        // order is the storage order, so the families do not need spelling out
        // here — and the overlay gate, which is a different axis, is named once
        // in StyleItem instead of at index 4.
        for (int slot = 0; slot < FamilySlots::kCount; ++slot) {
            item.setShowForSlot(slot, s.showTemplates[static_cast<std::size_t>(slot)]);
            item.familyTemplateKeys[slot] = QString::fromStdString(s.templateKeys[static_cast<std::size_t>(slot)]);
        }
        item.showOverlay = s.showTemplates[StyleItem::kOverlaySlot];
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
    case ShowOverlayRole: return item.showOverlay;
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
        { ShowOverlayRole, "showOverlay" },
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
        for (int slot = 0; slot < FamilySlots::kCount; ++slot) {
            s.showTemplates[static_cast<std::size_t>(slot)] = item.showForSlot(slot);
            s.templateKeys[static_cast<std::size_t>(slot)] = item.familyTemplateKeys[slot].toStdString();
        }
        s.showTemplates[StyleItem::kOverlaySlot] = item.showOverlay;   // its own axis (see StyleItem)
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
    // The four slide families go through the shared table; "overlay" is not one
    // of them (it gates the Overlays pane's layer, not a content family), so it is
    // the one tag this function still names.
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
    // A content type that names no family has no slot to pick into (an unknown
    // tag, or "overlay" — an overlay has no template; it always renders from its
    // own design's blocks).
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
        { "clearBackgroundOnText", item.clearBackgroundOnText },
        { "showOverlay", item.showOverlay },   // its own axis, not a family slot
        { "category", item.category },
    };
    // The four families' gates and template picks, named from the ONE table
    // (FamilySlots.h) instead of by hand: these are the property names the QML
    // style editor and the output model read, so they are built here and nowhere
    // else. An output model used to build its own key by capitalising a content
    // type, which named nothing for a tag that did not follow the convention.
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
        // Decodable images only: the background picker below rejects anything
        // else, so letting the picker offer every file just relocated the
        // rejection.
        { "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)" });
    return r.ok() && r.value() ? QString::fromStdString(*r.value()) : QString();
}
