#include "SlideListModel.h"

SlideListModel::SlideListModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_nextId(0)
{
    // Starts empty: no seeded/hardcoded slides. The user builds the roster
    // with "Add slide" (which auto-selects the new slide). This model owns
    // roster metadata (num/id/tag/tagColor/active) only; slide content lives
    // in EditScreen.qml's per-slide canvas archive.
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
    case IdRole: return item.id;
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
        { IdRole, "slideId" },
    };
}

void SlideListModel::addSlide()
{
    const int row = m_slides.size();
    beginInsertRows(QModelIndex(), row, row);
    SlideItem item;
    item.num = row + 1;
    item.id = nextId();
    m_slides.append(item);
    endInsertRows();

    // Auto-select the new slide — after "Add slide" the user edits it, so
    // the canvas must already be showing it.
    selectSlide(row);
}

int SlideListModel::duplicateSlide(int index)
{
    if (index < 0 || index >= m_slides.size())
        return -1;

    const int insertAt = index + 1;
    beginInsertRows(QModelIndex(), insertAt, insertAt);
    SlideItem copy = m_slides.at(index);
    copy.num = 0;
    copy.id = nextId(); // fresh id — the copy is its own slide, not an alias
    copy.active = false; // the copy lands next to the original, not selected
    m_slides.insert(insertAt, copy);
    endInsertRows();

    renumber(insertAt);
    return copy.id;
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

int SlideListModel::nextId()
{
    return ++m_nextId;
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

int SlideListModel::activeSlideId() const
{
    const SlideItem *item = activeItem();
    return item ? item->id : -1;
}

int SlideListModel::slideIdAt(int index) const
{
    if (index < 0 || index >= m_slides.size())
        return -1;
    return m_slides.at(index).id;
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
