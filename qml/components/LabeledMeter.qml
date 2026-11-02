import QtQuick
import VGRPresenterUI

Item {
    id: root

    property string label: ""
    property int pct: 0
    // -1 = a plain usage meter (the historic shape); >= 0 draws the allowance
    // band + cap tick and prints "pct% / cap%".
    property int capPct: -1
    readonly property bool capped: root.capPct >= 0
    property color barColor: Theme.accent
    property string readout: ""

    implicitWidth: 200
    implicitHeight: 30

    Text {
        anchors.top: parent.top
        text: root.label
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
        width: parent.width - capValue.width - 12
    }
    Text {
        id: capValue
        anchors.top: parent.top
        anchors.right: parent.right
        text: root.readout !== ""
              ? root.readout
              : (root.capped ? root.pct + "% / " + root.capPct + "%" : root.pct + "%")
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 6
        radius: 3
        color: Theme.inset

        // The allowance the cap represents — a quiet wash behind the usage.
        Rectangle {
            visible: root.capped
            width: root.capped ? parent.width * Math.min(100, Math.max(0, root.capPct)) / 100 : 0
            height: parent.height
            radius: 3
            color: Qt.alpha(Theme.accent, 0.22)
        }

        // What is actually used now.
        Rectangle {
            width: parent.width * Math.min(100, Math.max(0, root.pct)) / 100
            height: parent.height
            radius: 3
            color: root.barColor
            Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutQuad } }
        }

        // The cap line itself, so the bright fill reads against its limit.
        Rectangle {
            visible: root.capped
            x: root.capped
               ? Math.max(0, Math.min(parent.width - width,
                                      parent.width * Math.min(100, Math.max(0, root.capPct)) / 100 - width / 2))
               : 0
            width: 2
            height: parent.height
            radius: 1
            color: Theme.textMuted
        }
    }
}