#include "services/ProjectService.h"

#include "modules/library/ProjectLibrary.hpp"
#include "platform/PlatformAccessor.hpp"
#include "services/EngineBridge.h"
#include "services/EventBus.h"
#include "services/SettingsService.h"

#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>

namespace bl = bps::library;

namespace {

QString qstr(const std::string &s) { return QString::fromStdString(s); }

QVariantMap metaOf(const std::string &json)
{
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object().toVariantMap();
}

QString metaToJson(const QVariant &meta)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(meta.toMap())).toJson(QJsonDocument::Compact));
}

QVariantMap projectMap(const bl::Project &p)
{
    QVariantList items;
    for (const bl::ProjectItem &i : p.items)
        items.append(QVariantMap{ { QStringLiteral("id"), qstr(i.id) }, { QStringLiteral("type"), qstr(i.type) }, { QStringLiteral("ref"), qstr(i.ref) },
                                  { QStringLiteral("name"), qstr(i.name) }, { QStringLiteral("layout"), qstr(i.layout) }, { QStringLiteral("color"), qstr(i.color) },
                                  { QStringLiteral("meta"), metaOf(i.metaJson) } });
    return { { QStringLiteral("id"), qstr(p.id) }, { QStringLiteral("name"), qstr(p.name) }, { QStringLiteral("parent"), qstr(p.parent) },
             { QStringLiteral("notes"), qstr(p.notes) }, { QStringLiteral("archived"), p.archived }, { QStringLiteral("sectionsLocked"), p.sectionsLocked },
             { QStringLiteral("sectionsCollapsed"), p.sectionsCollapsed }, { QStringLiteral("items"), items } };
}

} // namespace

ProjectService &ProjectService::instance()
{
    static ProjectService s;
    return s;
}

ProjectService *ProjectService::create(QQmlEngine *engine, QJSEngine *jsEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(jsEngine)
    QJSEngine::setObjectOwnership(&instance(), QJSEngine::CppOwnership);
    return &instance();
}

ProjectService::ProjectService(QObject *parent) : QObject(parent)
{
    connect(&EngineBridge::instance(), &EngineBridge::bootedChanged, this, [this] {
        if (EngineBridge::instance().booted())
            load();
    });
    if (EngineBridge::instance().booted())
        load();
}

ProjectService::~ProjectService() = default;

void ProjectService::report(const QString &message) const
{
    EngineBridge::write(QStringLiteral("warning"), QStringLiteral("Projects"), message);
    EventBus::instance().notify(message, QStringLiteral("error"), tr("Projects"), QStringLiteral("projects.refused"));
}

void ProjectService::load()
{
    if (library_ || !EngineBridge::instance().booted())
        return;
    auto &platform = bps::platform::PlatformAccessor::Get();
    library_ = std::make_unique<bl::ProjectLibrary>(platform.Filesystem().Join(platform.Paths().UserDataDir(), "projects.json"));
    if (auto loaded = library_->Load(); !loaded.ok())
        report(tr("The projects could not be read (%1).").arg(qstr(loaded.error().message)));
    refresh();
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Projects"), QStringLiteral("Project library loaded: %1 folders and projects (%2)").arg(tree_.size()).arg(qstr(platform.Paths().UserDataDir()) + "/projects.json"));
    // The project that was open last time comes back — but only once the
    // engine's settings store has been read. This load and SettingsService::load
    // both wait on the SAME engine-boot signal, and the connection order between
    // them is not fixed, so reading `session.lastProject` right here can see the
    // engine's default "" and the user's project would never reopen. whenReady
    // is the one idiom for that: it runs the restore now if the store is already
    // open, else once it opens.
    SettingsService::instance().whenReady([this]() { if (library_) restoreLastProject(); });
}

// Reopens the project that was open at the last exit (its id lives in the
// settings store). A no-op when there was none, or it is gone from the library.
void ProjectService::restoreLastProject()
{
    const QString last = SettingsService::instance().value(QStringLiteral("session.lastProject")).toString();
    if (!last.isEmpty() && library_ && library_->Get(last.toStdString()).ok())
        openProject(last);
}

