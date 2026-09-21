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
//
// SCALABLE, not fixed: the tile fills whatever width its wall assigns (one
// output takes the whole column, two split it) and the internals are
// anchored — the preview pane holds a true 16:9, the badge/name scale
// proportionally — so a big tile looks composed, not a stretched 182px
// card. Height derives from the width (pane + footer); the caller may
// still set both explicitly (Edit's ITEMS tab keeps its compact cells).
Rectangle {
    id: root

    // OutputListModel roles
    required property string name
    required property string badge
    required property bool active
    required property bool isEnabled

    // Caller sets width (or anchors); height follows as pane + footer.
    implicitWidth: 182
    implicitHeight: 6 + previewPane.height + 6 + 16 + 6
    radius: 8
    // Live = danger-red border; inactive = visible slate border so an off
    // tile reads as "inactive", not just black.
    border.color: root.active ? "#85261f" : "#2b2e3d"
    border.width: 1
    color: "#16171e"
    // Disabled screens dim everywhere — same model, same state.
    opacity: root.isEnabled ? 1 : 0.45
    Behavior on opacity { NumberAnimation { duration: 120 } }

    // 16:9 preview pane — always inset 6px, always the right aspect. The pane
    // is TRANSPARENT by default (an output shows whatever is behind it until
    // content is on air), so it draws the shared transparency checkerboard
    // (same texture as the Edit canvas and the design-card previews) instead
    // of a solid placeholder colour.
    Rectangle {
        id: previewPane
        x: 6
        y: 6
        width: parent.width - 12
        height: width * 9 / 16
        clip: true
        radius: 4
        color: "transparent"

        Checkerboard {
            anchors.fill: parent
            tileSize: 9
            shadeA: "#3a3c48"
            shadeB: "#25262f"
        }

        // LIVE badge in the danger red (#ff4d3d family) when on air — same
        // pill the Screens settings card renders; neutral dark chip otherwise.
        Rectangle {
            x: 6
            y: 6
            height: Math.max(16, previewPane.height * 0.14)
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

        // Play glyph for whichever output is actually live — scales with
        // the pane so it stays centered and proportionate at any size.
        IconGlyph {
            visible: root.active
            anchors.centerIn: parent
            name: "playCircle"
            color: "#ff4d3d"
            implicitWidth: Math.max(28, previewPane.height * 0.25)
            implicitHeight: implicitWidth
        }
    }

    // Footer: output name — anchored to the pane's bottom, full width.
    Item {
        x: 6
        y: previewPane.y + previewPane.height + 6
        width: parent.width - 12
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
