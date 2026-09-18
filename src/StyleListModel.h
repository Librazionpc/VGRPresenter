#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <qqml.h>

// One output style — the presentation "theme" an output renders with.
// FreeShow models these in Settings · Styles (Styles.svelte + types/Settings):
// name + resolution, selectable per output. Ours stays at that minimal core;
// our differentiators (content filtering per output) live on OutputListModel,
// not here.
struct StyleItem
{
    QString name;
    QString res;
};

// QML singleton backing the Settings · Outputs "Styles" card. Singleton so a
// future second consumer (output monitor, presenter view) reads the same
// roster without threading a property through the tree.
class StyleListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        ResRole,
    };
    Q_ENUM(Role)

    explicit StyleListModel(QObject *parent = nullptr);
    ~StyleListModel() override;

    // OutputListModel resolves style names through this; QML_SINGLETON gives
    // us exactly one engine-owned instance, so remember it here.
    static StyleListModel *instance() { return s_instance; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addStyle();
    Q_INVOKABLE void removeStyle(int index);
    Q_INVOKABLE void renameStyle(int index, const QString &name);
    Q_INVOKABLE void setResolution(int index, const QString &res);

private:
    QList<StyleItem> m_styles;

    static QPointer<StyleListModel> s_instance;
};
