#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

struct SlideItem
{
    int num = 0;
    int id = 0;
    bool active = false;
    QString tag;
    QString tagColor;
    QString title;
    QString line1;
    QString line2;
    QString ref;
    QString engineId;
    QString categoryId;   // the engine's category assignment (kept so it survives syncs)
};

class SlideListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int activeSlideId READ activeSlideId NOTIFY activeSlideChanged)
    Q_PROPERTY(int activeNum READ activeNum NOTIFY activeSlideChanged)
    Q_PROPERTY(QString activeTag READ activeTag NOTIFY activeSlideChanged)
    Q_PROPERTY(QString activeTagColor READ activeTagColor NOTIFY activeSlideChanged)
    Q_PROPERTY(QString activeTitle READ activeTitle NOTIFY activeSlideChanged)
    Q_PROPERTY(QString activeLine1 READ activeLine1 NOTIFY activeSlideChanged)
    Q_PROPERTY(QString activeLine2 READ activeLine2 NOTIFY activeSlideChanged)
    Q_PROPERTY(QString activeRef READ activeRef NOTIFY activeSlideChanged)

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
        // Stable identity — delegates key per-slide state (EditScreen's
        // canvas archive) on this, never on `num`, which renumbers.
        IdRole,
        EngineIdRole,
        CategoryIdRole,
    };
    Q_ENUM(Role)

    explicit SlideListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addSlide();
    // Returns the new slide's stable id (-1 for an out-of-range index), so
    // the caller can clone per-slide state onto the copy.
    Q_INVOKABLE int duplicateSlide(int index);
    Q_INVOKABLE void removeSlide(int index);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void selectSlide(int index);
    // Stable id of the slide at `index`, or -1 when out of range. Lets QML
    // map a model row to archive keys without a JS-side shadow of the model.
    Q_INVOKABLE int slideIdAt(int index) const;
    Q_INVOKABLE int activeIndex() const;

    // ---- Engine projection ---------------------------------------------------
    // The engine owns the show; this model is its projection for the slide list.
    // The engine's id for the slide at `index` ("" out of range).
    Q_INVOKABLE QString engineIdAt(int index) const;
    Q_INVOKABLE int indexOfEngineId(const QString &engineId) const;   // -1 if absent
    // Roster fields the UI edits ({ title, tag, tagColor, line1, line2, ref }) —
    // what a flush sends to the engine.
    Q_INVOKABLE QVariantMap slideFieldsAt(int index) const;
    Q_INVOKABLE QVariantMap syncFromEngine(const QVariantList &slides);

    Q_INVOKABLE void setActiveTitle(const QString &title);
    Q_INVOKABLE void setActiveLine1(const QString &line1);
    Q_INVOKABLE void setActiveLine2(const QString &line2);
    Q_INVOKABLE void setActiveRef(const QString &ref);

    int activeNum() const;
    int activeSlideId() const;
    QString activeTag() const;
    QString activeTagColor() const;
    QString activeTitle() const;
    QString activeLine1() const;
    QString activeLine2() const;
    QString activeRef() const;

signals:
    void activeSlideChanged();

private:
    void renumber(int fromIndex);
    int nextId();
    const SlideItem *activeItem() const;

    QList<SlideItem> m_slides;
    int m_nextId = 0;
};