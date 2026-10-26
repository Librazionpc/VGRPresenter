import QtQuick
import QtQuick.Window
import VGRPresenterUI

// The transparency checkerboard — every surface that draws content over "no
// background": the Edit canvas, design-card and tile previews, the output
// window / monitor tiles' alpha inspect, and the Lines card's band swatch.
//
// FREESHOW'S OWN DEFINITION, which this follows (its public/global.css):
//
//     .checkered {
//         background: repeating-conic-gradient(rgba(70, 70, 80, 0.15) 0% 25%,
//                                              transparent 0% 50%) 50% / 26px 26px;
//     }
//
//   ONE TRANSLUCENT TINT, not two opaque shades. FreeShow paints every other
//   square and lets the surface show through the other half — 26px / 2 = 13px
//   cells. That is why it reads correctly on a light theme too: the tint is
//   #464650 at 15%, so over a dark surface it is a slightly raised square and
//   over a white one a slightly dimmed square.
//
//   A 13px CELL (26px tile), the same on every surface. Callers CAN override the
//   cell, and none should need to: FreeShow draws one board behind the canvas,
//   every card, the output preview and the phone remote alike, and a board that
//   changes size between two pictures of the same slide reads as two different
//   conventions.
//
//   A CENTRED PHASE (`background-position: 50%`). The board does not start at
//   the surface's top-left corner; its tiles are centred, so the edge cells are
//   cut and the pattern reads as texture rather than as a grid anchored to the
//   item's border.
//
// PAINTED IN SCREEN PIXELS, AND ON WHOLE DEVICE PIXELS (*devicePixelRatio*
// below). `hostScale` first:
// This is the part that matters for both looks and cost. Almost every host
// draws inside a SCALED subtree — the Edit canvas sits under
// `canvasZoom × canvasFitScale`, every DesignPreview under its `unit`
// (754 stage units squeezed into a 120px card) — and a Canvas rasterized there
// is created at the item's size IN THAT SUBTREE and then magnified by the
// ancestor transform. Magnifying an alternating pattern (a) smears every cell
// edge over a fractional pixel, so the same grid reads as a second, offset one
// ("two checkers"), and (b) rasterizes the board at STAGE size for a thumbnail
// (a 120px card used to build a 754×428 raster) and at zoom-multiplied size for
// the canvas. So the Canvas is counter-scaled: it is sized to `width ×
// hostScale` — the area it will actually cover on screen — and scaled by
// 1 / hostScale, which puts it back at 1:1 with real pixels. Cell edges land on
// whole pixels at any zoom, and a card's raster costs the card's own size.
//
//   ...and then the cell is snapped to whole DEVICE pixels. A 13px cell on a
//   125% display is 16.25 device pixels, so rounding each filled rectangle
//   independently made every fourth cell a pixel wider — a slow beat that the
//   eye reads as a second, coarser board of bigger squares sitting on top of
//   the fine one (the "big checkers around the text" report). Rounding the CELL
//   once — to an integer number of device pixels — and stepping by that makes
//   the lattice exactly regular: 16 device pixels everywhere at 125%, 13 at
//   100%, and no beat at any ratio in between.
//
// ONE PAINTED PASS, and only the tinted half of it. No Repeater of Rectangles,
// no second shade to fill: a caller that wants an opaque second colour passes
// shadeB, and only those cells are filled.
Item {
    id: root

    // The painted squares; the other half of the board is left UNPAINTED, so
    // whatever sits behind the item shows through — that is the whole point of
    // a transparency board.
    //
    // FreeShow's OWN value, unmodified and on both themes: `#464650` at 15%.
    // It was briefly lifted to 30% on the dark theme ("the cue is hard to see"),
    // and that is what made the board read as a grid of BOXES rather than as
    // texture — at 30% over the canvas's near-black, every filled square is a
    // distinct panel the eye lands on, while FreeShow's 15% is a faint lift the
    // eye reads through. The board is a transparency CUE, not a pattern: one
    // value, the same everywhere, exactly as its public/global.css has it.
    property color shadeA: Qt.rgba(70 / 255, 70 / 255, 80 / 255, 0.15)
    // The unpainted squares, as a colour. Transparent by default, which means
    // "paint nothing"; kept as a property because a caller may want an explicit
    // colour for a printed/exported surface.
    property color shadeB: "transparent"
    // CELL size, in SCREEN pixels — what the eye sees, independent of whatever
    // scale this item is drawn under. That is FreeShow's own unit (13px cells),
    // and it is why no caller divides by its zoom any more.
    property real tileSize: 13
    // The scale of the subtree this board is drawn in (1 = none). The Edit
    // canvas passes its `canvasZoom × canvasFitScale`, DesignPreview its
    // `unit`. The Canvas is counter-scaled by it so the raster is 1:1 with
    // screen pixels — see the header.
    property real hostScale: 1
    // A ceiling on painted cells, so a pathological surface (a 4K output
    // window at 13px would be ~25k cells) can never park the GUI thread in a
    // repaint: past this the cell is doubled until it fits. Invisible in
    // practice — it only engages on surfaces far larger than any preview.
    property int maxCells: 40000

    // The screen-space area this board covers: counter-scaled back into the
    // host's coordinates below, it fills this item exactly.
    readonly property real screenScale: Math.max(0.01, hostScale)

    // The display's device pixel ratio — the unit the lattice is snapped to
    // (see the header). Read off the window's Screen so a board painted on a
    // 125%/150% display is as regular as one on a 100% display.
    readonly property real dpr: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1

    Item {
        id: board

        width: root.width * root.screenScale
        height: root.height * root.screenScale
        scale: 1 / root.screenScale
        transformOrigin: Item.TopLeft

        Canvas {
            id: canvas
            anchors.fill: parent
            // The raster is already 1:1 with screen pixels (see the header), so
            // there is nothing left to interpolate — and nearest-neighbour keeps
            // a cell edge a single hard line even if the board lands on a
            // half-pixel under some ancestor transform.
            smooth: false
            antialiasing: false
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.imageSmoothingEnabled = false

                if (width < 1 || height < 1)
                    return
                const paintingA = root.shadeA.a > 0.001
                const paintingB = root.shadeB.a > 0.001
                if (!paintingA && !paintingB)
                    return

                // Everything below is in DEVICE pixels (the context is in
                // logical units, so each number is divided by dpr on its way
                // in): the cell is rounded once, to a whole device pixel, and
                // every cell is then an exact integer step away from the last.
                const dpr = root.dpr
                const devW = Math.round(width * dpr)
                const devH = Math.round(height * dpr)

                // A cell never drops below a pixel, and coarsens only past the
                // cell ceiling (see maxCells).
                let cell = Math.max(1, Math.round(root.tileSize * dpr))
                while (cell > 1 && (devW / cell) * (devH / cell) > root.maxCells)
                    cell *= 2

                // `background-position: 50%`, snapped to whole pixels: the tile
                // GRID is centred in the item, so the first cell starts half a
                // period (or less, on a small item) before the edge. Negative
                // offsets are deliberate — the Canvas clips them.
                const period = cell * 2
                const offX = Math.round(((devW - period) / 2) % period)
                const offY = Math.round(((devH - period) / 2) % period)

                // Walk from the first cell that can touch the item to the last
                // that does — the board covers it exactly, whatever its size.
                const parity = (n) => ((n % 2) + 2) % 2
                const firstCol = Math.floor(-offX / cell) - 1
                const firstRow = Math.floor(-offY / cell) - 1
                const lastCol = Math.ceil((devW - offX) / cell) + 1
                const lastRow = Math.ceil((devH - offY) / cell) + 1
                const unit = cell / dpr

                for (let col = firstCol; col <= lastCol; ++col) {
                    const xDev = offX + col * cell
                    if (xDev > devW)
                        break
                    for (let row = firstRow; row <= lastRow; ++row) {
                        const yDev = offY + row * cell
                        if (yDev > devH)
                            break
                        const colour = parity(col + row) === 0 ? root.shadeA : root.shadeB
                        if (colour.a > 0.001) {
                            ctx.fillStyle = colour
                            ctx.fillRect(xDev / dpr, yDev / dpr, unit, unit)
                        }
                    }
                }
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
        }
    }

    clip: true

    function _repaint() { canvas.requestPaint() }
    onShadeAChanged: _repaint()
    onShadeBChanged: _repaint()
    onTileSizeChanged: _repaint()
    onHostScaleChanged: _repaint()
    // Moved to a display with a different scaling factor: the lattice is snapped
    // to device pixels, so the whole board is repainted at the new ratio.
    onDprChanged: _repaint()
}
