import QtQuick
import VGRPresenterUI

// The transparency checkerboard — FreeShow's Zoomed.svelte convention, shared by every
// surface that draws content over "no background":
//
//   * the Edit screen's canvas (behind the slide background, so transparency reads through)
//   * design-card previews (an overlay/template with a transparent background)
//
// ONE painted surface, not a Repeater of many 16px Rectangles (that was the original
// shape here, and it is why the Edit canvas's grid read as "doubled": mCanvas applies a
// FRACTIONAL scale (canvasZoom * canvasFitScale) to fit the available space, and under
// this build's software renderer each tile's edges snap to the device-pixel grid
// INDEPENDENTLY — at a non-integer scale, adjacent tiles can round to different pixel
// boundaries, opening hairline seams/overlaps between them that read as a second, finer
// grid laid over the first. A Canvas paints the whole pattern as one continuous raster in
// a single pass, so there is no per-tile boundary left to round differently.
Item {
    id: root

    // The two shades, matching the Edit canvas.
    property color shadeA: "#2a2c38"
    property color shadeB: "#15161d"
    property int tileSize: 16

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const cols = Math.ceil(width / root.tileSize)
            const rows = Math.ceil(height / root.tileSize)
            for (let row = 0; row < rows; row++) {
                for (let col = 0; col < cols; col++) {
                    ctx.fillStyle = (row + col) % 2 === 0 ? root.shadeA : root.shadeB
                    ctx.fillRect(col * root.tileSize, row * root.tileSize, root.tileSize, root.tileSize)
                }
            }
        }
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    function _repaint() { canvas.requestPaint() }
    onShadeAChanged: _repaint()
    onShadeBChanged: _repaint()
    onTileSizeChanged: _repaint()
}
