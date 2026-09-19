import QtQuick
import VGRPresenterUI

// One output-monitor tile — the card shown in the Edit screen's ITEMS tab
// and the Show screen's monitor wall, driven by OutputListModel in both
// places so renaming/restyling/toggling a screen updates every surface at
// once (the Show view's tiles used to be hand-typed copies that never
// reacted to anything).
//
// Same visual contract the Edit tab's inline card had: live = danger-red
// border + LIVE badge + play glyph; disabled = dimmed; preview area shows
// the checkered "empty" texture until real per-output thumbnails exist.
Rectangle {
    id: root

    // OutputListModel roles
    required property string name
    required property string badge
    required property bool active
    required property bool isEnabled

    width: 182
    height: 143
    radius: 8
    // Live = danger-red border; inactive = visible slate border so an off
    // tile reads as "inactive", not just black.
    border.color: root.active ? "#85261f" : "#2b2e3d"
    border.width: 1
    color: "#16171e"
    // Disabled screens dim everywhere — same model, same state.
    opacity: root.isEnabled ? 1 : 0.45
    Behavior on opacity { NumberAnimation { duration: 120 } }

    Rectangle {
        x: 6
        y: 6
        width: 170
        height: 110
        clip: true
        radius: 4
        color: root.active ? "#101116" : "#1a1c26"

        // LIVE badge in the danger red (#ff4d3d family) when on air — same
        // pill the Screens settings card renders; neutral dark chip otherwise.
        Rectangle {
            x: 6
            y: 6
            height: 16
            width: badgeRow.width + 16
            radius: 4
            color: root.active ? "#33ff4d3d" : "#262833"

            Row {
                id: badgeRow
                anchors.centerIn: parent
                spacing: 4

                // Live dot only makes sense for the output on air.
                Rectangle {
                    visible: root.active
                    anchors.verticalCenter: parent.verticalCenter
                    width: 6; height: 6; radius: 3
                    color: "#ff4d3d"
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.active ? "#ff6b61" : "#9aa0b5"
                    font.family: "Inter"
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    text: root.badge
                }
            }
        }

        // Play glyph for whichever output is actually live.
        IconGlyph {
            visible: root.active
            anchors.centerIn: parent
            name: "playCircle"
            color: "#ff4d3d"
            implicitWidth: 28
            implicitHeight: 28
        }
    }

    // Footer: output name.
    Item {
        x: 6
        y: 120
        width: 170
        height: 16

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: "#e2e8f0"
            font.family: "Inter"
            font.pixelSize: 11
            font.weight: Font.Medium
            text: root.name
        }
    }
}
