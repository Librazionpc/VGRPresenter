#include "OutputListModel.h"

OutputListModel::OutputListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_outputs = {
        { QStringLiteral("Main Output"), QStringLiteral("LIVE 1"), QStringLiteral("Screen"),
          QStringLiteral("1920×1080 · 60 Hz"), true },
        { QStringLiteral("Stage Screen"), QStringLiteral("STAGE 1"), QStringLiteral("Screen"),
          QStringLiteral("1280×720 · 60 Hz"), false },
        { QStringLiteral("Nursery Display"), QStringLiteral("NURSERY"), QStringLiteral("Screen"),
          QStringLiteral("1280×800 · 60 Hz"), false },
        { QStringLiteral("Stream Overlay"), QStringLiteral("OBS FEED"), QStringLiteral("Stream"),
          QStringLiteral("1080p30 · NDI"), false },
    };
}

int OutputListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_outputs.size();
}

QVariant OutputListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_outputs.size())
        return {};

    const OutputItem &item = m_outputs.at(index.row());
    switch (role) {
    case NameRole: return item.name;
    case BadgeRole: return item.badge;
    case KindRole: return item.kind;
    case ResRole: return item.res;
    case ActiveRole: return item.active;
    default: return {};
    }
}

QHash<int, QByteArray> OutputListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { BadgeRole, "badge" },
        { KindRole, "kind" },
        { ResRole, "res" },
        { ActiveRole, "active" },
    };
}

void OutputListModel::addOutput()
{
    const int row = m_outputs.size();
    beginInsertRows(QModelIndex(), row, row);
    OutputItem item;
    item.name = QStringLiteral("New Output %1").arg(row + 1);
    item.badge = QStringLiteral("OUT %1").arg(row + 1);
    item.kind = QStringLiteral("Screen");
    item.res = QStringLiteral("—");
    m_outputs.append(item);
    endInsertRows();
}

void OutputListModel::removeOutput(int index)
{
    if (index < 0 || index >= m_outputs.size())
        return;

    beginRemoveRows(QModelIndex(), index, index);
    m_outputs.removeAt(index);
    endRemoveRows();
}

void OutputListModel::setActive(int index)
{
    if (index < 0 || index >= m_outputs.size())
        return;

    for (int i = 0; i < m_outputs.size(); ++i) {
        const bool shouldBeActive = (i == index);
        if (m_outputs[i].active != shouldBeActive) {
            m_outputs[i].active = shouldBeActive;
            const QModelIndex changed = this->index(i);
            emit dataChanged(changed, changed, { ActiveRole });
        }
    }
}
