import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// Reusable "type a name" dialog: dim scrim + centered card with one text field, in the same style as
// ConfirmDialog. The caller opens it with the field's starting text and reacts to accepted(text) /
// dismissed(); Enter accepts and Escape dismisses. Outside clicks are trapped by
// the shared scrim and leave the dialog open.
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

    // Optional rule a caller adds: function(text) returning "" when the name is fine, else
    // the sentence that says why not (e.g. "There is already a folder called ...").
    // The reason shows INSIDE the dialog, so Create never just does nothing.
    property var validator: null
    property string error: ""

    signal accepted(string text)
    signal dismissed()

    property bool shown: false
    readonly property string text: field.text.trim()

    function open(initial) {
        root.error = ""
        field.text = initial === undefined ? "" : initial
        shown = true
        field.focusInput()
    }
    function close() { shown = false }

    // The dialog belongs to the WINDOW, not to whatever pane declares it (the
    // library dock is only ~250px tall on a small window, and a card centered in
    // it then hangs below the window's own bottom edge — measured at 820x560, the
    // name dialog sat 31px outside the window; see
    // tools/selftest/run_modal_fit_test.sh). TemplatePickerModal reparents itself
    // the same way, for the same reason.
    parent: Overlay.overlay
    // The scrim below is NOT scaled (see AppScale.qml): it IS the dialog's black
    // box, and a scale on this root shrinks it away from the window's own edges.
    // The app scale goes on the CARD instead, read from the window — the
    // reparenting above leaves this component no scaled ancestor to inherit it
    // from.
    anchors.fill: parent
    visible: opacity > 0
    enabled: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 130 } }

    function accept() {
        if (root.text === "" && !root.allowEmpty) {
            root.error = qsTr("Type a name first.")
            return
        }
        const why = root.validator ? String(root.validator(root.text) || "") : ""
        if (why !== "") {
            root.error = why
            return
        }
        const value = root.text
        close()
        accepted(value)
    }
    function dismiss() {
        close()
        dismissed()
    }

    Keys.onEscapePressed: root.dismiss()

    // ---- Esc = dismiss, Enter = accept (qml/settings/Hotkeys.qml) --------
    // The dialog's existing keys, routed through the app's stack as well: the
    // window's Shortcut sees the key before the focused field does, so Enter in
    // the name field accepts here instead of only when the field happens to
    // forward it. The Keys handler above stays as the fallback for a keyboard
    // focus path that reaches this item directly.
    property int hotkeyScope: 0
    onShownChanged: {
        // Above whatever dialog is already up: a flat z would open this BEHIND an Edit/Add card (ModalCard bumps its own
        // z on every open), which is how the colour card appeared below the Edit Style card.
        if (root.shown)
            root.z = ModalStack.nextZ()
        if (root.shown && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: root.title !== "" ? root.title : "Name dialog",
                cancel: function () { root.dismiss() },
                accept: function () { root.accept() }
            })
        } else if (!root.shown && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    Component.onDestruction: Hotkeys.close(root.hotkeyScope)

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.dismiss()
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

            SettingsField {
                id: field
                width: parent.width
                showLabel: false
                placeholder: root.placeholder
                onAccepted: root.accept()
                error: root.error   // red border + the reason under the box
                onTextEdited: root.error = ""
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
