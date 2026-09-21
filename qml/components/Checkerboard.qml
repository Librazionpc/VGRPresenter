import QtQuick
import VGRPresenterUI

// The transparency checkerboard — FreeShow's Zoomed.svelte convention, shared by every
// surface that draws content over "no background":
//
//   * the Edit screen's canvas (behind the slide background, so transparency reads through)
//   * design-card previews (an overlay/template with a transparent background)
//
// Fixed whole-pixel tiles (16px), not width/columns — a fractional tile still looks uneven
// even with index-based positioning, because each tile's fractional edges snap to the pixel
// grid independently. Columns/rows overshoot (ceil) and the host's clip crops the remainder.
Item {
    id: root

    // The two shades, matching the Edit canvas.
    property color shadeA: "#2a2c38"
    property color shadeB: "#15161d"
    property int tileSize: 16

    readonly property int columns: Math.ceil(width / tileSize)
    readonly property int rows: Math.ceil(height / tileSize)

    Repeater {
        model: root.columns * root.rows
        delegate: Rectangle {
            required property int index
            readonly property int col: index % root.columns
            readonly property int row: Math.floor(index / root.columns)
            x: col * root.tileSize
            y: row * root.tileSize
            width: root.tileSize
            height: root.tileSize
            color: (row + col) % 2 === 0 ? root.shadeA : root.shadeB
        }
    }
}
