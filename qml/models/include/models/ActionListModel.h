#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QVariantList>
#include <qqml.h>

// One user-created Action (Settings · Appearance & Actions · "Manage
// actions" — the click/keypress action registry, scoped here to
// activation only). `effectId` names one entry of ActionListModel's own
// catalog (catalogEntries(), mirroring LiveOutputService's real
// Q_INVOKABLEs); `effectValue` is that effect's own parameter when it takes
// one (jump_to_group's group name).
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

// QML singleton backing Settings · Appearance & Actions' "Manage actions"
// dialog and the Action create/edit dialog. No seeded defaults (unlike
// GroupListModel's eight) — an empty roster is the honest starting point;
// every Action here is something the operator chose to wire up.
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

    // Creates a blank action (an empty name, no effect, no shortcut) and
    // returns its row — the Edit dialog opens straight onto it, same as
    // GroupListModel/StyleListModel's own addX() + edit-immediately pattern.
    Q_INVOKABLE int addAction();
    Q_INVOKABLE void removeAction(int index);
    Q_INVOKABLE void setName(int index, const QString &name);
    Q_INVOKABLE void setIcon(int index, const QString &icon);
    Q_INVOKABLE void setEffect(int index, const QString &effectId, const QString &effectValue);
    Q_INVOKABLE void setSteps(int index, const QVariantList &steps);
    Q_INVOKABLE void setVisibleInFunctions(int index, bool visible);
    // "" clears it. Empty when another action (or a group — the two
    // registries share ONE keyboard, see Main.qml's dispatch) already owns
    // the chord.
    Q_INVOKABLE void setShortcut(int index, const QString &shortcut);
    // "" clears the custom activation.
    Q_INVOKABLE void setTrigger(int index, const QString &triggerId);

    Q_INVOKABLE QVariantMap getAction(int index) const;
    Q_INVOKABLE int rowForId(const QString &id) const;
    // Every (shortcut -> action id) pair that has one — Main.qml's dispatch
    // reads this the same way it reads GroupListModel::shortcutMap().
    Q_INVOKABLE QVariantMap shortcutMap() const;

    // THE CATALOG: every real effect an Action can run, grouped for the
    // picker, grouped by section. Each entry:
    // { id, name, icon, section, needsGroup }. `needsGroup` effects
    // (jump_to_group) prompt for a group name from GroupListModel's own
    // roster instead of taking a free value — there is nothing else a
    // "jump" could mean here.
    Q_INVOKABLE QVariantList catalogEntries() const;
    // THE TRIGGERS (FreeShow's custom activations): every app event an action
    // can be bound to instead of a keypress. Each entry: { id, name, icon }.
    // The ids are what ActionStore persists and what the Functions pane reads
    // back, so the label/icon table lives HERE rather than in whichever dialog
    // happens to draw it.
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
