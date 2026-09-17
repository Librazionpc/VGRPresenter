#pragma once

#include <QAbstractListModel>
#include <QList>
#include <qqml.h>

struct OutputItem
{
    QString name;
    QString badge;
    QString kind;
    QString res;
    bool active = false;
};

// Mock CRUD backend for the app's output roster (screens/streams a show can
// be sent to). A QML singleton — not a per-screen instance — because both
// Settings · Outputs (OutputsScreen.qml) and the Edit screen's output-
// monitor grid (EditScreen.qml) render the SAME roster from two different,
// unrelated branches of the QML tree; a plain property couldn't be handed
// down to both without threading it through Main.qml. Adding/removing an
// output or flipping which one is live now shows up in both places at once
// instead of two hand-typed arrays silently drifting apart.
class OutputListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        BadgeRole,
        KindRole,
        ResRole,
        ActiveRole,
    };
    Q_ENUM(Role)

    explicit OutputListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addOutput();
    Q_INVOKABLE void removeOutput(int index);
    Q_INVOKABLE void setActive(int index);

private:
    QList<OutputItem> m_outputs;
};
