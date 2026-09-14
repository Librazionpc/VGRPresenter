#pragma once

#include <QAbstractListModel>
#include <QList>
#include <qqml.h>

struct SlideItem
{
    int num = 0;
    bool active = false;
    QString tag;
    QString tagColor;
    QString title;
    QString line1;
    QString line2;
    QString ref;
};

// Mock CRUD backend for the Edit screen's slide list. Seeded with the same
// content the ground-truth Figma export uses, but backed by a real model so
// "Add slide" / selecting a row actually mutates state instead of pointing
// at a static QML array. Stands in for the real show/slide data source.
class SlideListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

public:
    enum Role {
        NumRole = Qt::UserRole + 1,
        ActiveRole,
        TagRole,
        TagColorRole,
        TitleRole,
        Line1Role,
        Line2Role,
        RefRole,
    };
    Q_ENUM(Role)

    explicit SlideListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addSlide();
    Q_INVOKABLE void removeSlide(int index);
    Q_INVOKABLE void selectSlide(int index);

private:
    void renumber(int fromIndex);

    QList<SlideItem> m_slides;
};
