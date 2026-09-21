#include "StyleListModel.h"

QPointer<StyleListModel> StyleListModel::s_instance = nullptr;

StyleListModel::StyleListModel(QObject *parent)
    : QAbstractListModel(parent)
{
    s_instance = this;

    // No hardcoded seed styles — the roster starts empty and the user builds
    // their own (same rule as the slides: nothing coded, everything created).
    // Outputs with no valid styleIndex render as "None" until one is assigned.
}

StyleListModel::~StyleListModel()
{
    if (s_instance == this)
        s_instance = nullptr;
}

int StyleListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_styles.size();
}

QVariant StyleListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_styles.size())
        return {};

    const StyleItem &item = m_styles.at(index.row());
    switch (role) {
    case NameRole: return item.name;
    case ResRole: return item.res;
    case ContentTypeRole: return item.contentType;
    case TemplateKeyRole: return item.templateKey;
    case BackgroundColorRole: return item.backgroundColor;
    default: return {};
    }
}

QHash<int, QByteArray> StyleListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { ResRole, "res" },
        { ContentTypeRole, "contentType" },
        { TemplateKeyRole, "templateKey" },
        { BackgroundColorRole, "backgroundColor" },
    };
}

void StyleListModel::addStyle()
{
    const int row = m_styles.size();
    beginInsertRows(QModelIndex(), row, row);
    m_styles.append({ QStringLiteral("New Style %1").arg(row + 1),
                      QStringLiteral("1920×1080") });
    endInsertRows();
}

void StyleListModel::removeStyle(int index)
{
    if (index < 0 || index >= m_styles.size())
        return;
    // Never remove the last style — every output needs one to point at.
    if (m_styles.size() <= 1)
        return;

    beginRemoveRows(QModelIndex(), index, index);
    m_styles.removeAt(index);
    endRemoveRows();
}

void StyleListModel::renameStyle(int index, const QString &name)
{
    if (index < 0 || index >= m_styles.size())
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_styles[index].name == trimmed)
        return;

    m_styles[index].name = trimmed;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { NameRole });
}

void StyleListModel::setResolution(int index, const QString &res)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].res == res)
        return;

    m_styles[index].res = res;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ResRole });
}

void StyleListModel::setContentType(int index, const QString &contentType)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].contentType == contentType)
        return;

    m_styles[index].contentType = contentType;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { ContentTypeRole });
}

void StyleListModel::setTemplateKey(int index, const QString &templateKey)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].templateKey == templateKey)
        return;

    m_styles[index].templateKey = templateKey;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { TemplateKeyRole });
}

void StyleListModel::setBackgroundColor(int index, const QString &color)
{
    if (index < 0 || index >= m_styles.size())
        return;
    if (m_styles[index].backgroundColor == color)
        return;

    m_styles[index].backgroundColor = color;
    const QModelIndex changed = this->index(index);
    emit dataChanged(changed, changed, { BackgroundColorRole });
}

QVariantMap StyleListModel::getStyle(int index) const
{
    if (index < 0 || index >= m_styles.size())
        return {};

    const StyleItem &item = m_styles.at(index);
    return {
        { "name", item.name },
        { "res", item.res },
        { "contentType", item.contentType },
        { "templateKey", item.templateKey },
        { "backgroundColor", item.backgroundColor },
    };
}
