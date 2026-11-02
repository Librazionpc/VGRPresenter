pragma Singleton
import QtQuick

//   1. THE FRAME'S RULE (mapX / mapY / mapW / mapH / mapRect). The engine lays a
//   2. THE STAGE'S OWN PROPORTION (fitScale / heightForWidth / widthForHeight).
// 1.7778): at 1920x1080 that is 0.9%, i.e. up to ~9px at the very edge of the
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
    function mapRect(block, targetW, targetH) {
        return {
            x: grid.mapX(Number(block.x) || 0, targetW),
            y: grid.mapY(Number(block.y) || 0, targetH),
            width: grid.mapW(Number(block.width) || 0, targetW),
            height: grid.mapH(Number(block.height) || 0, targetH)
        }
    }
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