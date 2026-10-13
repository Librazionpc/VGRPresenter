#pragma once

#include "modules/presentation/PresentationTypes.hpp"   // the family slot indices (kFamilySlot*)

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
    // ALLOWS that content type. NEW styles start with everything ACTIVE —
    // matching StyleStore's JSON read default (missing keys read true) and
    // OutputStyleSpec's own C++ defaults, so a fresh "New Style" never
    // silently refuses every content family on the real output (the
    // all-false default made BuildGatedSlideScene render BACKGROUND-ONLY
    // scenes — styled bg, no text — while the QML tile kept showing text
    // through its gate-free DesignPreview path). A style that genuinely
    // wants no content is meaningless; the engine reads all-false as
    // "unconfigured" and admits everything anyway
    // (SlideRules::StyleAdmits — the one gate implementation, which the
    // engine's builds and the live preview both call).
    bool showShows = true;
    bool showMedia = true;
    bool showScripture = true;
    bool showTable = true;
    // A fifth gate, same on/off shape, but for the Overlays pane's
    // compositor layer rather than a slide content family — no template
    // slot of its own (see familyTemplateKeys below, still sized 4).
    bool showOverlay = true;
    // PER-FAMILY TEMPLATE KEYS ("" = no pick for that family, which then
    // INHERITS the style's whole-style template — StyleStore.hpp's documented
    // contract, baked engine-side by StyleSpecBuilder: an explicit pick always
    // wins, and the SHOWS slot additionally falls back to the Bible template
    // when even the whole-style template is unset or a legacy preset). The
    // style renders each content family through its OWN template:
    // shows/media/scripture/table, indexed by the engine's family slots
    // (presentation::kFamilySlot*, whose order this array follows).
    QString familyTemplateKeys[bps::presentation::kFamilySlotCount];

    // The overlay gate is NOT one of the four families (it gates the Overlays
    // pane's compositor layer, not a slide's content), so the engine stores it at
    // index kFamilySlotCount of its showTemplates array. Named here so no caller
    // spells that index out (the old code wrote `[4]` in load and save).
    static constexpr int kOverlaySlot = static_cast<int>(bps::presentation::kFamilySlotCount);

    // The gate for one family SLOT rather than by member name, so the roster's
    // load / save / setters can walk the families with one loop instead of
    // spelling the same four-way choice out three times (the named members stay:
    // they are the roles and the variant keys QML reads).
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
        ShowOverlayRole,
        CategoryRole,
    };
    Q_ENUM(Role)

    explicit StyleListModel(QObject *parent = nullptr);
    ~StyleListModel() override;

    // OutputListModel resolves style names through this.
    //
    // EAGER, not lazy (fixed 2026-10-01). These model singletons used to be
    // instantiated by QML itself, on first property access — so a C++ reader
    // that ran before any QML binding touched the type got a null and
    // silently did nothing (the monitor tile showed the transparency
    // checkerboard because the output's style resolved as "no style").
    // main.cpp registers THIS object with QML, so the two sides can no longer
    // disagree about which roster exists.
    static StyleListModel *instance();
    // The QML-side factory contract. The engine does NOT call it (measured
    // 2026-10-11: the generated registration is qmlRegisterTypesAndRevisions
    // and the engine default-constructs its own instance), which is why QML and
    // C++ can end up on two different rosters. That is why Main.qml hands the
    // QML-side instance to OutputListModel at startup
    // (OutputListModel::adoptStyleRoster) — the roster C++ readers use is the
    // one the UI actually edits, whatever object the engine built first.
    static StyleListModel *create(QQmlEngine *engine, QJSEngine *jsEngine);

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
    // contentType: "shows"|"media"|"scripture"|"table"|"overlay".
    Q_INVOKABLE void setShowTemplate(int index, const QString &contentType, bool on);
    // The per-family template pick ("" clears the family to no template —
    // Shows then falls back to the Bible template). contentType:
    // "shows"|"media"|"scripture"|"table".
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
    // Re-reads the roster from the kernel's StyleStore. Called at construction
    // AND on EngineBridge::bootedChanged: this singleton is built at QML load,
    // BEFORE the deferred boot has opened kernel.json, so the first read is
    // empty — without the re-hydrate the user's styles vanish on every launch
    // (and the first mutation flushed the empty roster over the real one).
    void reloadFromStore();

    QList<StyleItem> m_styles;

    static QPointer<StyleListModel> s_instance;
};
