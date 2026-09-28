#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <qqml.h>

// One output style — the presentation "theme" an output renders with
// (FreeShow's Styles, src/types/Settings.ts). `id` is the STABLE identity:
// outputs reference styles by id (OutputItem::styleId) so removing a style
// can't re-point every later output at the wrong theme the way an index
// reference would. The roster is persisted through the kernel's StyleStore
// (DatabaseManager "styles"/"roster" document) and survives restarts.
//
// Deliberately NOT video/audio bus routing or per-output window behavior
// (always-on-top, locked, transparent, geometry/cropping) — those are
// per-OUTPUT concerns (a shared style applied to two outputs shouldn't force
// them to share window behavior), so they live on OutputListModel instead,
// not duplicated here.
struct StyleItem
{
    QString id;
    QString name;
    QString res;
    // "shows" | "media" | "scripture" | "table" — VGRPresenter's own reduced
    // set, not FreeShow's full Shows/Songs/Scripture/PPT/Video.
    QString contentType = QStringLiteral("shows");
    // "lowerThird" | "title" | "sidebar" | "bottomBar" | "fullscreen" — see
    // TemplatePickerModal.qml's own `templates` list, the single source of
    // truth for keys/names/descriptions this just stores a key into. The
    // ENGINE maps the key to a layout preset when the style is on air.
    QString templateKey = QStringLiteral("lowerThird");
    // Same "transparent" convention as BackgroundColorModal's
    // transparentValue / CanvasItemStyle.backgroundColor.
    QString backgroundColor = QStringLiteral("transparent");
    // The style's background IMAGE (absolute file path, "" = none) — painted
    // cover-fit on air behind all content, FreeShow's backgroundImage.
    QString backgroundImage;
    // FreeShow's clearStyleBackgroundOnText: when the slide itself carries a
    // background colour, the style's background steps aside for it.
    bool clearBackgroundOnText = false;
    // Per-content-type gates: a chip ON means an output wearing this style
    // ALLOWS that content type. NEW styles start with everything INACTIVE —
    // the user activates exactly what the style serves (an intuitive explicit
    // choice, not four pre-ticked boxes); existing rosters keep their saved
    // flags (the JSON read defaults missing keys to true).
    bool showShows = false;
    bool showMedia = false;
    bool showScripture = false;
    bool showTable = false;
    // PER-FAMILY TEMPLATE KEYS ("" = inherit templateKey — the whole-style
    // pick). The style renders each content family through its OWN template:
    // shows/media/scripture/table, index order matching the show* flags.
    QString familyTemplateKeys[4];
    // Shows-only category label (free text).
    QString category;
};

// QML singleton backing the Settings · Outputs "Styles" card. Singleton so a
// second consumer (output monitor, presenter view) reads the same roster
// without threading a property through the tree.
//
// Persistence contract: the constructor hydrates from StyleStore (a missing
// or corrupt document reads as an empty roster — never a failed boot), and
// every mutating call saves the whole roster back. Remove/duplicate keep
// outputs consistent: removing a style re-points every output that used it
// to "None" (-1).
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
        CategoryRole,
    };
    Q_ENUM(Role)

    explicit StyleListModel(QObject *parent = nullptr);
    ~StyleListModel() override;

    // OutputListModel resolves style names through this; QML_SINGLETON gives
    // us exactly one engine-owned instance, so remember it here.
    static StyleListModel *instance() { return s_instance; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void addStyle();
    // Only refuses the LAST style when something still points at it — the
    // UI re-points outputs to "None" first (removeStyle also does it
    // model-side, so a stale caller can't orphan a reference).
    Q_INVOKABLE void removeStyle(int index);
    Q_INVOKABLE void duplicateStyle(int index);
    Q_INVOKABLE void renameStyle(int index, const QString &name);
    Q_INVOKABLE void setResolution(int index, const QString &res);
    Q_INVOKABLE void setContentType(int index, const QString &contentType);
    Q_INVOKABLE void setTemplateKey(int index, const QString &templateKey);
    Q_INVOKABLE void setBackgroundColor(int index, const QString &color);
    Q_INVOKABLE void setBackgroundImage(int index, const QString &path);
    Q_INVOKABLE void setClearBackgroundOnText(int index, bool on);
    Q_INVOKABLE void setShowTemplate(int index, const QString &contentType, bool on);
    // The per-family template pick ("" clears the family back to the
    // whole-style templateKey). contentType: "shows"|"media"|"scripture"|"table".
    Q_INVOKABLE void setFamilyTemplateKey(int index, const QString &contentType,
                                           const QString &templateKey);
    Q_INVOKABLE void setCategory(int index, const QString &category);

    // Native image-file picker (png/jpg/webp/...) for the style background
    // image; "" when cancelled. Lives here (not QML) so the engine's dialog
    // service is used, same as every other file picker in the app.
    Q_INVOKABLE QString pickImageFile() const;

    // Snapshot for the Edit Style dialog — one QVariantMap with the same
    // keys as the role names, so QML doesn't keep parallel state (same
    // convention as OutputListModel::getOutput).
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

    QList<StyleItem> m_styles;

    static QPointer<StyleListModel> s_instance;
};
