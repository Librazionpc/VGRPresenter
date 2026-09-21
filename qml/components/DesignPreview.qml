import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// Draws a DESIGN - an overlay or a template - from the BLOCKS the ENGINE keeps for it
// (DesignLibraryService.designs(): { key, kind, text, x, y, width, height, bind, meta, style }).
//
// The blocks are in the Edit screen's stage units (754 x 428); this item lays the stage out at its own
// width (set the width and the height follows). It draws the same kinds the Edit screen draws, with the
// same meta keys, so a card preview and the canvas agree:
//   text ..... meta { fontFamily, fontSize, bold, italic, underline, strikethrough, align, color, autoSize }
//   shape .... meta { shapeType } rectangle | circle | line | triangle | arrow | star | hexagon
//   clock .... meta { format, showSeconds, showDate, style: "analog" }
//   timer .... meta { mode, durationSeconds }
//   vignette . the screen's edges tinted: style.backgroundColor = the tint, meta.inset = how far in
//   corners .. the four screen corners cut round: style.backgroundColor = the colour, meta.inset = radius
// A template's bound block (bind set) shows the name of the field that feeds it, so the preview reads
// as a layout rather than empty text.
Item {
    id: root

    property var blocks: []
    property string background: "transparent"
    // What time the clocks show; the host ticks it (one shared ticker rather than one per card).
    property date now: new Date()

    readonly property real stageW: 754
    readonly property real stageH: 428
    readonly property real unit: width / stageW

    implicitHeight: Math.round(width * stageH / stageW)
    clip: true

    function cornersPath(w, h, r) {
        return "M 0 0 L " + r + " 0 A " + r + " " + r + " 0 0 0 0 " + r + " Z "
             + "M " + w + " 0 L " + w + " " + r + " A " + r + " " + r + " 0 0 0 " + (w - r) + " 0 Z "
             + "M " + w + " " + h + " L " + (w - r) + " " + h + " A " + r + " " + r + " 0 0 0 " + w + " " + (h - r) + " Z "
             + "M 0 " + h + " L 0 " + (h - r) + " A " + r + " " + r + " 0 0 0 " + r + " " + h + " Z"
    }

    // The stage, shrunk to fit. A transparent background shows the shared
    // transparency checkerboard (the Edit canvas's convention), anything else
    // paints its colour.
    Item {
        width: root.stageW; height: root.stageH
        scale: root.unit
        transformOrigin: Item.TopLeft

        Checkerboard {
            anchors.fill: parent
            visible: root.background === "" || root.background === "transparent"
        }
        Rectangle {
            anchors.fill: parent
            visible: !(root.background === "" || root.background === "transparent")
            color: root.background
        }

        Repeater {
            model: root.blocks

            delegate: Item {
                id: el
                required property var modelData
                readonly property string kind: modelData.kind
                readonly property var meta: modelData.meta !== undefined ? modelData.meta : {}
                readonly property var style: modelData.style !== undefined ? modelData.style : {}
                readonly property bool screenWide: kind === "vignette" || kind === "corners"
                // The box behind a block: text and shapes always have one; anything else (a clock...) only with a fill.
                // (A vignette / corners block is drawn by its own item below and must never be filled as a box.)
                readonly property bool hasBox: !screenWide && (kind === "text" || kind === "shape"
                                               || (style.backgroundColor !== undefined && style.backgroundColor !== "transparent"
                                                   && style.backgroundColor !== ""))

                x: modelData.x; y: modelData.y
                width: modelData.width !== undefined ? modelData.width : 160
                height: modelData.height !== undefined ? modelData.height : 40

                // fill + border (the box the Edit screen draws behind text and shapes)
                Rectangle {
                    visible: el.hasBox && (el.kind !== "shape" || (!el.isCircle && !el.isLine && !el.isGlyph))
                    anchors.fill: parent
                    color: el.style.backgroundColor !== undefined && el.style.backgroundColor !== "transparent" ? el.style.backgroundColor : "transparent"
                    radius: el.kind === "shape" ? (el.isCircle ? Math.min(width, height) / 2 : (el.style.cornerRadius ?? 0))
                                               : (el.style.cornerRadius ?? 0)
                    border.width: el.style.borderEnabled === true ? (el.style.borderWidth ?? 2) : 0
                    border.color: el.style.borderColor !== undefined ? el.style.borderColor : "#ffffff"
                }

                // --- text (and a bound template field's placeholder) ---
                Text {
                    visible: el.kind === "text"
                    anchors.fill: parent
                    leftPadding: el.style.padding ?? 0; rightPadding: el.style.padding ?? 0
                    text: el.modelData.text !== "" ? el.modelData.text
                        : (el.modelData.bind !== undefined && el.modelData.bind !== "" ? "{" + el.modelData.bind + "}" : "")
                    color: el.meta.color ?? "#f2f4fa"
                    font.family: el.meta.fontFamily ?? Theme.fontFamily
                    font.pixelSize: el.meta.fontSize ?? 16
                    font.bold: el.meta.bold === true
                    font.italic: el.meta.italic === true
                    font.underline: el.meta.underline === true
                    font.strikeout: el.meta.strikethrough === true
                    font.letterSpacing: el.meta.letterSpacing ?? 0
                    lineHeight: el.meta.lineHeight ?? 1.2
                    horizontalAlignment: {
                        const a = el.meta.align ?? "center"
                        return a === "left" ? Text.AlignLeft : (a === "right" ? Text.AlignRight : Text.AlignHCenter)
                    }
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.WordWrap
                    elide: Text.ElideRight
                }

                // --- shape glyphs (the same marks the Edit screen draws) ---
                Text {
                    visible: el.isGlyph || el.isLine
                    anchors.fill: parent
                    text: el.isLine ? "" : (el.shapeType === "triangle" ? "▲" : el.shapeType === "arrow" ? "→"
                                            : el.shapeType === "star" ? "★" : "⬢")
                    color: el.style.backgroundColor !== "transparent" ? el.style.backgroundColor : "#9aa0b0"
                    font.pixelSize: Math.min(el.width, el.height) * 0.7
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                Rectangle {
                    // a line: a thin bar across the box
                    visible: el.isLine
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width; height: Math.max(2, el.style.borderWidth ?? 3)
                    color: el.style.backgroundColor !== "transparent" ? el.style.backgroundColor : "#9aa0b0"
                    rotation: 0
                }

                // --- clock ---
                Text {
                    visible: el.kind === "clock" && el.meta.style !== "analog"
                    anchors.fill: parent
                    text: Qt.formatDateTime(root.now, (el.meta.format !== "24" ? "h" : "HH") + ":mm"
                                            + (el.meta.showSeconds !== false ? ":ss" : ""))
                    color: el.meta.color ?? "#f2f4fa"
                    font.family: el.meta.fontFamily ?? Theme.fontFamily
                    font.pixelSize: el.meta.fontSize ?? Math.min(el.width, el.height) * 0.5
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                Item {
                    visible: el.kind === "clock" && el.meta.style === "analog"
                    anchors.centerIn: parent
                    width: Math.min(el.width, el.height); height: width

                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: "transparent"
                        border.width: 2
                        border.color: el.meta.color ?? "#f2f4fa"
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.verticalCenter
                        width: 2; height: parent.height * 0.28
                        color: el.meta.color ?? "#f2f4fa"
                        rotation: root.now.getHours() % 12 * 30 + root.now.getMinutes() * 0.5
                        transformOrigin: Item.Bottom
                    }
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.verticalCenter
                        width: 2; height: parent.height * 0.4
                        color: el.meta.color ?? "#f2f4fa"
                        rotation: root.now.getMinutes() * 6
                        transformOrigin: Item.Bottom
                    }
                }

                // --- timer ---
                Text {
                    visible: el.kind === "timer"
                    anchors.fill: parent
                    text: "00:00"
                    color: el.meta.color ?? "#f2f4fa"
                    font.family: el.meta.fontFamily ?? Theme.fontFamily
                    font.pixelSize: el.meta.fontSize ?? Math.min(el.width, el.height) * 0.5
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                // --- vignette: the block's tint fading in from all four edges, meta.inset deep (a CSS inset shadow) ---
                Item {
                    visible: el.kind === "vignette"
                    anchors.fill: parent
                    id: vig
                    readonly property real depth: Math.max(1, Math.min(el.meta.inset ?? 100, Math.min(el.width, el.height) / 2))
                    readonly property color clear: Qt.alpha(el.tint, 0)

                    Rectangle {   // top
                        width: parent.width; height: parent.depth
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: el.tint }
                            GradientStop { position: 1.0; color: vig.clear }
                        }
                    }
                    Rectangle {   // bottom
                        y: parent.height - parent.depth
                        width: parent.width; height: parent.depth
                        gradient: Gradient {
                            orientation: Gradient.Vertical
                            GradientStop { position: 0.0; color: vig.clear }
                            GradientStop { position: 1.0; color: el.tint }
                        }
                    }
                    Rectangle {   // left
                        width: parent.depth; height: parent.height
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: el.tint }
                            GradientStop { position: 1.0; color: vig.clear }
                        }
                    }
                    Rectangle {   // right
                        x: parent.width - parent.depth
                        width: parent.depth; height: parent.height
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: vig.clear }
                            GradientStop { position: 1.0; color: el.tint }
                        }
                    }
                }

                // --- corners: the four screen corners cut round (meta.inset = the radius) ---
                Shape {
                    visible: el.kind === "corners"
                    anchors.fill: parent
                    ShapePath {
                        fillColor: el.tint
                        strokeColor: "transparent"
                        PathSvg { path: root.cornersPath(el.width, el.height, Math.max(1, el.meta.inset ?? 40)) }
                    }
                }

                // --- kinds only overlays use ---
                readonly property color tint: el.style.backgroundColor !== undefined ? el.style.backgroundColor : "#000000"
                readonly property string shapeType: el.meta.shapeType ?? "rectangle"
                readonly property bool isCircle: el.shapeType === "circle"
                readonly property bool isLine: el.shapeType === "line"
                readonly property bool isGlyph: ["triangle", "arrow", "star", "hexagon"].includes(el.shapeType)
            }

        }
    }
}
