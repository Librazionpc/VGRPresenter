#include "services/StyleSpecBuilder.h"

#include "FamilySlots.h"

#include <QLatin1String>
#include <QVariantList>
#include <QVariantMap>

#include "models/OutputListModel.h"
#include "models/StyleListModel.h"
#include "services/DesignLibraryService.h"
#include "services/ShowConverter.h"

namespace {

namespace bp = bps::presentation;

// THE TEMPLATE'S OWN BACKGROUND WINS when the style's colour is empty or
// transparent: the template design IS the look the user picked (Big Bold
// carries #000000) — a transparent style colour must not steamroll it. The
// style's colour still wins when the style actually SETS one (and the slide's
// own background beats both, in the engine's precedence).
void adoptTemplateBackground(const QVariantMap &design, bp::OutputStyleSpec &spec)
{
    const QString bg = design.value(QStringLiteral("background")).toString();
    if (!bg.isEmpty() && bg != QLatin1String("transparent")
        && (spec.backgroundColor.empty() || spec.backgroundColor == "transparent"))
        spec.backgroundColor = bg.toStdString();
}

// A legacy preset key ("lowerThird"...) rides StyleBuilder::LayoutFor — nothing
// to bake. Anything else is an ENGINE TEMPLATE DESIGN id ("tpl-…", picked from
// the Template library): copy that design's blocks into the spec so the engine
// renders the template as the style's layout. The engine never reads the
// library itself — the pushed spec is the whole truth — so this is also what
// makes a template edit reach the on-air output (OutputListModel re-pushes on
// TemplateLibraryService::changed).
void bakeWholeStyleTemplate(bp::OutputStyleSpec &spec)
{
    const QString key = QString::fromStdString(spec.templateKey);
    if (key.isEmpty() || key.startsWith(QLatin1String("tpl-")) == false)
        return;
    const QVariantMap design = TemplateLibraryService::instance().design(key);
    const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
    if (blocks.isEmpty())
        return;   // unknown/deleted id: keep the plain-layout fallback
    adoptTemplateBackground(design, spec);
    spec.templateBlocks.reserve(blocks.size());
    for (const QVariant &b : blocks)
        spec.templateBlocks.push_back(ShowConverter::blockFromVariant(b.toMap()));
}

// Bakes each per-family template design's blocks into the spec's
// familyTemplateBlocks slots (no-op for ""/preset family keys), then runs the
// two rules that decide what a family slot left empty renders through.
//
// INHERIT: a family slot left on "None" rides the style's WHOLE-STYLE design
// (its documented contract — StyleStore.hpp: an empty key "inherits the
// style's whole-style templateKey"). The old no-inherit behaviour stranded
// exactly the families a style didn't name: with the style's contentType on
// "shows", SceneBuilder's family gate excluded the whole-style template from
// scripture/table slides, so the slide's own tab template (default font,
// default autoSize) rendered instead and the style's font rules never
// reached the output — the live "NDI doesn't see the font rules I set" and
// "scripture always shrinks" reports. Binding is family-agnostic ("text" =
// the content, "ref" = the reference — SceneBuilder::BindTemplateBlocks), so
// any design carries any family's content.
//
// SHOWS additionally keeps a Bible fallback (the Scripture design) for when
// even the whole-style template is unset or a legacy preset: show content
// (imported shows, Quick Lyrics — slides carry no family tag) must never
// silently fall back to the old plain layout. An explicit pick always wins
// over both.
void bakeFamilyTemplates(bp::OutputStyleSpec &spec)
{
    for (size_t i = 0; i < 4; ++i) {
        const QString famKey = QString::fromStdString(spec.familyTemplateKeys[i]);
        if (famKey.isEmpty() || famKey.startsWith(QLatin1String("tpl-")) == false)
            continue;   // "" = none for this family; a preset key rides LayoutFor — no bake
        const QVariantMap design = TemplateLibraryService::instance().design(famKey);
        const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
        if (blocks.isEmpty())
            continue;   // unknown/deleted id: the family falls back per SceneBuilder
        // Same template-bg-wins rule as the whole-style bake: a family template
        // with its own background (the Table design's #000000) adopts into the
        // style when the style's colour is unset/transparent. This is what
        // made Table render black while Shows/Scripture (transparent-bg
        // designs) showed a tinted fallback instead.
        adoptTemplateBackground(design, spec);
        spec.familyTemplateBlocks[i].reserve(blocks.size());
        for (const QVariant &b : blocks)
            spec.familyTemplateBlocks[i].push_back(ShowConverter::blockFromVariant(b.toMap()));
    }
    // The inherit pass: only when the whole-style template is an ENGINE
    // DESIGN (a legacy preset key can't be baked — the engine renders those
    // via LayoutFor with the family key, which is empty here by definition).
    const QString wholeKey = QString::fromStdString(spec.templateKey);
    if (wholeKey.startsWith(QLatin1String("tpl-"))) {
        const QVariantMap whole = TemplateLibraryService::instance().design(wholeKey);
        const QVariantList wholeBlocks = whole.value(QStringLiteral("blocks")).toList();
        if (!wholeBlocks.isEmpty()) {
            for (size_t i = 0; i < 4; ++i) {
                if (!spec.familyTemplateKeys[i].empty() || !spec.familyTemplateBlocks[i].empty())
                    continue;   // explicit pick (baked above) always wins
                spec.familyTemplateBlocks[i].reserve(wholeBlocks.size());
                for (const QVariant &b : wholeBlocks)
                    spec.familyTemplateBlocks[i].push_back(ShowConverter::blockFromVariant(b.toMap()));
            }
        }
    }
    // The Bible fallback for show content (see the function comment): the
    // baked blocks alone gate the engine's family branch — the shows key
    // stays "" so the roster still reads "None".
    if (spec.familyTemplateBlocks[0].empty()
        && QString::fromStdString(spec.familyTemplateKeys[0]).isEmpty()
        && !wholeKey.startsWith(QLatin1String("tpl-"))) {
        const QVariantMap design = TemplateLibraryService::instance().design(QStringLiteral("tpl-scripture"));
        const QVariantList blocks = design.value(QStringLiteral("blocks")).toList();
        if (!blocks.isEmpty()) {
            spec.familyTemplateBlocks[0].reserve(blocks.size());
            for (const QVariant &b : blocks)
                spec.familyTemplateBlocks[0].push_back(ShowConverter::blockFromVariant(b.toMap()));
        }
    }
}

} // namespace

