import QtQuick
import QtQuick.Effects
import VGRPresenterUI

Rectangle {
    id: root

    // Keep popups open when the user clicks outside them. Callers can opt in
    // to backdrop dismissal for a specific transient dialog.
    property bool clickToDismiss: false

    property Item blurSource: null

    signal dismissed()

    readonly property bool canBlur: GraphicsInfo.api !== GraphicsInfo.Software
    readonly property bool blurActive: root.blurSource !== null && root.canBlur

    anchors.fill: parent
    color: root.blurActive ? "transparent" : "#cc000000"

    MultiEffect {
        anchors.fill: parent
        visible: root.blurActive
        source: root.blurActive ? root.blurSource : null
        autoPaddingEnabled: false
        blurEnabled: true
        blurMax: 48
        blur: 1.0
    }
    Rectangle {
        visible: root.blurActive
        anchors.fill: parent
        color: "#80000000"
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onClicked: if (root.clickToDismiss) root.dismissed()

        onWheel: (wheel) => wheel.accepted = true
    }
}