import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property string label: ""
    property int pct: 0
    // At and above this the tile warns.
    property int warnAt: 90
    readonly property bool hot: root.pct >= root.warnAt
    readonly property color tone: root.hot ? Theme.warningLight : Theme.accent

    implicitWidth: 180
    implicitHeight: 74
    radius: Theme.radiusMd
    color: Theme.inset
    border.width: 1
    border.color: root.hot ? Qt.alpha(Theme.warningLight, 0.45) : Theme.borderSubtle
    Behavior on border.color { ColorAnimation { duration: 160 } }

    Text {
        x: 14
        y: 11
        text: root.label.toUpperCase()
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: 10
        font.weight: Font.DemiBold
        font.letterSpacing: 0.8
    }

    Row {
        x: 14
        y: 26
        spacing: 2
        Text {
            id: value
            text: String(root.pct)
            color: root.hot ? Theme.warningLight : Theme.textPrimary
            font.family: FontService.monoFamily
            font.pixelSize: 22
            font.weight: Font.DemiBold
        }
        Text {
            text: "%"
            anchors.baseline: value.baseline
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
        }
    }

    Rectangle {
        x: 14
        y: parent.height - 14
        width: parent.width - 28
        height: 4
        radius: 2
        color: Theme.borderSubtle

        Rectangle {
            width: parent.width * Math.max(0, Math.min(100, root.pct)) / 100
            height: parent.height
            radius: 2
            color: root.tone
            Behavior on width { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
        }
    }
}