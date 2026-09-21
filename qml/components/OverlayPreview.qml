import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// Draws an overlay from the elements the ENGINE keeps for it (OverlayLibraryService.overlays()).
//
// Every element's numbers are on a 1920 x 1080 canvas; this item lays that canvas out at its own width
// (so it is 16:9 - set the width and the height follows) and draws each element in it:
//   box ........ a filled / bordered rectangle
//   text ....... the same, with a line of text in it
//   clock ...... the time as digits, or an analog face
//   vignette ... the screen's edges tinted (a radial fade to the element's colour)
//   corners .... the four screen corners cut round, in the element's colour
// The colours arrive as QML colours (the service converts CSS rgba()), "" meaning none.
Item {
    id: root

    property var elements: []
    // What time the clocks show; the host ticks it (one shared ticker rather than one per card).
    property date now: new Date()

    readonly property real unit: width / 1920
    implicitHeight: Math.round(width * 9 / 16)
    clip: true

    // The four cut corners of a w x h screen as one SVG path (each is the area outside a quarter circle).
    function cornersPath(w, h, r) {
        return "M 0 0 L " + r + " 0 A " + r + " " + r + " 0 0 0 0 " + r + " Z "
             + "M " + w + " 0 L " + w + " " + r + " A " + r + " " + r + " 0 0 0 " + (w - r) + " 0 Z "
             + "M " + w + " " + h + " L " + (w - r) + " " + h + " A " + r + " " + r + " 0 0 0 " + w + " " + (h - r) + " Z "
             + "M 0 " + h + " L 0 " + (h - r) + " A " + r + " " + r + " 0 0 0 " + r + " " + h + " Z"
    }

    // The 1920 x 1080 canvas, shrunk to fit.
    Item {
        width: 1920; height: 1080
        scale: root.unit
        transformOrigin: Item.TopLeft

        Repeater {
            model: root.elements

            delegate: Item {
                id: el
                required property var modelData
                readonly property string kind: modelData.kind
                readonly property bool hasFill: kind === "box" || kind === "text" || kind === "clock"

                x: modelData.x; y: modelData.y
                width: modelData.width; height: modelData.height

                // fill and border
                Rectangle {
                    visible: el.hasFill
                    anchors.fill: parent
                    color: el.modelData.background !== "" ? el.modelData.background : "transparent"
                    radius: Math.min(el.modelData.radius, Math.min(width, height) / 2)
                    border.width: el.modelData.borderWidth
                    border.color: el.modelData.borderColor !== "" ? el.modelData.borderColor : "transparent"
                }

                // text
                Text {
                    visible: el.kind === "text"
                    anchors.fill: parent
                    leftPadding: 10; rightPadding: 10
                    text: el.modelData.text
                    color: el.modelData.textColor
                    font.family: Theme.fontFamily
                    font.pixelSize: el.modelData.fontSize
                    font.bold: el.modelData.bold
                    font.capitalization: el.modelData.uppercase ? Font.AllUppercase : Font.MixedCase
                    horizontalAlignment: el.modelData.align === "center" ? Text.AlignHCenter
                                       : (el.modelData.align === "right" ? Text.AlignRight : Text.AlignLeft)
                    verticalAlignment: Text.AlignVCenter
                    wrapMode: Text.NoWrap
                    elide: Text.ElideRight
                }

                // digital clock
                Text {
                    visible: el.kind === "clock" && !el.modelData.analog
                    anchors.fill: parent
                    rightPadding: 10; leftPadding: 10
                    text: Qt.formatTime(root.now, "hh:mm")
                    color: el.modelData.textColor
                    font.family: Theme.fontFamily
                    font.pixelSize: el.modelData.fontSize
                    font.bold: el.modelData.bold
                    horizontalAlignment: el.modelData.align === "center" ? Text.AlignHCenter
                                       : (el.modelData.align === "right" ? Text.AlignRight : Text.AlignLeft)
                    verticalAlignment: Text.AlignVCenter
                }

                // analog clock: twelve ticks and the three hands, on the element's own (round) face
                Item {
                    visible: el.kind === "clock" && el.modelData.analog
                    anchors.fill: parent

                    Repeater {
                        model: 12
                        delegate: Item {
                            required property int index
                            x: parent.width / 2; y: parent.height / 2
                            rotation: index * 30
                            Rectangle {
                                x: -width / 2
                                y: -el.height * 0.46
                                width: el.width * (index % 3 === 0 ? 0.012 : 0.006)
                                height: el.height * (index % 3 === 0 ? 0.06 : 0.035)
                                radius: width / 2
                                color: el.modelData.textColor
                            }
                        }
                    }
                    // hour
                    Rectangle {
                        x: parent.width / 2 - width / 2; y: parent.height / 2 - height
                        width: el.width * 0.022; height: el.height * 0.24
                        radius: width / 2
                        color: el.modelData.textColor
                        transformOrigin: Item.Bottom
                        rotation: (root.now.getHours() % 12 + root.now.getMinutes() / 60) * 30
                    }
                    // minute
                    Rectangle {
                        x: parent.width / 2 - width / 2; y: parent.height / 2 - height
                        width: el.width * 0.014; height: el.height * 0.36
                        radius: width / 2
                        color: el.modelData.textColor
                        transformOrigin: Item.Bottom
                        rotation: (root.now.getMinutes() + root.now.getSeconds() / 60) * 6
                    }
                    // second
                    Rectangle {
                        x: parent.width / 2 - width / 2; y: parent.height / 2 - height
                        width: el.width * 0.007; height: el.height * 0.4
                        radius: width / 2
                        color: Theme.danger
                        transformOrigin: Item.Bottom
                        rotation: root.now.getSeconds() * 6
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: el.width * 0.03; height: width; radius: width / 2
                        color: el.modelData.textColor
                    }
                }

                // vignette: clear in the middle, the element's colour at the edges
                Shape {
                    visible: el.kind === "vignette"
                    anchors.fill: parent
                    ShapePath {
                        strokeColor: "transparent"
                        fillGradient: RadialGradient {
                            centerX: el.width / 2; centerY: el.height / 2
                            focalX: centerX; focalY: centerY
                            centerRadius: Math.hypot(el.width, el.height) / 2
                            GradientStop { position: 0.0; color: Qt.alpha(el.modelData.background, 0) }
                            GradientStop { position: 0.6; color: Qt.alpha(el.modelData.background, 0) }
                            GradientStop { position: 1.0; color: Qt.alpha(el.modelData.background, 0.9) }
                        }
                        startX: 0; startY: 0
                        PathLine { x: el.width; y: 0 }
                        PathLine { x: el.width; y: el.height }
                        PathLine { x: 0; y: el.height }
                        PathLine { x: 0; y: 0 }
                    }
                }

                // rounded screen corners
                Shape {
                    visible: el.kind === "corners"
                    anchors.fill: parent
                    ShapePath {
                        fillColor: el.modelData.background
                        strokeColor: "transparent"
                        PathSvg { path: root.cornersPath(el.width, el.height, Math.max(1, el.modelData.inset)) }
                    }
                }
            }
        }
    }
}
