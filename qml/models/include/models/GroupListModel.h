#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <qqml.h>

// One group — a named song-section tag ("verse", "chorus"...) a slide can
// carry (FreeShow's groups store, Settings · Look & Feel · Manage groups).
// `id` is the stable identity a slide's own tag round-trips against nothing
// here yet (Phase 1: the registry only) — a future slide-tag picker resolves
// by NAME, the same way FreeShow's own slide.group is a plain string, not an
// id reference; the registry's job is colour + shortcut + (future) template
// for whichever name a slide happens to carry.
struct GroupItem
{
    QString id;
    QString name;
    QString color = QStringLiteral("#5825f5");
    // A single letter (uppercase by convention), or "" for none. Live: press
    // it during GO LIVE to jump to the next slide carrying this group.
    QString shortcut;
    // The layout template this group's slides use — an engine design id ("tpl-…")
    // or "" for none. This is what makes a group more than a colour: a slide
    // carrying this tag composes through THIS template instead of the style's
    // own pick (StyleSpecBuilder::specForStyleId takes the tag).
    QString templateId;
    // The eight ship-with-the-app groups. Renaming one clears this (the same
    // "editing a default detaches it" rule the rest of the roster follows).
    bool isDefault = false;
};

// QML singleton backing Settings · Look & Feel's "Manage groups" dialog.
// Persistence contract: the constructor hydrates from GroupStore; a missing
// or corrupt document — or a document that has NEVER been saved — seeds the
// eight built-in defaults (FreeShow's own set: break, bridge, chorus, intro,
// outro, pre_chorus, tag, verse) and saves them once, so a fresh install
// shows the same starter roster FreeShow does instead of an empty table.
class GroupListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        ColorRole,
        ShortcutRole,
        TemplateRole,
        DefaultRole,
    };
    Q_ENUM(Role)

    explicit GroupListModel(QObject *parent = nullptr);
    ~GroupListModel() override;

    // EAGER, not lazy — see StyleListModel's own comment for the exact bug
    // this avoids (a C++ reader running before any QML binding touched the
    // type got a null and silently did nothing).
    static GroupListModel *instance();
    static GroupListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addGroup();
    Q_INVOKABLE void removeGroup(int index);
    Q_INVOKABLE void renameGroup(int index, const QString &name);
    Q_INVOKABLE void setColor(int index, const QString &color);
    // "" clears it. Empty when another group already carries it — one
    // shortcut, one group, same uniqueness ManageGroups.svelte's
    // `existingShortcuts` filter enforces.
    Q_INVOKABLE void setShortcut(int index, const QString &shortcut);
    // "" clears it. The id of a template DESIGN ("tpl-…"); an id the template
    // library does not know is still stored — the composition falls back to the
    // style's own pick for it, exactly like a style naming a deleted template.
    Q_INVOKABLE void setTemplate(int index, const QString &templateId);
    // The template a slide carrying `tag` should compose through — "" when the tag
    // names no group, or names one with no template of its own. This is the ONLY
    // reader of the group → template link outside the model (StyleSpecBuilder).
    Q_INVOKABLE QString templateForTag(const QString &tag) const;
    // Restores the eight built-ins, discarding every custom group and
    // override — the "more options" Reset button in the dialog.
    Q_INVOKABLE void resetToDefaults();

    // Snapshot for the dialog's rows — one QVariantMap with the same keys as
    // the role names (same convention as StyleListModel::getStyle).
    Q_INVOKABLE QVariantMap getGroup(int index) const;
    Q_INVOKABLE int rowForId(const QString &id) const;
    // The live-shortcut keymap: every (shortcut letter -> group name) pair
    // that actually has one, for Main.qml's global Shortcut handlers to
    // resolve a keypress against without walking the model themselves.
    Q_INVOKABLE QVariantMap shortcutMap() const;
    // A compact "chorus | outro | verse 1 …" preview string, each name
    // ready to be coloured by its own group — Settings · Look & Feel's
    // "Manage groups" row reads this instead of re-deriving it.
    Q_INVOKABLE QVariantList previewEntries(int limit) const;

signals:
    void rosterChanged();

private:
    void save();
    void reloadFromStore();
    void seedDefaults();

    QList<GroupItem> m_groups;

    static QPointer<GroupListModel> s_instance;
};
