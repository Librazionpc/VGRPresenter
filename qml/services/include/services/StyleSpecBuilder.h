#pragma once

#include <QString>

#include "modules/presentation/PresentationTypes.hpp"

class StyleListModel;

// WHO A STYLE LOOKS LIKE TO THE ENGINE.
//
// The engine renders from one thing: the OutputStyleSpec the UI pushes for an
// output (PresentationTypes.hpp). Composing that spec is not a formatting
// detail — it carries the style's own background rule (a picked template
// design's background adopting into the style), the per-family template blocks
// the engine's family branch gates on, and the per-family inherit rule — so the
// preview that draws a monitor tile has to compose the SAME spec the outputs
// render, or the tile shows a picture no output is sending.
//
// It used to be composed three times: twice in OutputListModel (the active
// output's spec, and one per other styled output) and once in
// LiveOutputService::outputStyleForPreview for the preview. The three had
// already drifted — the preview resolved its roster through a different path,
// never carried the style's category, and fell back to the Bible layout on a
// different condition than the engine's spec did — which is exactly the
// "that's not what the output shows" class of report. This is that
// composition, once.
namespace StyleSpecBuilder {

// The roster the UI edits: OutputListModel's adopted hand-over first (the
// object the Styles screen writes through), StyleListModel::instance() only as
// the fallback for a run where QML never handed one over (see
// OutputListModel::adoptedStyleRoster). Nullable.
StyleListModel *uiStyleRoster();

// The engine's view of one style ROW of `styles` — the style's fields, its
// per-family template keys, and the blocks of every template design it picks
// baked in. A negative row yields an empty spec.
bps::presentation::OutputStyleSpec specForStyleRow(int row, const StyleListModel *styles);

// The same, resolving the row itself through uiStyleRoster(). An unknown id —
// and "" (an output with no style at all) — yields an EMPTY spec, which the
// engine renders unstyled.
bps::presentation::OutputStyleSpec specForStyleId(const QString &styleId);

} // namespace StyleSpecBuilder
