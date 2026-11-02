import QtQuick
import QtQuick.Window
import VGRPresenterUI

Item {
    id: root

    property color shadeA: Qt.rgba(70 / 255, 70 / 255, 80 / 255, 0.15)
    property color shadeB: "transparent"
    property real tileSize: 13
    property real hostScale: 1
    property int maxCells: 40000

    // The screen-space area this board covers: counter-scaled back into the
    // host's coordinates below, it fills this item exactly.
    readonly property real screenScale: Math.max(0.01, hostScale)

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

                const dpr = root.dpr
                const devW = Math.round(width * dpr)
                const devH = Math.round(height * dpr)

                // A cell never drops below a pixel, and coarsens only past the
                // cell ceiling (see maxCells).
                let cell = Math.max(1, Math.round(root.tileSize * dpr))
                while (cell > 1 && (devW / cell) * (devH / cell) > root.maxCells)
                    cell *= 2

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