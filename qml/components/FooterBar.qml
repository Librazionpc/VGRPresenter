import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property bool showSave: true
    property string saveText: "Save Changes"
    property string cancelText: "Cancel"
    // Hook for the self-test driver, derived by the host from its own
    // objectName (ModalCard passes its own — the same "<objectName>_card"
    // convention it already publishes for the card): a scenario clicks the REAL
    // Save/Cancel button of a named dialog instead of calling its handler, so
    // what it proves is the operator's own click path. Empty on an unnamed
    // host, which leaves both buttons unnamed as before.
    property string hookName: ""

    signal cancelRequested()
    signal saveRequested()

    // Short enough to read as a footer strip, not a section of its own — the
    // 34px buttons keep a 4px breathing margin top and bottom, and both hosts
    // (ModalCard's flick reserve, ModalShell's row height) read THIS value, so
    // the dialogs tighten with it automatically.
    implicitHeight: 42
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
