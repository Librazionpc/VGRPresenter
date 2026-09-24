import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// Draws a DESIGN - an overlay or a template - from the BLOCKS the ENGINE keeps for it
// (DesignLibraryService.designs(): { key, kind, text, x, y, width, height, bind, meta, style }).
//
// The blocks are in the Edit screen's stage units (754 x 428); this item lays the stage out at its own
// width (set the width and the height follows). They may be plain objects (a library card) or the Edit screen's live
// CanvasItem objects (a slide thumbnail) - either way every property is read through bindings, so a live item re-draws here
// the moment it changes, without the list being rebuilt. It draws the same kinds the Edit screen draws, with the
// same meta keys, so a card preview and the canvas agree:
//   text ..... meta { fontFamily, fontSize, bold, italic, underline, strikethrough, align, color, autoSize }
//   shape .... meta { shapeType } rectangle | circle | line | triangle | arrow | star | hexagon
//   clock .... meta { format, showSeconds, showDate, style: "analog" }
//   timer .... meta { mode, durationSeconds }
//   camera / media / audio ... the canvas's own tile: the source's name (block text) over the kind's icon
//   vignette . the screen's edges tinted: style.backgroundColor = the tint, meta.inset = how far in
//   corners .. the four screen corners cut round: style.backgroundColor = the colour, meta.inset = radius
// A template's bound block (bind set) shows the name of the field that feeds it, so the preview reads
// as a layout rather than empty text.
//
// It is ONE renderer for every small picture of content in the app: the library cards, and the Edit screen's slide list
// (the LIVE thumbnails - SlideCanvasStore.blocksOf() re-reads the slide whenever anything on it changes). It follows the
// canvas's own rules for text (weight, case, list, alignment, shrink/grow to fit), clocks and timers (the same LiveClock
// formatting), shapes and transparency, so what the thumbnail shows is what the slide is.
Item {
    id: root

    property var blocks: []
    property string background: "transparent"
    // What time the clocks show; the host ticks it (one shared ticker rather than one per card).
    property date now: new Date()
    // Checkerboard tile size in stage units - a strip of many small slides passes a bigger one to keep them cheap.
    property int checkerSize: 16

    readonly property real stageW: 754
    readonly property real stageH: 428
    readonly property real unit: width / stageW

    implicitHeight: Math.round(width * stageH / stageW)
    clip: true

    // The canvas's clock/timer formatting, shared (this one never ticks; the host feeds `now`).
    LiveClock { id: fmt; running: false }
    // Fully transparent? Works for "transparent", "#00000000" (what a live colour reads back as) and real colours alike.
    function isClear(c) { return c === undefined || c === null || c === "" || Qt.color(c).a === 0 }

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
            tileSize: root.checkerSize
            visible: root.isClear(root.background)
        }
        Rectangle {
            anchors.fill: parent
            visible: !root.isClear(root.background)
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
                // The box behind a block the canvas draws one for: a text or a clock (a fill of nothing is just its border).
                // (Shapes, timers and camera / media tiles draw their own; a vignette / corners block is never a box.)
                readonly property bool hasBox: kind === "text" || kind === "clock"

                x: modelData.x; y: modelData.y
                width: modelData.width !== undefined ? modelData.width : 160
                height: modelData.height !== undefined ? modelData.height : 40

                // fill + border (the box the Edit screen draws behind a text or a clock)
                Rectangle {
                    visible: el.hasBox
                    anchors.fill: parent
                    color: el.style.backgroundColor !== undefined ? el.style.backgroundColor : "transparent"
                    radius: el.style.cornerRadius ?? 0
                    border.width: el.style.borderEnabled === true ? (el.style.borderWidth ?? 2) : 0
                    border.color: el.style.borderColor !== undefined ? el.style.borderColor : "#ffffff"
                }

                // --- text (and a bound template field's placeholder) ---
                // Auto-size runs through the SAME explicit fit solver as the edit
                // canvas (EditScreen.qml's recomputeFit): Qt's Text.Fit converged
                // differently in this scaled-down subtree than on the canvas, and
                // grown text could spill below its box on one surface but not the
                // other. A tiny bisection over candidate sizes, re-measuring an
                // invisible twin each step, gives both surfaces the same answer
                // (the largest size whose real laid-out height/width fit the box).
                property real fitProbe: el.meta.fontSize ?? 16
                property real fitAnswer: el.meta.fontSize ?? 16
                property bool fitSolving: false
                readonly property var fitInputs: [fitText.text, el.width, el.height, el.fitMode,
                                                  el.meta.fontSize ?? 16, el.meta.lineHeight ?? 1.2,
                                                  el.style.padding ?? 0]
                // EVERY text block solves — "none" included: the box is the master and the
                // text must always sit inside it (a "none" block renders at its set size
                // while that fits, and SQUEEZES the moment it doesn't; nothing is ever cut).
                onFitInputsChanged: if (!fitSolving)
                    Qt.callLater(el.solveFit)
                Component.onCompleted: if (!fitSolving)
                    Qt.callLater(el.solveFit)
                function solveFit() {
                    if (fitSolving)
                        return
                    fitSolving = true
                    const pad = el.style.padding ?? 0
                    const bw = el.width - pad * 2, bh = el.height - pad * 2
                    let lo = 3, hi = 400
                    if (bw <= 0 || bh <= 0) {
                        lo = el.meta.fontSize ?? 16
                    } else {
                        for (let i = 0; i < 11; ++i) {
                            const mid = (lo + hi) / 2
                            fitProbe = mid
                            if (fitMeasure.contentWidth <= bw && fitMeasure.contentHeight <= bh)
                                lo = mid
                            else
                                hi = mid
                        }
                    }
                    // grow: fill the box; shrink/none: at most the set size (the squeeze).
                    fitAnswer = el.fitMode === "grow" ? lo : Math.min(el.meta.fontSize ?? 16, lo)
                    fitProbe = fitAnswer
                    fitSolving = false
                }
                Text {
                    id: fitMeasure
                    visible: false
                    width: el.width - (el.style.padding ?? 0) * 2
                    text: fitText.text
                    font.family: fitText.font.family
                    font.pixelSize: el.fitProbe
                    font.weight: fitText.font.weight
                    font.italic: fitText.font.italic
                    font.capitalization: fitText.font.capitalization
                    font.letterSpacing: fitText.font.letterSpacing
                    lineHeight: fitText.lineHeight
                    lineHeightMode: Text.ProportionalHeight
                    wrapMode: Text.WordWrap
                }
                Text {
                    id: fitText
                    visible: el.kind === "text"
                    anchors.fill: parent
                    leftPadding: el.style.padding ?? 0; rightPadding: el.style.padding ?? 0
                    // (a block from the canvas may carry no text at all)
                    text: TextFormatService.applyList((el.modelData.text ?? "") !== "" ? el.modelData.text
                        : ((el.modelData.bind ?? "") !== "" ? "{" + el.modelData.bind + "}" : ""), el.meta.list ?? "")
                    color: el.meta.color ?? "#f2f4fa"
                    font.family: el.meta.fontFamily ?? Theme.fontFamily
                    // Always the solver's answer: grow fills the box, shrink/none render
                    // at the set size and squeeze only on overflow — the text ALWAYS sits
                    // inside its box, on every surface this item draws for.
                    font.pixelSize: el.fitAnswer
                    fontSizeMode: Text.FixedSize
                    font.weight: el.meta.bold ? Font.Bold
                        : el.meta.fontWeight === "Regular" ? Font.Normal
                        : el.meta.fontWeight === "SemiBold" ? Font.DemiBold
                        : el.meta.fontWeight === "Bold" ? Font.Bold : Font.Medium
                    font.italic: el.meta.italic === true
                    font.capitalization: el.meta.textCase === "upper" ? Font.AllUppercase
                                       : el.meta.textCase === "lower" ? Font.AllLowercase
                                       : el.meta.textCase === "capitalize" ? Font.Capitalize : Font.MixedCase
                    font.underline: el.meta.underline === true
                    font.strikeout: el.meta.strikethrough === true
                    font.letterSpacing: el.meta.letterSpacing ?? 0
                    lineHeight: el.meta.lineHeight ?? 1.2
                    horizontalAlignment: {
                        const a = el.meta.align ?? "center"
                        return a === "left" ? Text.AlignLeft : a === "right" ? Text.AlignRight
                             : a === "justify" ? Text.AlignJustify : Text.AlignHCenter
                    }
                    // Mirrors the canvas's own EditableCanvasLabel/EditScreen.qml verticalAlignment binding -
                    // defaults centered, a template can set "top" (or "bottom") on a block that reads better
                    // anchored to an edge (a big sermon-paragraph box, say) instead of floating mid-box.
                    verticalAlignment: {
                        const a = el.meta.verticalAlign ?? "center"
                        return a === "top" ? Text.AlignTop : a === "bottom" ? Text.AlignBottom : Text.AlignVCenter
                    }
                    wrapMode: Text.WordWrap
                    // (No elide: the solver above guarantees the laid-out text fits the
                    // box, so there is nothing to truncate — swallowed tails with an
                    // ellipsis were this line's doing.)
                }

                // --- shapes: the Edit screen's drawing - the fill is the item's background (transparent draws nothing, so an
                //     outline-only frame stays an outline), the stroke its border ---
                Rectangle {   // rectangle / rounded / anything unrecognised
                    visible: el.kind === "shape" && !el.isCircle && !el.isLine && !el.isGlyph
                    anchors.fill: parent
                    radius: el.style.cornerRadius ?? 0
                    color: el.fillColor
                    border.color: el.strokeColor
                    border.width: el.strokeWidth
                }
                Rectangle {   // circle - the square that fits, centred
                    visible: el.kind === "shape" && el.isCircle
                    anchors.centerIn: parent
                    width: Math.min(parent.width, parent.height); height: width
                    radius: width / 2
                    color: el.fillColor
                    border.color: el.strokeColor
                    border.width: el.strokeWidth
                }
                Rectangle {   // line - a thin bar across the middle
                    visible: el.kind === "shape" && el.isLine
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width; height: Math.max(2, el.strokeWidth || 3)
                    color: el.fillColor
                }
                Text {        // triangle / arrow / star / hexagon
                    visible: el.kind === "shape" && el.isGlyph
                    anchors.centerIn: parent
                    text: el.shapeType === "triangle" ? "▲" : el.shapeType === "arrow" ? "→" : el.shapeType === "star" ? "★" : "⬡"
                    color: el.fillColor
                    font.pixelSize: Math.min(el.width, el.height) * 0.7
                }

                // --- clock (digits, or an analog face) - formatted and sized like the canvas's own ---
                Column {
                    visible: el.kind === "clock" && el.meta.style !== "analog"
                    anchors.centerIn: parent
                    spacing: 2

                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: fmt.formatClock(root.now, el.meta.format !== "24", el.meta.showSeconds !== false)
                        color: el.meta.color ?? "#9b8ff5"
                        font.family: "Segoe UI"
                        font.weight: Font.DemiBold
                        font.pixelSize: Math.max(10, Math.min(el.width, el.height) * 0.22)
                    }
                    Text {
                        visible: el.meta.showDate === true
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: Qt.formatDate(root.now, "dddd, MMMM d")
                        color: el.meta.color !== undefined ? Qt.alpha(el.meta.color, 0.7) : "#5c6475"
                        font.family: "Segoe UI"
                        font.pixelSize: 10
                    }
                }
                Item {
                    visible: el.kind === "clock" && el.meta.style === "analog"
                    anchors.centerIn: parent
                    width: Math.min(el.width, el.height) * 0.8; height: width

                    Rectangle {   // the face: a dark disc unless the clock has no background
                        anchors.fill: parent
                        radius: width / 2
                        color: root.isClear(el.style.backgroundColor) ? "transparent" : "#12131a"
                        border.width: 2
                        border.color: el.meta.color ?? "#3a4155"
                    }
                    Rectangle {   // hour
                        width: 4; height: parent.height * 0.24; radius: 2
                        color: el.meta.color ?? "#eef1f8"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.verticalCenter
                        transformOrigin: Item.Bottom
                        rotation: (root.now.getHours() % 12 + root.now.getMinutes() / 60) * 30
                    }
                    Rectangle {   // minute
                        width: 3; height: parent.height * 0.34; radius: 1.5
                        color: el.meta.color ?? "#c8cdd9"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.verticalCenter
                        transformOrigin: Item.Bottom
                        rotation: (root.now.getMinutes() + root.now.getSeconds() / 60) * 6
                    }
                    Rectangle {   // second
                        visible: el.meta.showSeconds !== false
                        width: 1.5; height: parent.height * 0.4
                        color: el.meta.color ?? "#6c5ce7"
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.verticalCenter
                        transformOrigin: Item.Bottom
                        rotation: root.now.getSeconds() * 6
                    }
                    Rectangle {   // pin
                        anchors.centerIn: parent
                        width: 6; height: 6; radius: 3
                        color: el.meta.color ?? "#6c5ce7"
                    }
                }

                // --- timer: a dark box unless the item has a fill, then the count (the canvas's own rule) ---
                Rectangle {
                    visible: el.kind === "timer"
                    anchors.fill: parent
                    radius: el.style.cornerRadius ?? 8
                    color: !root.isClear(el.style.backgroundColor) ? el.style.backgroundColor : "#12131a"
                    border.color: el.style.borderEnabled === true ? el.style.borderColor : "#3a4155"
                    border.width: el.style.borderEnabled === true ? (el.style.borderWidth ?? 2) : 1

                    Text {
                        anchors.centerIn: parent
                        text: fmt.formatDuration(fmt.timerSeconds(root.now, el.meta.mode ?? "countdown", el.meta.durationSeconds ?? 300,
                                                                  el.meta.startedAt ?? root.now.getTime()))
                        color: "#9b8ff5"
                        font.family: "Segoe UI"
                        font.weight: Font.DemiBold
                        font.pixelSize: Math.max(10, Math.min(el.width, el.height) * 0.28)
                    }
                }

                // --- camera: the live-preview tile the canvas shows (its source's name at the bottom) ---
                Rectangle {
                    visible: el.kind === "camera"
                    anchors.fill: parent
                    radius: 8
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#123326" }
                        GradientStop { position: 1; color: "#07130e" }
                    }
                    Rectangle { x: 8; y: 9; width: 5; height: 5; radius: 3; color: "#ff5d5d" }
                    Text { x: 18; y: 7; text: qsTr("LIVE"); color: "#e2e8f0"; font.family: "Segoe UI"; font.pixelSize: 8; font.weight: Font.Medium }
                    Rectangle { x: 10; y: 24; width: 36; height: 34; radius: 4; color: "#14503a" }
                    Rectangle { x: 30; y: 24; width: 24; height: 34; radius: 4; color: "#0f3a2c" }
                    Text {
                        x: 10; y: 68
                        text: (el.modelData.text ?? "") !== "" ? el.modelData.text : qsTr("Camera")
                        color: "#eef0f6"; font.family: "Segoe UI"; font.pixelSize: 9; font.weight: Font.Medium
                    }
                }

                // --- media / audio / any other kind: the canvas's generic tile - the kind's icon and name, and the picked file ---
                Rectangle {
                    visible: el.isPlaceholder
                    anchors.fill: parent
                    radius: 6
                    color: "#1a1c26"
                    border.width: 1
                    border.color: "#3a4155"
                    Column {
                        anchors.centerIn: parent
                        spacing: 4
                        IconGlyph {
                            anchors.horizontalCenter: parent.horizontalCenter
                            name: el.kind === "audio" ? "music" : el.kind === "media" ? "play" : "shape"
                            color: "#9b8ff5"
                            width: 16; height: 16
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: el.kind.toUpperCase()
                            color: "#aeb6c8"; font.family: "Segoe UI"; font.pixelSize: 13; font.weight: Font.Medium
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            visible: (el.modelData.text ?? "") !== ""
                            text: el.modelData.text ?? ""
                            color: "#5c6475"; font.family: "Segoe UI"; font.pixelSize: 10
                        }
                    }
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
                // text fitting, in the canvas's terms ("shrink" / "grow" also arrive as "shrinkToFit" / "growToFit")
                readonly property string fitMode: (el.meta.autoSize === "growToFit" || el.meta.autoSize === "grow") ? "grow"
                    : (el.meta.autoSize === "shrinkToFit" || el.meta.autoSize === "shrink") ? "shrink" : "none"
                readonly property bool isPlaceholder: !screenWide && !["text", "shape", "clock", "timer", "camera"].includes(el.kind)
                // a shape's look comes from the item's own style, exactly as on the canvas
                readonly property color fillColor: el.style.backgroundColor !== undefined ? el.style.backgroundColor : "#3a3f55"
                readonly property color strokeColor: el.style.borderColor !== undefined ? el.style.borderColor : "#6c5ce7"
                readonly property real strokeWidth: el.style.borderEnabled === true ? (el.style.borderWidth ?? 2) : 0
                readonly property bool isCircle: el.shapeType === "circle"
                readonly property bool isLine: el.shapeType === "line"
                readonly property bool isGlyph: ["triangle", "arrow", "star", "hexagon"].includes(el.shapeType)
            }

        }
    }
}
