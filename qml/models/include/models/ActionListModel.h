#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QVariantList>
#include <qqml.h>

struct ActionItem
{
    QString id;
    QString name;
    QString icon = QStringLiteral("flag");
    QVariantList steps;
    QString effectId;   // compatibility summary: first step
    QString effectValue;
    QString shortcut;
    // A custom activation (FreeShow's trigger): the app event that runs this
    // action by itself — one of ActionListModel's own trigger ids, "" = none.
    QString trigger;
    bool visibleInFunctions = true;
};

class ActionListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        IconRole,
        EffectIdRole,
        EffectValueRole,
        ShortcutRole,
        TriggerRole,
        StepsRole,
        VisibleInFunctionsRole,
    };
    Q_ENUM(Role)

    explicit ActionListModel(QObject *parent = nullptr);
    ~ActionListModel() override;

    static ActionListModel *instance();
    static ActionListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE int addAction();
    Q_INVOKABLE void removeAction(int index);
    Q_INVOKABLE void setName(int index, const QString &name);
    Q_INVOKABLE void setIcon(int index, const QString &icon);
    Q_INVOKABLE void setEffect(int index, const QString &effectId, const QString &effectValue);
    Q_INVOKABLE void setSteps(int index, const QVariantList &steps);
    Q_INVOKABLE void setVisibleInFunctions(int index, bool visible);
    Q_INVOKABLE void setShortcut(int index, const QString &shortcut);
    // "" clears the custom activation.
    Q_INVOKABLE void setTrigger(int index, const QString &triggerId);

    Q_INVOKABLE QVariantMap getAction(int index) const;
    Q_INVOKABLE int rowForId(const QString &id) const;
    // Every (shortcut -> action id) pair that has one — Main.qml's dispatch
    // reads this the same way it reads GroupListModel::shortcutMap().
    Q_INVOKABLE QVariantMap shortcutMap() const;

    Q_INVOKABLE QVariantList catalogEntries() const;
    Q_INVOKABLE QVariantList triggerEntries() const;
    // The catalog entry for one effect id (for the "Start action" summary
    // row's icon/name) — {} when effectId is "" or unknown.
    Q_INVOKABLE QVariantMap catalogEntry(const QString &effectId) const;

signals:
    void rosterChanged();

private:
    void save();
    void reloadFromStore();

    QList<ActionItem> m_actions;

    static QPointer<ActionListModel> s_instance;
};