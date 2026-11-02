import QtQuick
import VGRPresenterUI

Item {
    id: root

    property var options: []
    property string current: ""
    // The choice to flag as the recommended one (a dot, no words).
    property string marked: ""
    // Segment padding and the track's own inset.
    property int hPad: 16
    property int inset: 3

    signal picked(string key)

    implicitHeight: 34
    implicitWidth: row.width + root.inset * 2

    // The track.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMd
        color: Theme.inset
        border.width: 1
        border.color: Theme.borderSubtle
    }

    // The sliding highlight.
    Rectangle {
        id: highlight
        readonly property Item target: {
            for (let i = 0; i < segments.count; ++i) {
                const item = segments.itemAt(i)
                if (item && item.key === root.current)
                    return item
            }
            return null
        }
        visible: target !== null
        y: root.inset
        height: parent.height - root.inset * 2
        x: target ? target.x + root.inset : root.inset
        width: target ? target.width : 0
        radius: Theme.radiusMd - 2
        color: Theme.accentSoft
        border.width: 1
        border.color: Theme.accent
        Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    }

    Row {
        id: row
        x: root.inset
        y: root.inset
        height: parent.height - root.inset * 2

        Repeater {
            id: segments
            model: root.options

            delegate: Item {
                id: seg
                required property var modelData
                readonly property string key: String(modelData.key)
                readonly property bool active: root.current === seg.key

                width: label.implicitWidth + root.hPad * 2
                height: parent.height

                Text {
                    id: label
                    anchors.centerIn: parent
                    text: seg.modelData.label
                    color: seg.active ? Theme.accentLight : (area.containsMouse ? Theme.textPrimary : Theme.textSecondary)
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                    font.weight: seg.active ? Font.DemiBold : Font.Medium
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade } }
                }

                // The engine's recommendation, as a quiet dot on the segment's corner.
                Rectangle {
                    visible: root.marked !== "" && seg.key === root.marked
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 5
                    anchors.rightMargin: 6
                    width: 5
                    height: 5
                    radius: 2.5
                    color: Theme.success
                }

                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.picked(seg.key)
                }
            }
        }
    }
}