import QtQuick
import VGRPresenterUI

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
    property string error: ""

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
        // The field's caption is the same "subscript" family as a settings row's
        // description (see Theme.textSub) — the unit suffix below stays textXs.
        font.pixelSize: Theme.textSub
    }

    Rectangle {
        width: parent.width
        height: 34
        radius: Theme.radiusMd
        color: Theme.inset
        border.width: 1
        border.color: root.error !== "" ? Theme.danger
                    : input.activeFocus ? Theme.accent : Theme.border
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

    Text {
        width: parent.width
        visible: root.error !== ""
        text: root.error
        wrapMode: Text.WordWrap
        color: Theme.dangerLight
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
    }
}