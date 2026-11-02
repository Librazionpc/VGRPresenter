import QtQuick
import VGRPresenterUI

ModalCard {
    id: root
    objectName: "unsavedChangesDialog"

    title: qsTr("Unsaved changes")
    subtitle: ShowService.showName !== ""
              ? qsTr("\"%1\" has changes that haven't been saved.").arg(ShowService.showName)
              : qsTr("This show has changes that haven't been saved.")
    cardWidth: 460
    saveText: qsTr("Save")
    cancelText: qsTr("Cancel")

    property var saveAction: null
    property var _then: null
    // True while a save is in flight: the dialog stays up (and its buttons wait)
    // rather than letting a second Save land on top of the first.
    property bool saving: false

    function ask(then) {
        root._then = then
        root.open()
    }

    function _proceed() {
        const f = root._then
        root._then = null
        root.close()
        if (f)
            f()
    }

    onCancelled: {
        root._then = null
        root.saving = false
        root.close()
    }

    onAccepted: root.confirm()
    function confirm() {
        if (!root.saveAction || root.saving) {
            if (!root.saveAction) {
                root._then = null
                root.close()
            }
            return
        }
        root.saving = true
        root.saveAction(function (saved) {
            root.saving = false
            if (saved)
                root._proceed()
            else {
                root._then = null
                root.close()
            }
        })
    }

    Text {
        width: parent.width
        text: qsTr("If you don't save, your latest changes will be lost.")
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
    }

    AppButton {
        objectName: "unsavedDontSaveButton"
        text: qsTr("Don't save")
        variant: "danger"
        onClicked: root._proceed()
    }
}