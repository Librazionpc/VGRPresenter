import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property bool showSave: true
    property string saveText: "Save Changes"
    property string cancelText: "Cancel"

    signal cancelRequested()
    signal saveRequested()

    implicitHeight: 48
    color: Theme.surface

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.border
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.space5
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space3

        AppButton {
            text: root.cancelText
            variant: "secondary"
            onClicked: root.cancelRequested()
        }

        AppButton {
            visible: root.showSave
            text: root.saveText
            variant: "primary"
            onClicked: root.saveRequested()
        }
    }
}