namespace StyleSpecBuilder {

StyleListModel *uiStyleRoster()
{
    // The roster the UI edits — read through the STATIC hand-over first, not
    // just one object's own copy: QML builds its own StyleListModel too (the
    // engine default-constructs the registered singleton instead of calling
    // create()), so instance() and the object Main.qml hands the roster to are
    // two different rosters. A reader addressing the wrong one falls back to
    // StyleListModel::instance() — a second roster holding styles as they were
    // at boot, which is how a monitor tile came to compose a preview from a
    // stale template/background while the real output rendered the edit.
    if (StyleListModel *adopted = OutputListModel::adoptedStyleRoster())
        return adopted;
    // The C++ singleton is only the fallback for a run where QML never handed
    // one over (a headless test, or a read before the hand-over).
    return StyleListModel::instance();
}

bps::presentation::OutputStyleSpec specForStyleRow(int row, const StyleListModel *styles)
{
    bps::presentation::OutputStyleSpec spec;
    if (!styles || row < 0 || row >= styles->rowCount())
        return spec;
    const QVariantMap style = styles->getStyle(row);
    spec.name = style.value(QStringLiteral("name")).toString().toStdString();
    spec.contentType = style.value(QStringLiteral("contentType")).toString().toStdString();
    spec.templateKey = style.value(QStringLiteral("templateKey")).toString().toStdString();
    spec.backgroundColor = style.value(QStringLiteral("backgroundColor")).toString().toStdString();
    spec.backgroundImage = style.value(QStringLiteral("backgroundImage")).toString().toStdString();
    spec.clearBackgroundOnText =
        style.value(QStringLiteral("clearBackgroundOnText")).toBool();
    // The four content families' gates and template picks, walked through the ONE
    // family table (FamilySlots.h) — the property names and the slot order live
    // there, so neither is spelled again here. The spec's overlay gate is the
    // fifth switch on a DIFFERENT axis (the Overlays pane's compositor layer), so
    // it is not one of the four and is read by name.
    for (int slot = 0; slot < FamilySlots::kCount; ++slot) {
        const FamilySlots::Family &fam = FamilySlots::Rows()[static_cast<std::size_t>(slot)];
        spec.SetFamilyGate(static_cast<std::size_t>(slot),
                           style.value(QString::fromLatin1(fam.showProperty)).toBool());
        spec.familyTemplateKeys[static_cast<std::size_t>(slot)] =
            style.value(QString::fromLatin1(fam.templateProperty)).toString().toStdString();
    }
    spec.showOverlay = style.value(QStringLiteral("showOverlay")).toBool();
    spec.category = style.value(QStringLiteral("category")).toString().toStdString();
    bakeWholeStyleTemplate(spec);
    bakeFamilyTemplates(spec);
    return spec;
}

bps::presentation::OutputStyleSpec specForStyleId(const QString &styleId)
{
    StyleListModel *styles = uiStyleRoster();
    if (!styles)
        return {};
    return specForStyleRow(styles->rowForId(styleId), styles);
}

} // namespace StyleSpecBuilder
