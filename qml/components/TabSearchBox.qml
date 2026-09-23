import QtQuick
import VGRPresenterUI

// The search at the right end of the library dock's tab bar, drawn like one of the tabs: a magnifier,
// the word "Search" and the tab bar's red underline. It is tied to the ACTIVE tab: the tab bar keeps one
// query per tab, so Shows filters the shows, Media the media, and coming back to a tab finds its search
// as you left it. This component is just the box - it shows `text`, reports what the user types through
// `edited`, and clears itself on Esc or the x.
Item {
    id: root

    property string text: ""
    // Shown while empty and idle ("Search"); while the box has focus it says what it will search.
    property string placeholder: qsTr("Search")
    property string focusedPlaceholder: ""

    signal edited(string text)

    // Hugs its own content (icon + placeholder/typed text), the same as every tab beside it - was a fixed 160, wider than "Search"
    // ever needs, so the red underline (spanning the full box) stretched out past the visible text.
    implicitWidth: Math.max(112, glass.x + glass.width + 12 + hint.implicitWidth + 26)
    implicitHeight: 31

    readonly property string hintText: input.activeFocus && root.focusedPlaceholder !== "" ? root.focusedPlaceholder : root.placeholder
    Text {
        id: hint
        visible: false   // measurement-only twin; the real placeholder is inside `input`, drawn with the same font
        text: root.hintText
        font: input.font
    }

    IconGlyph {
        id: glass
        anchors.left: parent.left
        anchors.leftMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        // The stock magnifier is a 10.5 px glyph; enlarged (with its line kept at 2 px) to sit with the tabs.
        readonly property real enlarge: 1.45
        width: 10.5; height: 10.5
        scale: enlarge
        strokeWidth: 2 / enlarge
        name: "search"
        color: "#f1f5f9"
    }

    TextInput {
        id: input
        anchors.left: glass.right
        anchors.leftMargin: 12
        anchors.right: clearButton.left
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        color: "#f1f5f9"
        font.family: Theme.fontFamily
        font.pixelSize: 15
        font.weight: Font.DemiBold
        clip: true
        selectByMouse: true
        // Follows the tab bar (switching to another tab's search). Typing edits it in place.
        text: root.text
        onTextEdited: root.edited(text)
        Keys.onEscapePressed: { input.clear(); root.edited("") }

        Text {
            visible: input.text.length === 0
            anchors.verticalCenter: parent.verticalCenter
            text: input.activeFocus && root.focusedPlaceholder !== "" ? root.focusedPlaceholder : root.placeholder
            color: "#f1f5f9"
            font: input.font
        }
    }

    // Anywhere on the box starts typing.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.IBeamCursor
        onClicked: input.forceActiveFocus()
        z: -1
    }

    // Clear.
    Rectangle {
        id: clearButton
        visible: input.text.length > 0
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 18; height: 18; radius: 9
        color: clearArea.pressed ? "#2a2d3a" : "transparent"
        IconGlyph {
            anchors.centerIn: parent
            name: "close"
            color: Theme.textSecondary
            width: 9; height: 9
        }
        MouseArea {
            id: clearArea
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: { input.clear(); root.edited(""); input.forceActiveFocus() }
        }
    }

    // The tab bar's red underline, the same as the selected tab's.
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 2
        radius: 1
        color: "#ff4d3d"
    }
}
