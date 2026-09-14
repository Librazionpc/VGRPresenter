#include "SlideListModel.h"

SlideListModel::SlideListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_slides = {
        { 1, true, QStringLiteral("SUNDAY SERVICE"), QStringLiteral("#9b8ff5"),
          QStringLiteral("WELCOME HOME"),
          QStringLiteral("For where two or three gather in my name,"),
          QStringLiteral("there am I with them."),
          QStringLiteral("— Matthew 18:20") },
        { 2, false, QStringLiteral("WORSHIP"), QStringLiteral("#ff8a3d"),
          QStringLiteral("Great Are You Lord"),
          QStringLiteral("It's Your breath in our lungs"),
          QStringLiteral("so we pour out our praise"), QString() },
        { 3, false, QStringLiteral("WORSHIP"), QStringLiteral("#ff8a3d"),
          QStringLiteral("Way Maker"),
          QStringLiteral("You are the Way Maker"),
          QStringLiteral("Miracle Worker"), QString() },
        { 4, false, QStringLiteral("WORSHIP"), QStringLiteral("#f0b73d"),
          QStringLiteral("Oceans"),
          QStringLiteral("You call me out upon the waters"),
          QStringLiteral("the great unknown"), QString() },
        { 5, false, QString(), QString(), QString(), QString(), QString(), QString() },
        { 6, false, QString(), QString(), QString(), QString(), QString(), QString() },
        { 7, false, QString(), QString(), QString(), QString(), QString(), QString() },
        { 8, false, QString(), QString(), QString(), QString(), QString(), QString() },
    };
}

int SlideListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_slides.size();
}

QVariant SlideListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_slides.size())
        return {};

    const SlideItem &item = m_slides.at(index.row());
    switch (role) {
    case NumRole: return item.num;
    case ActiveRole: return item.active;
    case TagRole: return item.tag;
    case TagColorRole: return item.tagColor;
    case TitleRole: return item.title;
    case Line1Role: return item.line1;
    case Line2Role: return item.line2;
    case RefRole: return item.ref;
    default: return {};
    }
}

QHash<int, QByteArray> SlideListModel::roleNames() const
{
    return {
        { NumRole, "num" },
        { ActiveRole, "active" },
        { TagRole, "tag" },
        { TagColorRole, "tagColor" },
        { TitleRole, "title" },
        { Line1Role, "line1" },
        { Line2Role, "line2" },
        { RefRole, "ref" },
    };
}

void SlideListModel::addSlide()
{
    const int row = m_slides.size();
    beginInsertRows(QModelIndex(), row, row);
    SlideItem item;
    item.num = row + 1;
    m_slides.append(item);
    endInsertRows();
}

void SlideListModel::removeSlide(int index)
{
    if (index < 0 || index >= m_slides.size())
        return;

    beginRemoveRows(QModelIndex(), index, index);
    m_slides.removeAt(index);
    endRemoveRows();

    renumber(index);
}

void SlideListModel::selectSlide(int index)
{
    if (index < 0 || index >= m_slides.size())
        return;

    for (int i = 0; i < m_slides.size(); ++i) {
        const bool shouldBeActive = (i == index);
        if (m_slides[i].active != shouldBeActive) {
            m_slides[i].active = shouldBeActive;
            const QModelIndex changed = this->index(i);
            emit dataChanged(changed, changed, { ActiveRole });
        }
    }
}

void SlideListModel::renumber(int fromIndex)
{
    for (int i = fromIndex; i < m_slides.size(); ++i) {
        m_slides[i].num = i + 1;
        const QModelIndex changed = this->index(i);
        emit dataChanged(changed, changed, { NumRole });
    }
}
