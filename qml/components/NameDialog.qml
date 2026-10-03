import QtQuick
import VGRPresenterUI

// Reusable "type a name" dialog: dim scrim + centered card with one text field, in the same style as
// ConfirmDialog. The caller opens it with the field's starting text and reacts to accepted(text) /
// dismissed(); Enter accepts, Escape or a click outside dismisses. It closes itself on either.
//
//   NameDialog { id: nameDialog; title: qsTr("New category"); confirmLabel: qsTr("Create")
//                onAccepted: (text) => service.createCategory(text) }
//   nameDialog.open("")
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property string title: ""
    property string placeholder: ""
    property string confirmLabel: qsTr("Create")
    // Whether an empty name may be accepted (a creation that has a default name allows it).
    property bool allowEmpty: false

    signal accepted(string text)
    signal dismissed()

    property bool shown: false
    readonly property string text: field.text.trim()

    function open(initial) {
        field.text = initial === undefined ? "" : initial
        shown = true
        field.focusInput()
    }
    function close() { shown = false }

    anchors.fill: parent
    visible: opacity > 0
    enabled: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 130 } }

    function accept() {
        if (root.text === "" && !root.allowEmpty)
            return
        const value = root.text
        close()
        accepted(value)
    }
    function dismiss() {
        close()
        dismissed()
    }

    Keys.onEscapePressed: root.dismiss()

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.dismiss()
    }

    Rectangle {
        width: 380
        height: dialogCol.implicitHeight + 2 * Theme.space6
        anchors.centerIn: parent
        radius: Theme.radiusLg
        color: Theme.surface

        Column {
            id: dialogCol
            x: Theme.space6
            y: Theme.space6
            width: parent.width - 2 * Theme.space6
            spacing: Theme.space5

            Text {
                width: parent.width
                text: root.title
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textLg
                font.weight: Font.DemiBold
                color: Theme.textPrimary
                wrapMode: Text.WordWrap
            }

            SettingsField {
                id: field
                width: parent.width
                showLabel: false
                placeholder: root.placeholder
                onAccepted: root.accept()
            }

            Row {
                anchors.right: parent.right
                spacing: Theme.space3

                AppButton {
                    text: qsTr("Cancel")
                    variant: "secondary"
                    onClicked: root.dismiss()
                }
                AppButton {
                    text: root.confirmLabel
                    variant: "primary"
                    enabled: root.allowEmpty || root.text !== ""
                    onClicked: root.accept()
                }
            }
        }
    }
}
