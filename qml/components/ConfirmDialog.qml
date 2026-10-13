import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// Reusable confirm dialog: dim scrim + centered card, one decision slot.
// Caller supplies title/body/button labels and reacts to confirmed()/dismissed().
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property string title: ""
    property string message: ""
    property string confirmLabel: "Delete"
    property string cancelLabel: "Cancel"
    // "danger" renders the confirm button red; anything else uses primary styling.
    property string confirmVariant: "danger"

    signal confirmed()
    signal dismissed()

    // Same as NameDialog: window-level, so a card that cannot fit the pane that
    // declares it still stays inside the window (see the modal-fit scenario in
    // qml/Main.qml and tools/selftest/run_modal_fit_test.sh).
    parent: Overlay.overlay
    // The scrim below is NOT scaled (see AppScale.qml): it IS the dialog's black
    // box, and a scale on this root shrinks it away from the window's own edges.
    // The app scale goes on the CARD instead (same as NameDialog).
    anchors.fill: parent
    visible: opacity > 0
    enabled: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 130 } }

    property bool shown: false
    function open() { shown = true; openCount = openCount + 1 }
    function close() { shown = false }

    // Bumps on every open so callers with stale contexts (item deleted while
    // dialog was up) can reset their bindings via onOpenCountChanged.
    property int openCount: 0

    // Shared scrim — also consumes wheel events so scrolling can't leak
    // into the page's Flickable behind the dialog.
    ModalScrim {
        anchors.fill: parent
        onDismissed: root.dismissed()
    }

    Rectangle {
        id: card
        // Hook for the modal-fit scenario — same convention as ModalCard's card.
        objectName: root.objectName === "" ? "" : root.objectName + "_card"
        // The app's one scale, on the card (see AppScale.qml) — never on the root,
        // whose scrim has to stay window-sized.
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
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

            Text {
                width: parent.width
                text: root.message
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
            }

            Row {
                anchors.right: parent.right
                spacing: Theme.space3

                AppButton {
                    text: root.cancelLabel
                    variant: "secondary"
                    onClicked: root.dismissed()
                }
                AppButton {
                    text: root.confirmLabel
                    variant: root.confirmVariant
                    onClicked: root.confirmed()
                }
            }
        }
    }
}
