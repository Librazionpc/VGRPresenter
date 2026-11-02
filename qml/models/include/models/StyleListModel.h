#pragma once

#include "modules/presentation/PresentationTypes.hpp"   // the family slot indices (kFamilySlot*)

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QVariantList>
#include <qqml.h>

struct StyleItem
{
    QString id;
    QString name;
    QString res;
    // "shows" | "media" | "scripture" | "table" — VGRPresenter's own reduced
    // set, not FreeShow's full Shows/Songs/Scripture/PPT/Video.
    QString contentType = QStringLiteral("shows");
    QString templateKey;
    // Same "transparent" convention as BackgroundColorModal's
    // transparentValue / CanvasItemStyle.backgroundColor.
    QString backgroundColor = QStringLiteral("transparent");
    // The style's background IMAGE (absolute file path, "" = none) — painted
    // cover-fit on air behind all content, FreeShow's backgroundImage.
    QString backgroundImage;
    bool backgroundImageExists = false;
    // FreeShow's clearStyleBackgroundOnText: when the slide itself carries a
    // background colour, the style's background steps aside for it.
    bool clearBackgroundOnText = false;
    bool showShows = true;
    bool showMedia = true;
    bool showScripture = true;
    bool showTable = true;
    bool showOverlay = true;
    QString familyTemplateKeys[bps::presentation::kFamilySlotCount];

    static constexpr int kOverlaySlot = static_cast<int>(bps::presentation::kFamilySlotCount);

    bool showForSlot(int slot) const
    {
        switch (slot) {
        case static_cast<int>(bps::presentation::kFamilySlotMedia):     return showMedia;
        case static_cast<int>(bps::presentation::kFamilySlotScripture): return showScripture;
        case static_cast<int>(bps::presentation::kFamilySlotTable):     return showTable;
        default:                                                        return showShows;
        }
    }
    void setShowForSlot(int slot, bool on)
    {
        switch (slot) {
        case static_cast<int>(bps::presentation::kFamilySlotMedia):     showMedia = on; break;
        case static_cast<int>(bps::presentation::kFamilySlotScripture): showScripture = on; break;
        case static_cast<int>(bps::presentation::kFamilySlotTable):     showTable = on; break;
        default:                                                        showShows = on; break;
        }
    }
    // Shows-only category label (free text).
    QString category;
    QVariantList showCategoryTemplates;
    bool onBoard = false;
};

class StyleListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        ResRole,
        ContentTypeRole,
        TemplateKeyRole,
        BackgroundColorRole,
        BackgroundImageRole,
        ClearBackgroundOnTextRole,
        ShowShowsRole,
        ShowMediaRole,
        ShowScriptureRole,
        ShowTableRole,
        ShowOverlayRole,
        CategoryRole,
        ShowCategoryTemplatesRole,
        // Whether this style has a card on the Routing board (StyleItem::onBoard).
        OnBoardRole,
    };
    Q_ENUM(Role)

    explicit StyleListModel(QObject *parent = nullptr);
    ~StyleListModel() override;

    static StyleListModel *instance();
    static StyleListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addStyle();
    // Put this style on the Routing board / take it off. Never touches the style
    // itself; persisted with the roster (StoredStyle::onBoard).
    Q_INVOKABLE void setOnBoard(int index, bool onBoard);
    Q_INVOKABLE void removeStyle(int index);
    Q_INVOKABLE void duplicateStyle(int index);
    Q_INVOKABLE void renameStyle(int index, const QString &name);
    Q_INVOKABLE void setResolution(int index, const QString &res);
    Q_INVOKABLE void setContentType(int index, const QString &contentType);
    Q_INVOKABLE void setTemplateKey(int index, const QString &templateKey);
    Q_INVOKABLE void setBackgroundColor(int index, const QString &color);
    Q_INVOKABLE void setBackgroundImage(int index, const QString &path);
    Q_INVOKABLE void setClearBackgroundOnText(int index, bool on);
    // contentType: "shows"|"media"|"scripture"|"table"|"overlay".
    Q_INVOKABLE void setShowTemplate(int index, const QString &contentType, bool on);
    Q_INVOKABLE void setFamilyTemplateKey(int index, const QString &contentType,
                                           const QString &templateKey);
    Q_INVOKABLE void setCategory(int index, const QString &category);
    Q_INVOKABLE void setShowCategoryTemplates(int index, const QVariantList &entries);

    Q_INVOKABLE QString pickImageFile() const;   // gui-ok: a modal native picker parks the interface thread on PURPOSE — the user is looking at the dialog, and its own event loop keeps the window painting; the work after the pick (import, scan, copy) already runs off the interface thread

    Q_INVOKABLE QVariantMap getStyle(int index) const;

    // Row for a stable id (-1 = no such style). Outputs store styleId;
    // this is how they map back to rows after a restart shuffles order.
    Q_INVOKABLE int rowForId(const QString &id) const;

signals:
    // Emitted after any persisted change — OutputListModel listens so its
    // styleName roles re-resolve without polluting its own dataChanged.
    void rosterChanged();

private:
    int nextIdNumber() const;
    void save();
    void reloadFromStore();

    QList<StyleItem> m_styles;

    static QPointer<StyleListModel> s_instance;
};