import QtQuick
import VGRPresenterUI

// One labeled settings text field — small caption above, inset rounded
// box with a TextInput inside, optional right-aligned unit suffix (e.g.
// "kbps") and optional placeholder. Shared by the settings screens'
// value inputs (Recording's bitrates, server URL, stream key, and any
// future section) so field styling can't drift between screens.
Column {
    id: root

    property string label: ""
    // Two-column forms embed the field beside their own label — hide this
    // component's caption line entirely (no reserved empty line).
    property bool showLabel: true
    property alias text: input.text
    // Stream keys render as dots — pass TextInput.Password.
    property alias echoMode: input.echoMode
    property string suffix: ""
    property string placeholder: ""

    // User-driven edits only (not programmatic `text =` assignments) — the
    // dialog pattern: load once on open, write back through this.
    signal textEdited(string text)
    // Enter pressed in the field.
    signal accepted()

    // Puts the caret in the field (selecting what is there), for a dialog that opens on it.
    function focusInput() {
        input.forceActiveFocus()
        input.selectAll()
    }

    width: 200
    spacing: 6

    Text {
        visible: root.showLabel
        text: root.label
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
    }

    Rectangle {
        width: parent.width
        height: 34
        radius: Theme.radiusMd
        color: Theme.inset
        border.width: 1
        border.color: input.activeFocus ? Theme.accent : Theme.border
        Behavior on border.color { ColorAnimation { duration: 100 } }

        // Placeholder — visible only while empty and unfocused.
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            visible: input.text === "" && !input.activeFocus
            text: root.placeholder
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
        }

        TextInput {
            id: input
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.right: parent.right
            anchors.rightMargin: root.suffix !== "" ? suffixLabel.width + 20 : 12
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            clip: true
            selectByMouse: true
            onTextEdited: root.textEdited(input.text)
            onAccepted: root.accepted()
        }

        Text {
            id: suffixLabel
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            visible: root.suffix !== ""
            text: root.suffix
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }
    }
}
