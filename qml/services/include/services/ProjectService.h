#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

namespace bps::library { class ProjectLibrary; }

class QQmlEngine;
class QJSEngine;

// The UI's window onto the ENGINE's projects (bps::library::ProjectLibrary - FreeShow's Projects panel): the folder / project tree, the
// open project's items, the item on the centre page, and every change to them. The engine decides what a project may hold, what may
// be dropped where and how items move; this hands that to QML and keeps which project and item are open.
class ProjectService : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Folders and projects in tree order: [{ id, type: "folder"|"project", name, parent, depth, itemCount, archived }]
    Q_PROPERTY(QVariantList tree READ tree NOTIFY changed)
    // The project that is open ("" = the tree is showing), and its details: { id, name, parent, notes, archived, sectionsLocked,
    // sectionsCollapsed, items: [{ id, type, ref, name, layout, color, meta }] }
    Q_PROPERTY(QString activeProjectId READ activeProjectId NOTIFY activeChanged)
    Q_PROPERTY(QVariantMap activeProject READ activeProject NOTIFY changed)
    // The item on the centre page (an index into the open project's items, -1 = none) and the item itself.
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeChanged)
    Q_PROPERTY(QVariantMap activeItem READ activeItem NOTIFY activeChanged)
    // Projects opened lately (newest first) - empty unless there are at least two.
    Q_PROPERTY(QVariantList recent READ recent NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)

public:
    static ProjectService &instance();
    static ProjectService *create(QQmlEngine *engine, QJSEngine *jsEngine);
    ~ProjectService() override;

    QVariantList tree() const { return tree_; }
    QString activeProjectId() const { return activeId_; }
    QVariantMap activeProject() const { return active_; }
    int activeIndex() const { return activeIndex_; }
    QVariantMap activeItem() const;
    QVariantList recent() const { return recent_; }
    bool ready() const { return library_ != nullptr; }

    // ---- the tree ----
    Q_INVOKABLE QString createProject(const QString &name = QString(), const QString &parentFolder = QString());   // opens it
    Q_INVOKABLE QString createFolder(const QString &name = QString(), const QString &parentFolder = QString());
    Q_INVOKABLE bool rename(const QString &id, const QString &name);
    Q_INVOKABLE QString duplicateProject(const QString &id);
    Q_INVOKABLE bool moveNode(const QString &id, const QString &newParent);
    Q_INVOKABLE bool deleteNode(const QString &id);
    Q_INVOKABLE bool setArchived(const QString &projectId, bool archived);

    // ---- the open project ----
    Q_INVOKABLE bool openProject(const QString &id);
    Q_INVOKABLE void closeProject();
    Q_INVOKABLE bool setSectionsLocked(bool locked);
    Q_INVOKABLE bool setSectionsCollapsed(bool collapsed);
    // Puts the item at `index` on the centre page (-1 = none).
    Q_INVOKABLE void selectItem(int index);
    Q_INVOKABLE bool removeItem(int index);
    Q_INVOKABLE bool renameItem(int index, const QString &name);
    Q_INVOKABLE bool setItemLayout(int index, const QString &layout);
    Q_INVOKABLE bool setItemColor(int index, const QString &color);
    Q_INVOKABLE bool addSection(const QString &name = QString(), int index = -1);
    // Moves the items at `indexes` together to `position` (an index in the list as it is now).
    Q_INVOKABLE bool moveItems(const QVariantList &indexes, int position);

    // ---- dropping ----
    // Does the engine let this kind of thing be dropped on that area? (Drives the drop highlight.)
    Q_INVOKABLE bool acceptsDrop(const QString &area, const QString &kind, bool reorder = false) const;
    // A drop on the open project (a project is opened first if none is; one is made when there are none): `items` = [{ ref, name, type, meta }].
    // The engine turns it into project items; false + a toast when it refuses.
    Q_INVOKABLE bool dropOnProject(const QString &kind, const QVariantList &items, int index = -1);

signals:
    void changed();
    void activeChanged();

private:
    explicit ProjectService(QObject *parent = nullptr);
    void load();
    void refresh();
    void report(const QString &message) const;

    std::unique_ptr<bps::library::ProjectLibrary> library_;
    QVariantList tree_;
    QVariantList recent_;
    QVariantMap active_;
    QString activeId_;
    int activeIndex_ = -1;
};
