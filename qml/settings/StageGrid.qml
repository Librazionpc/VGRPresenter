pragma Singleton
import QtQuick

// THE EDIT STAGE'S GRID, IN ONE PLACE.
//
// A design's blocks are stored in STAGE UNITS — the Edit canvas's own coordinate
// space, 754 x 428. Every surface that draws such a block has to turn those units
// into its own pixels, and the ENGINE has to turn them into the output frame's
// pixels. Until this file existed each of them worked the grid out for itself:
// DesignPreview carried its own stageW/stageH, the Edit canvas laid itself out at
// a literal 754x428, and ShowCenter, ReferencePane, SlideListItem and the main
// screen each re-derived the stage's proportion as `width * 428 / 754`. Ten copies
// of the same two numbers is ten chances for a surface to disagree with the frame
// the outputs actually paint — the reason a block could sit correctly in the Edit
// canvas and somewhere else in a monitor tile.
//
// TWO RULES LIVE HERE, and the difference between them is deliberate:
//
//   1. THE FRAME'S RULE (mapX / mapY / mapW / mapH / mapRect). The engine lays a
//      block out AXIS BY AXIS — x and width by the frame's WIDTH, y and height by
//      its HEIGHT (SceneBuilder's block mapping, the one owner of what goes on
//      air). This is full-bleed by construction: a block at the stage's edge
//      reaches the frame's edge, and no letterbox ever appears on a live output.
//      Any surface that is standing in for the FRAME (the canvas's own
//      output-frame ruler, a monitor tile showing what is on air) must use these.
//
//   2. THE STAGE'S OWN PROPORTION (fitScale / heightForWidth / widthForHeight).
//      A picture of the whole stage — a library card, a slide thumbnail, the Edit
//      canvas itself — keeps the stage's proportions, so its height follows its
//      width and the design cannot look stretched by the panel it is in.
//
// The two rules differ only because 754:428 is not exactly 16:9 (1.7617 vs
// 1.7778): at 1920x1080 that is 0.9%, i.e. up to ~9px at the very edge of the
// frame and nothing at all along the centre lines. Rule 1 is the one the OUTPUT
// obeys; rule 2 is a display convenience and must never be used to decide where a
// block lands in a frame.
//
// The engine's half of the same constant lives in DesignLibrary.hpp
// (design::kStageWidth / kStageHeight) and SceneBuilder reads it from there — one
// definition per side, and they are the same numbers. Change one, change both.
QtObject {
    id: grid

    // The stage the operator designs on, in its own units.
    readonly property real w: 754
    readonly property real h: 428
    // The stage's proportion (h / w) — for the rare caller that wants the ratio
    // itself rather than a mapped length.
    readonly property real aspect: h / w

    // --- rule 1: the frame's mapping (axis by axis, full bleed) ---------------
    function mapX(x, targetW) { return x / grid.w * targetW }
    function mapY(y, targetH) { return y / grid.h * targetH }
    function mapW(width, targetW) { return width / grid.w * targetW }
    function mapH(height, targetH) { return height / grid.h * targetH }
    // A block's rect in a target frame: { x, y, width, height } in target pixels.
    // `block` may be any object with x / y / width / height (a design block, a
    // canvas item — anything storing stage units).
    function mapRect(block, targetW, targetH) {
        return {
            x: grid.mapX(Number(block.x) || 0, targetW),
            y: grid.mapY(Number(block.y) || 0, targetH),
            width: grid.mapW(Number(block.width) || 0, targetW),
            height: grid.mapH(Number(block.height) || 0, targetH)
        }
    }
    // A text size is a HEIGHT quantity: the engine scales a block's fontSize by
    // the frame's height (never its width), so a font sized here lands on air at
    // the size the frame draws.
    function mapFontSize(fontSize, targetH) { return grid.mapH(fontSize, targetH) }

    // --- rule 2: the stage's own proportion (pictures of the whole stage) -----
    // Height for a given width, and the reverse — what the thumbnails use.
    function heightForWidth(px) { return Math.round(px * grid.aspect) }
    function widthForHeight(px) { return Math.round(px / grid.aspect) }
    // The uniform scale that fits `availW x availH` — the Edit canvas's own fit
    // rule, so a canvas and a card scale a stage identically.
    function fitScale(availW, availH) {
        return Math.min(availW / grid.w, availH / grid.h)
    }
}
