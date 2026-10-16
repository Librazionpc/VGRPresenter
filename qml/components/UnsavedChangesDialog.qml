import QtQuick
import VGRPresenterUI

// "Save changes?" — asked before something that would throw away the open show's
// unsaved changes (New show, Open, closing the window). ShowSession.guardUnsaved
// calls ask(then); `then` runs only if the user lets us proceed:
//   Save        -> saveAction() ran and the show ended up saved -> proceed
//   Don't save  -> proceed, changes discarded
//   Cancel / X  -> nothing happens
// Lives at the top of Main.qml (not inside EditScreen) so it can appear over any
// screen, including when New show is clicked from the Show screen.
ModalCard {
    id: root
    // Names this dialog's parts for the quit-path harness (run_quit_test.sh): the
    // card becomes `<objectName>_card` and the footer's buttons
    // `<objectName>_save` / `<objectName>_cancel` (see ModalCard's hookName), so a
    // scenario can answer the question the way the user does — a real click on
    // "Save" / "Don't save" — instead of calling into the dialog's internals.
    objectName: "unsavedChangesDialog"

    title: qsTr("Unsaved changes")
    subtitle: ShowService.showName !== ""
              ? qsTr("\"%1\" has changes that haven't been saved.").arg(ShowService.showName)
              : qsTr("This show has changes that haven't been saved.")
    cardWidth: 460
    saveText: qsTr("Save")
    cancelText: qsTr("Cancel")

    // function(then) -> void: STARTS the save and calls `then(saved)` when the
    // verdict lands — the write is a queued document job (see ShowService), so the
    // answer cannot be a return value any more. Set by Main.qml.
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

    // Save -> continue only if the save actually happened (the user may cancel the
    // Save As dialog, or the write may fail — then we stay where we were). The
    // verdict arrives asynchronously, so this waits for it: the dialog stays open
    // (and `saving` holds its buttons) until the file is written or refused.
    //
    // The body is a callable function so the answer has ONE implementation: the
    // footer's Save button runs this, and so does a scenario whose synthetic click
    // could not land (run_quit_test.sh; a click that silently does nothing would
    // otherwise read as "the app hangs on close").
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
