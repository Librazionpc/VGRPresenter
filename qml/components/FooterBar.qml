import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property bool showSave: true
    property string saveText: "Save Changes"
    property string cancelText: "Cancel"
    property string hookName: ""

    signal cancelRequested()
    signal saveRequested()

    implicitHeight: 64
    color: Theme.surface

    Rectangle {
        anchors.top: parent.top
        width: parent.width
        height: 1
        color: Theme.border
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.space6
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space3

        AppButton {
            objectName: root.hookName === "" ? "" : root.hookName + "_cancel"
            text: root.cancelText
            variant: "secondary"
            onClicked: root.cancelRequested()
        }

        AppButton {
            objectName: root.hookName === "" ? "" : root.hookName + "_save"
            visible: root.showSave
            text: root.saveText
            variant: "primary"
            onClicked: root.saveRequested()
        }
    }
}