void ProjectService::refresh()
{
    tree_.clear();
    recent_.clear();
    if (library_) {
        for (const bl::ProjectTreeRow &r : library_->Tree())
            tree_.append(QVariantMap{ { QStringLiteral("id"), qstr(r.id) }, { QStringLiteral("type"), qstr(r.type) }, { QStringLiteral("name"), qstr(r.name) },
                                      { QStringLiteral("parent"), qstr(r.parent) }, { QStringLiteral("depth"), r.depth },
                                      { QStringLiteral("itemCount"), static_cast<qlonglong>(r.itemCount) }, { QStringLiteral("archived"), r.archived } });
        for (const bl::Project &p : library_->RecentlyUsed())
            recent_.append(projectMap(p));
        if (!activeId_.isEmpty()) {
            auto open = library_->Get(activeId_.toStdString());
            if (open.ok()) {
                active_ = projectMap(open.value());
                const int count = static_cast<int>(open.value().items.size());
                if (activeIndex_ >= count) activeIndex_ = count - 1;
            } else {
                activeId_.clear();
                active_.clear();
                activeIndex_ = -1;
            }
        }
    }
    emit changed();
    emit activeChanged();
}

QVariantMap ProjectService::activeItem() const
{
    const QVariantList items = active_.value(QStringLiteral("items")).toList();
    return activeIndex_ >= 0 && activeIndex_ < items.size() ? items.at(activeIndex_).toMap() : QVariantMap{};
}

// ---- the tree ----

QString ProjectService::createProject(const QString &name, const QString &parentFolder)
{
    if (!library_) return {};
    auto made = library_->Create(name.toStdString(), parentFolder.toStdString());
    if (!made.ok()) { report(qstr(made.error().message)); return {}; }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Projects"), QStringLiteral("Created project '%1' (%2)").arg(name, qstr(made.value())));
    refresh();
    openProject(qstr(made.value()));
    return qstr(made.value());
}

QString ProjectService::createFolder(const QString &name, const QString &parentFolder)
{
    if (!library_) return {};
    auto made = library_->CreateFolder(name.toStdString(), parentFolder.toStdString());
    if (!made.ok()) { report(qstr(made.error().message)); return {}; }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Projects"), QStringLiteral("Created folder '%1' (%2)").arg(name, qstr(made.value())));
    refresh();
    return qstr(made.value());
}

bool ProjectService::rename(const QString &id, const QString &name)
{
    if (!library_) return false;
    auto r = library_->Rename(id.toStdString(), name.toStdString());
    if (!r.ok()) { report(qstr(r.error().message)); return false; }
    refresh();
    return true;
}

QString ProjectService::duplicateProject(const QString &id)
{
    if (!library_) return {};
    auto made = library_->Duplicate(id.toStdString());
    if (!made.ok()) { report(qstr(made.error().message)); return {}; }
    refresh();
    return qstr(made.value());
}

bool ProjectService::moveNode(const QString &id, const QString &newParent)
{
    if (!library_) return false;
    auto r = library_->Move(id.toStdString(), newParent.toStdString());
    if (!r.ok()) { report(qstr(r.error().message)); return false; }
    refresh();
    return true;
}

bool ProjectService::deleteNode(const QString &id)
{
    if (!library_) return false;
    const bool wasOpen = id == activeId_;
    auto r = library_->Delete(id.toStdString());
    if (!r.ok()) { report(qstr(r.error().message)); return false; }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Projects"), QStringLiteral("Deleted %1%2").arg(id, wasOpen ? QStringLiteral(" (it was open)") : QString()));
    if (wasOpen) { activeId_.clear(); active_.clear(); activeIndex_ = -1; }
    refresh();
    return true;
}

bool ProjectService::setArchived(const QString &projectId, bool archived)
{
    if (!library_) return false;
    auto r = library_->SetArchived(projectId.toStdString(), archived);
    if (!r.ok()) { report(qstr(r.error().message)); return false; }
    refresh();
    return true;
}

// ---- the open project ----

bool ProjectService::openProject(const QString &id)
{
    if (!library_) return false;
    auto opened = library_->Open(id.toStdString());
    if (!opened.ok()) { report(qstr(opened.error().message)); return false; }
    activeId_ = id;
    activeIndex_ = opened.value().items.empty() ? -1 : 0;   // the first item is on the centre page, like FreeShow
    if (SettingsService::instance().value(QStringLiteral("session.lastProject")).toString() != id)
        SettingsService::instance().setValue(QStringLiteral("session.lastProject"), id);
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Projects"), QStringLiteral("Opened project '%1' with %2 item(s)").arg(qstr(opened.value().name)).arg(opened.value().items.size()));
    refresh();
    return true;
}

void ProjectService::closeProject()
{
    activeId_.clear();
    active_.clear();
    activeIndex_ = -1;
    if (!SettingsService::instance().value(QStringLiteral("session.lastProject")).toString().isEmpty())
        SettingsService::instance().setValue(QStringLiteral("session.lastProject"), QString());
    emit changed();
    emit activeChanged();
}

