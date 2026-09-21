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

    title: qsTr("Unsaved changes")
    subtitle: ShowService.showName !== ""
              ? qsTr("\"%1\" has changes that haven't been saved.").arg(ShowService.showName)
              : qsTr("This show has changes that haven't been saved.")
    cardWidth: 460
    saveText: qsTr("Save")
    cancelText: qsTr("Cancel")

    // function() -> bool: saves the show, true when it ended up saved. Set by Main.qml.
    property var saveAction: null
    property var _then: null

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
        root.close()
    }

    // Save -> continue only if the save actually happened (the user may cancel the
    // Save As dialog, or the write may fail — then we stay where we were).
    onAccepted: {
        if (root.saveAction && root.saveAction())
            root._proceed()
        else {
            root._then = null
            root.close()
        }
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
        text: qsTr("Don't save")
        variant: "danger"
        onClicked: root._proceed()
    }
}
