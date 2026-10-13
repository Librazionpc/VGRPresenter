#pragma once

// What the Scripture tab and The Table tab share: both fill a chosen scripture-style template with picked passages (verses, or a sermon's
// paragraphs) through the ENGINE's slide builder (bps::presentation::FillBookSlides). Only the source of the words, the tab's own
// options and the kind of template differ, so everything else lives here once.

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "modules/presentation/BookFill.hpp"

#include <vector>

namespace SlideBuilder {

// One tab's identity in the shared template library and in the settings.
struct Profile {
    const char *contentType;       // the template type it lists: "scripture" | "table"
    const char *templateSetting;   // the setting that keeps its choice: "scripture.template" | "table.template"
    const char *defaultTemplate;   // the id of its plain layout
};

// The blocks of a template from the engine's template library. LAYOUT ONLY:
// a template supplies no background. Baking the design's own background into
// every generated slide made the template's colour outrank the OUTPUT STYLE's
// (an explicitly transparent style still rendered opaque), so the background
// is the style's / slide's business alone — see SceneBuilder's precedence.
std::vector<bps::presentation::ContentBlock> templateBlocks(const QString &id);

QVariantList blocksToVariants(const std::vector<bps::presentation::ContentBlock> &blocks);

// The chosen template - or the default when nothing is chosen or the chosen one has been deleted.
QString templateId(const Profile &profile);
QVariantList templates(const Profile &profile);          // [{ id, name, color }] of this tab's type only
QString templateName(const QString &id);
// false: the template names no placeholder of the ONE vocabulary the fill knows —
// {scripture_*}, {table_*} or {meta_*} (BookFill::HasPlaceholders). The wording
// here used to say {scripture_*} alone, which is how a layout written entirely
// in the table_ spelling came to be read as a plain "old style" one.
bool templateHasValues(const QString &id);

// The first slide the template makes of the source, in the Edit screen's block shape:
// { blocks, background, reference, hasValues, slideCount }. Empty blocks when there is nothing to show.
QVariantMap preview(const QString &templateId, const bps::presentation::BookSource &source,
                    const bps::presentation::BookFillOptions &options);

// Every slide the template makes of the source: [{ title, background, blocks,
// body, fill, contentType }]. `body` and `fill` are the SLIDE'S OWN values —
// the body text, and the whole placeholder vocabulary it was filled with —
// because once a template's slots are filled they no longer say which block was
// the content or what each one used to ask for (see BookFill::BookSlide); the
// engine resolves an output style's own slots from them. `contentType`
// ("scripture" | "table") rides on each slide so
// the output style's own engine template only restyles ITS content family —
// the style's template must not steamroll the other tab's layout.
QVariantList slides(const QString &templateId, const bps::presentation::BookSource &source,
                    const bps::presentation::BookFillOptions &options,
                    const QString &contentType = QString());

} // namespace SlideBuilder