#define WITH_OPEN_PROJECT if (!library_ || activeId_.isEmpty()) return false; const std::string project = activeId_.toStdString();
#define FINISH(result) if (!(result).ok()) { report(qstr((result).error().message)); return false; } EngineBridge::write(QStringLiteral("info"), QStringLiteral("Projects"), QStringLiteral("%1 on project %2").arg(QLatin1String(__func__), activeId_)); refresh(); return true;

bool ProjectService::setSectionsLocked(bool locked) { WITH_OPEN_PROJECT auto r = library_->SetSectionsLocked(project, locked); FINISH(r) }
bool ProjectService::setSectionsCollapsed(bool collapsed) { WITH_OPEN_PROJECT auto r = library_->SetSectionsCollapsed(project, collapsed); FINISH(r) }
bool ProjectService::removeItem(int index) { WITH_OPEN_PROJECT auto r = library_->RemoveItem(project, index); FINISH(r) }
bool ProjectService::renameItem(int index, const QString &name) { WITH_OPEN_PROJECT auto r = library_->RenameItem(project, index, name.toStdString()); FINISH(r) }
bool ProjectService::setItemLayout(int index, const QString &layout) { WITH_OPEN_PROJECT auto r = library_->SetItemLayout(project, index, layout.toStdString()); FINISH(r) }
bool ProjectService::setItemColor(int index, const QString &color) { WITH_OPEN_PROJECT auto r = library_->SetItemColor(project, index, color.toStdString()); FINISH(r) }

void ProjectService::selectItem(int index)
{
    const int count = active_.value(QStringLiteral("items")).toList().size();
    const int next = index >= 0 && index < count ? index : -1;
    if (next == activeIndex_) return;
    activeIndex_ = next;
    emit activeChanged();
}

bool ProjectService::addSection(const QString &name, int index)
{
    WITH_OPEN_PROJECT
    bl::ProjectItem section;
    section.type = "section";
    section.name = name.toStdString();
    auto r = library_->AddItems(project, { section }, index);
    FINISH(r)
}

bool ProjectService::moveItems(const QVariantList &indexes, int position)
{
    WITH_OPEN_PROJECT
    const QVariantMap before = activeItem();
    std::vector<int> picked;
    for (const QVariant &i : indexes) picked.push_back(i.toInt());
    auto r = library_->MoveItems(project, picked, position);
    if (!r.ok()) { report(qstr(r.error().message)); return false; }
    // The item that was on the centre page stays on it wherever it went.
    const QString keepId = before.value(QStringLiteral("id")).toString();
    refresh();
    if (!keepId.isEmpty()) {
        const QVariantList items = active_.value(QStringLiteral("items")).toList();
        for (int i = 0; i < items.size(); ++i)
            if (items.at(i).toMap().value(QStringLiteral("id")).toString() == keepId) { activeIndex_ = i; emit activeChanged(); break; }
    }
    return true;
}

// ---- dropping ----

bool ProjectService::acceptsDrop(const QString &area, const QString &kind, bool reorder) const
{
    return bl::AcceptsDrop(area.toStdString(), kind.toStdString(), reorder);
}

bool ProjectService::dropOnProject(const QString &kind, const QVariantList &items, int index)
{
    if (!library_) return false;
    if (!bl::AcceptsDrop("project", kind.toStdString())) {
        report(tr("A project does not take that."));
        return false;
    }
    // With no project open, the drop lands in one: the newest, or a new one.
    if (activeId_.isEmpty()) {
        const auto tree = tree_;
        QString target;
        for (const QVariant &row : tree)
            if (row.toMap().value(QStringLiteral("type")).toString() == QLatin1String("project")) { target = row.toMap().value(QStringLiteral("id")).toString(); break; }
        if (target.isEmpty())
            target = createProject(tr("New project"));
        else
            openProject(target);
        if (activeId_.isEmpty()) return false;
    }
    bl::DropPayload payload;
    payload.kind = kind.toStdString();
    for (const QVariant &v : items) {
        const QVariantMap m = v.toMap();
        payload.items.push_back({ m.value(QStringLiteral("ref")).toString().toStdString(), m.value(QStringLiteral("name")).toString().toStdString(),
                                  m.value(QStringLiteral("type")).toString().toStdString(),
                                  m.contains(QStringLiteral("meta")) ? metaToJson(m.value(QStringLiteral("meta"))).toStdString() : std::string("{}") });
    }
    auto r = library_->DropOnProject(activeId_.toStdString(), payload, index);
    if (!r.ok()) { report(qstr(r.error().message)); return false; }
    EngineBridge::write(QStringLiteral("info"), QStringLiteral("Projects"), QStringLiteral("Dropped %1 '%2' item(s) on project %3").arg(payload.items.size()).arg(kind, activeId_));
    refresh();
    return true;
}
