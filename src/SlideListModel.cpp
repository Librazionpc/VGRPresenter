#include "SlideListModel.h"

SlideListModel::SlideListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_slides = {
        { 1, true, QStringLiteral("SUNDAY SERVICE"), QStringLiteral("#9b8ff5"),
          QStringLiteral("WELCOME HOME"),
          QStringLiteral("“For where two or three gather in my name,"),
          QStringLiteral("there am I with them.”"),
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

void SlideListModel::duplicateSlide(int index)
{
    if (index < 0 || index >= m_slides.size())
        return;

    const int insertAt = index + 1;
    beginInsertRows(QModelIndex(), insertAt, insertAt);
    SlideItem copy = m_slides.at(index);
    copy.active = false; // the copy lands next to the original, not selected
    m_slides.insert(insertAt, copy);
    endInsertRows();

    renumber(insertAt);
}

void SlideListModel::removeSlide(int index)
{
    if (index < 0 || index >= m_slides.size())
        return;

    const bool removedWasActive = m_slides.at(index).active;

    beginRemoveRows(QModelIndex(), index, index);
    m_slides.removeAt(index);
    endRemoveRows();

    renumber(index);

    // Keep exactly one slide active (if any remain) so the canvas always
    // has something to show — pick the row that took the removed one's
    // place, falling back to the new last row.
    if (removedWasActive && !m_slides.isEmpty()) {
        const int fallback = qMin(index, m_slides.size() - 1);
        selectSlide(fallback);
    } else if (removedWasActive) {
        emit activeSlideChanged();
    }
}

void SlideListModel::selectSlide(int index)
{
    if (index < 0 || index >= m_slides.size())
        return;

    bool activeChanged = false;
    for (int i = 0; i < m_slides.size(); ++i) {
        const bool shouldBeActive = (i == index);
        if (m_slides[i].active != shouldBeActive) {
            m_slides[i].active = shouldBeActive;
            activeChanged = true;
            const QModelIndex changed = this->index(i);
            emit dataChanged(changed, changed, { ActiveRole });
        }
    }
    if (activeChanged)
        emit activeSlideChanged();
}

void SlideListModel::renumber(int fromIndex)
{
    for (int i = fromIndex; i < m_slides.size(); ++i) {
        m_slides[i].num = i + 1;
        const QModelIndex changed = this->index(i);
        emit dataChanged(changed, changed, { NumRole });
    }
}

int SlideListModel::activeIndex() const
{
    for (int i = 0; i < m_slides.size(); ++i) {
        if (m_slides.at(i).active)
            return i;
    }
    return -1;
}

const SlideItem *SlideListModel::activeItem() const
{
    const int i = activeIndex();
    return i >= 0 ? &m_slides.at(i) : nullptr;
}

void SlideListModel::setActiveTitle(const QString &title)
{
    const int i = activeIndex();
    if (i < 0 || m_slides[i].title == title)
        return;
    m_slides[i].title = title;
    const QModelIndex changed = index(i);
    emit dataChanged(changed, changed, { TitleRole });
    emit activeSlideChanged();
}

void SlideListModel::setActiveLine1(const QString &line1)
{
    const int i = activeIndex();
    if (i < 0 || m_slides[i].line1 == line1)
        return;
    m_slides[i].line1 = line1;
    const QModelIndex changed = index(i);
    emit dataChanged(changed, changed, { Line1Role });
    emit activeSlideChanged();
}

void SlideListModel::setActiveLine2(const QString &line2)
{
    const int i = activeIndex();
    if (i < 0 || m_slides[i].line2 == line2)
        return;
    m_slides[i].line2 = line2;
    const QModelIndex changed = index(i);
    emit dataChanged(changed, changed, { Line2Role });
    emit activeSlideChanged();
}

void SlideListModel::setActiveRef(const QString &ref)
{
    const int i = activeIndex();
    if (i < 0 || m_slides[i].ref == ref)
        return;
    m_slides[i].ref = ref;
    const QModelIndex changed = index(i);
    emit dataChanged(changed, changed, { RefRole });
    emit activeSlideChanged();
}

int SlideListModel::activeNum() const
{
    const SlideItem *item = activeItem();
    return item ? item->num : 0;
}

QString SlideListModel::activeTag() const
{
    const SlideItem *item = activeItem();
    return item ? item->tag : QString();
}

QString SlideListModel::activeTagColor() const
{
    const SlideItem *item = activeItem();
    return item ? item->tagColor : QString();
}

QString SlideListModel::activeTitle() const
{
    const SlideItem *item = activeItem();
    return item ? item->title : QString();
}

QString SlideListModel::activeLine1() const
{
    const SlideItem *item = activeItem();
    return item ? item->line1 : QString();
}

QString SlideListModel::activeLine2() const
{
    const SlideItem *item = activeItem();
    return item ? item->line2 : QString();
}

QString SlideListModel::activeRef() const
{
    const SlideItem *item = activeItem();
    return item ? item->ref : QString();
}
