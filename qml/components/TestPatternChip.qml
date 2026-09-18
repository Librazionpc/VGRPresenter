import QtQuick
import VGRPresenterUI

// One test-pattern option chip: a small swatch rendering the actual pattern
// (SMPTE bars / gradient / checker / solid) + a label. Selectable; the
// swatch is pure Canvas-free QML (Rectangles + a gradient), no JS needed.
Rectangle {
    id: root

    // "smpte" | "gradient" | "checker" | "solidred"
    property string pattern: "smpte"
    property string label: ""
    property bool selected: false
    signal picked()

    implicitWidth: swatch.width + 8 + chipLabel.implicitWidth + 24
    implicitHeight: 30
    radius: Theme.radiusMd
    color: root.selected ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.15)
                         : (area.containsMouse ? Theme.chip : Theme.inset)
    border.width: 1
    border.color: root.selected ? Theme.accent : Theme.border
    Behavior on color { ColorAnimation { duration: 100 } }

    Item {
        id: swatch
        x: 12
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 16
        clip: true

        // SMPTE bars — vertical stripes in the classic order. One gradient
        // stop per bar: cheap, dependency-free, and reads instantly.
        Rectangle {
            anchors.fill: parent
            visible: root.pattern === "smpte"
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.00; color: "#c0c0c0" }
                GradientStop { position: 0.00; color: "#c0c0c0" }
                GradientStop { position: 0.17; color: "#c0c000" }
                GradientStop { position: 0.33; color: "#00c0c0" }
                GradientStop { position: 0.50; color: "#00c000" }
                GradientStop { position: 0.67; color: "#c000c0" }
                GradientStop { position: 0.83; color: "#c00000" }
                GradientStop { position: 1.00; color: "#0000c0" }
            }
        }

        Rectangle {
            anchors.fill: parent
            visible: root.pattern === "gradient"
            radius: 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "#6C5CE7" }
                GradientStop { position: 1; color: "#00c2ff" }
            }
        }

        // Checker — 2×2 grid of alternating squares.
        Item {
            anchors.fill: parent
            visible: root.pattern === "checker"

            Rectangle { x: 0; y: 0; width: 12; height: 8; color: "#e2e8f0" }
            Rectangle { x: 12; y: 0; width: 12; height: 8; color: "#16171e" }
            Rectangle { x: 0; y: 8; width: 12; height: 8; color: "#16171e" }
            Rectangle { x: 12; y: 8; width: 12; height: 8; color: "#e2e8f0" }
        }

        Rectangle {
            anchors.fill: parent
            visible: root.pattern === "solidred"
            radius: 2
            color: "#ff4d3d"
        }
    }

    Text {
        id: chipLabel
        x: swatch.x + swatch.width + 8
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: root.selected ? Theme.accentLight : Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
        font.weight: root.selected ? Font.DemiBold : Font.Medium
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.picked()
    }
}
