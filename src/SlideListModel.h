#pragma once

#include <QAbstractListModel>
#include <QList>
#include <qqml.h>

struct SlideItem
{
    // Stable identity: `num` is the display position and renumbers whenever
    // slides are removed/inserted, so per-slide state elsewhere in the app
    // (EditScreen's canvas archive) keys on `id`, never on `num`.
    int num = 0;
    int id = 0;
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

    // The canvas (the big slide preview, as opposed to the thumbnail list)
    // renders whichever slide is active. Exposed as properties rather than
    // making QML call get(activeIndex) itself, so the canvas text just
    // binds directly and updates whenever selection changes.
    // The active slide's stable id — the key EditScreen's per-slide canvas
    // archive saves/loads under. -1 when no slide is active (empty roster).
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
    Q_INVOKABLE void selectSlide(int index);
    // Stable id of the slide at `index`, or -1 when out of range. Lets QML
    // map a model row to archive keys without a JS-side shadow of the model.
    Q_INVOKABLE int slideIdAt(int index) const;

    // Inline-editing the canvas text objects (DraggableCanvasText +
    // EditableCanvasLabel in EditScreen.qml) writes back through these
    // rather than the read-only active* properties above.
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
    int activeIndex() const;
    const SlideItem *activeItem() const;

    QList<SlideItem> m_slides;
    int m_nextId = 0;
};
