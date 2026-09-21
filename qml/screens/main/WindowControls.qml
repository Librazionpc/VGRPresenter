import QtQuick
import VGRPresenterUI
import "../../components"

// Minimize / maximize-restore / close for the frameless window — the app's own
// title-bar buttons (the native ones are gone with the native title bar, which is
// what lets the File/Edit/View/Help menu live in the top strip). Each button is a
// full-height hit target with a hover wash (close goes red), like a native caption.
//
// Hover comes from PositionHoverArea (pointer-position truth) — this build never
// delivers hover-exit to MouseAreas, so a containsMouse wash would stick.
//
// Literal colors, not Theme.* — same AOT limitation as AppMenuBar.qml at this depth.
Item {
    id: root

    width: 138
    height: 32

    readonly property var win: Window.window
    // A frameless window that is "maximized" reports FullScreen on Windows (Qt uses the
    // full screen geometry for it), so both states count as maximized.
    readonly property bool maximized: root.win !== null
        && (root.win.visibility === Window.Maximized || root.win.visibility === Window.FullScreen)

    function toggleMaximize() {
        if (!root.win)
            return
        if (root.maximized)
            root.win.showNormal()
        else
            root.win.showMaximized()
    }

    component CaptionButton: Item {
        id: btn
        property bool danger: false
        readonly property bool hovered: hoverArea.hovered
        readonly property color glyphColor: btn.danger && btn.hovered ? "#ffffff" : "#c9d1e0"
        default property alias glyph: glyphHolder.data
        signal activated()

        width: 46
        height: 32

        Rectangle {
            anchors.fill: parent
            color: btn.hovered ? (btn.danger ? "#e5534b" : "#1fffffff") : "transparent"
            Behavior on color { ColorAnimation { duration: 90 } }
        }
        Item {
            id: glyphHolder
            anchors.centerIn: parent
            width: 10
            height: 10
        }
        PositionHoverArea {
            id: hoverArea
            anchors.fill: parent
            showCursor: false   // a caption button keeps the arrow, like a native one
            onClicked: btn.activated()
        }
    }

    Row {
        anchors.right: parent.right

        CaptionButton {
            id: minimizeButton
            onActivated: if (root.win) root.win.showMinimized()
            Rectangle {
                anchors.centerIn: parent
                width: 10
                height: 1
                color: minimizeButton.glyphColor
            }
        }

        CaptionButton {
            id: maximizeButton
            onActivated: root.toggleMaximize()
            // Maximized shows the "restore" glyph (two overlapping squares).
            Rectangle {
                visible: !root.maximized
                anchors.centerIn: parent
                width: 10
                height: 10
                color: "transparent"
                border.width: 1
                border.color: maximizeButton.glyphColor
            }
            Rectangle {
                visible: root.maximized
                x: 2
                y: 0
                width: 8
                height: 8
                color: "transparent"
                border.width: 1
                border.color: maximizeButton.glyphColor
            }
            Rectangle {
                visible: root.maximized
                x: 0
                y: 2
                width: 8
                height: 8
                color: "#12131a"
                border.width: 1
                border.color: maximizeButton.glyphColor
            }
        }

        CaptionButton {
            id: closeButton
            danger: true
            // close() runs the window's onClosing guard (unsaved-changes prompt).
            onActivated: if (root.win) root.win.close()
            Rectangle {
                anchors.centerIn: parent
                width: 14
                height: 1
                rotation: 45
                color: closeButton.glyphColor
            }
            Rectangle {
                anchors.centerIn: parent
                width: 14
                height: 1
                rotation: -45
                color: closeButton.glyphColor
            }
        }
    }
}
