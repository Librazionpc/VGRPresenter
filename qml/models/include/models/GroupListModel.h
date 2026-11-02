#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <qqml.h>

struct GroupItem
{
    QString id;
    QString name;
    QString color = QStringLiteral("#5825f5");
    // A single letter (uppercase by convention), or "" for none. Live: press
    // it during GO LIVE to jump to the next slide carrying this group.
    QString shortcut;
    QString templateId;
    // The eight ship-with-the-app groups. Renaming one clears this (the same
    // "editing a default detaches it" rule the rest of the roster follows).
    bool isDefault = false;
};

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

    static GroupListModel *instance();
    static GroupListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addGroup();
    Q_INVOKABLE void removeGroup(int index);
    Q_INVOKABLE void renameGroup(int index, const QString &name);
    Q_INVOKABLE void setColor(int index, const QString &color);
    Q_INVOKABLE void setShortcut(int index, const QString &shortcut);
    Q_INVOKABLE void setTemplate(int index, const QString &templateId);
    Q_INVOKABLE QString templateForTag(const QString &tag) const;
    // Restores the eight built-ins, discarding every custom group and
    // override — the "more options" Reset button in the dialog.
    Q_INVOKABLE void resetToDefaults();

    // Snapshot for the dialog's rows — one QVariantMap with the same keys as
    // the role names (same convention as StyleListModel::getStyle).
    Q_INVOKABLE QVariantMap getGroup(int index) const;
    Q_INVOKABLE int rowForId(const QString &id) const;
    Q_INVOKABLE QVariantMap shortcutMap() const;
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