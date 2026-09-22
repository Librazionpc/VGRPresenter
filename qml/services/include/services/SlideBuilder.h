#pragma once

// What the Scripture tab and The Table tab share: both fill a chosen scripture-style template with picked passages (verses, or a sermon's
// paragraphs) through the ENGINE's slide builder (bps::presentation::BuildScriptureSlides). Only the source of the words, the tab's own
// options and the kind of template differ, so everything else lives here once.

#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include "modules/presentation/ScriptureSlides.hpp"

#include <vector>

namespace SlideBuilder {

// One tab's identity in the shared template library and in the settings.
struct Profile {
    const char *contentType;       // the template type it lists: "scripture" | "table"
    const char *templateSetting;   // the setting that keeps its choice: "scripture.template" | "table.template"
    const char *defaultTemplate;   // the id of its plain layout
};

// The blocks (and background) of a template from the engine's template library.
std::vector<bps::presentation::ContentBlock> templateBlocks(const QString &id, QString *background = nullptr);

QVariantList blocksToVariants(const std::vector<bps::presentation::ContentBlock> &blocks);

// The chosen template - or the default when nothing is chosen or the chosen one has been deleted.
QString templateId(const Profile &profile);
QVariantList templates(const Profile &profile);          // [{ id, name, color }] of this tab's type only
QString templateName(const QString &id);
bool templateHasValues(const QString &id);               // false: the template has no {scripture_*} placeholder to fill

// The first slide the template makes of the source, in the Edit screen's block shape:
// { blocks, background, reference, hasValues, slideCount }. Empty blocks when there is nothing to show.
QVariantMap preview(const QString &templateId, const bps::presentation::ScriptureSource &source,
                    const bps::presentation::ScriptureSettings &options);

// Every slide the template makes of the source: [{ title, background, blocks }].
QVariantList slides(const QString &templateId, const bps::presentation::ScriptureSource &source,
                    const bps::presentation::ScriptureSettings &options);

} // namespace SlideBuilder
