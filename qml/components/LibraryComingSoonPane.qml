// Shared library-tab placeholder — consistent chrome while a tab's real
// pane is unbuilt (Media/Audio/Overlays/Templates/Calendar/Functions until
// their content lands). Same surface colors, borders and label typography
// as the browser panes, one centered status message, and the bottom-right
// "+ New …" pill from the reference design, wired to a signal so the pane's
// future real action has a single funnel.
import QtQuick
import VGRPresenterUI

Item {
    id: root

    // Tab-flavored copy so every dead tab at least says the right thing.
    property string title: qsTr("Not built yet")
    property string message: qsTr("This library is next on the roadmap.")
    property string newLabel: qsTr("New")

    // Emitted when the New pill is clicked — the tab's future real action
    // funnels here (same pattern as newShowRequested).
    signal newRequested()

    Rectangle {
        anchors.fill: parent
        color: "#0f1015"

        Column {
            anchors.centerIn: parent
            spacing: Theme.space2

            IconGlyph {
                name: "layoutTemplate"
                color: Theme.textMuted
                width: 24; height: 24
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.title
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXl
                font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.message
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
            }
        }

        // The "+ New …" pill, bottom-right — same position and styling as
        // the reference's New show/timer/overlay pills.
        Rectangle {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 16
            anchors.bottomMargin: 12
            width: newPillRow.implicitWidth + 32
            height: 38
            radius: 19
            color: newPillMouse.containsMouse ? "#1e1f28" : "#12131a"
            border.color: Theme.border
            border.width: 1
            MouseArea {
                id: newPillMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.newRequested()
            }
            Row {
                id: newPillRow
                anchors.centerIn: parent
                spacing: 8
                PlusGlyph { size: 14; thickness: 1.6; color: Theme.danger; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: root.newLabel
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: 14
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }
}
