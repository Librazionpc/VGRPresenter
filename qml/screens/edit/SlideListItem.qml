import QtQuick

// One row in the Edit screen's slide list. Pulled out of EditScreen.qml's
// Repeater so the CRUD-row presentation (selection, hover, delete) is a
// self-contained, reusable unit instead of an inline anonymous delegate —
// it only knows about its own props/signals, not the model.
//
// Literal colors, not Theme.* — same AOT-compiler limitation as
// AppMenuBar.qml at this nesting depth.
Rectangle {
    id: root

    required property int index
    required property int num
    required property bool active
    required property string tag
    required property string tagColor
    required property string title
    required property string line1
    required property string line2
    required property string ref

    signal selected()
    signal duplicateRequested()
    signal deleteRequested()
    // (x, y) are in this item's own local coordinate space — the consumer
    // maps them into whatever coordinate space its context-menu popup
    // needs (see EditScreen.qml, which escapes the slide list's Flickable
    // clip via mapToItem).
    signal contextMenuRequested(real x, real y)

    // Also true while hovering the delete button itself: it sits on top of
    // hoverArea, and Qt Quick only delivers hover to the topmost MouseArea
    // at a given point, so hoverArea.containsMouse alone would flip false
    // (and the button fade out) the instant the cursor reached it.
    readonly property bool hovered: hoverArea.containsMouse || deleteArea.containsMouse

    height: 124
    width: 256
    border.color: root.active ? "#6c5ce7" : "#232530"
    border.width: root.active ? 1.2 : 1
    color: "#161823"
    radius: 9

    Rectangle {
        visible: root.active
        height: parent.height
        width: 3
        radius: 2
        color: "#6c5ce7"
    }

    Rectangle {
        x: 16
        y: 4
        height: 108
        width: 224
        border.color: "#262a38"
        border.width: 1
        color: "#0d0f16"
        radius: 8

        Column {
            visible: root.tag !== ""
            anchors.centerIn: parent
            width: parent.width - 12
            spacing: 3

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                color: root.tagColor || "#9b8ff5"
                font.family: "Inter"
                font.pixelSize: 7
                font.weight: Font.Normal
                text: root.tag
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                color: "#f2f4fa"
                font.family: "Inter"
                font.pixelSize: 12
                text: root.title
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                color: "#8a8fa3"
                font.family: "Inter"
                font.pixelSize: 8
                text: root.line1
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                color: "#8a8fa3"
                font.family: "Inter"
                font.pixelSize: 8
                text: root.line2
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                visible: root.ref !== ""
                horizontalAlignment: Text.AlignHCenter
                color: "#6b7080"
                font.family: "Inter"
                font.pixelSize: 7
                text: root.ref
            }
        }
    }

    Rectangle {
        x: 218
        y: 8
        height: 18
        width: 18
        color: "#1c2030"
        radius: 4

        Text {
            anchors.centerIn: parent
            color: "#c9cedd"
            font.family: "Inter"
            font.pixelSize: 8
            text: String(root.num)
        }
    }

    // Full-row select/duplicate area — declared before the delete button so
    // the button's own MouseArea stacks on top and isn't swallowed by this.
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                root.contextMenuRequested(mouse.x, mouse.y)
            else
                root.selected()
        }
        onDoubleClicked: root.duplicateRequested()
    }

    // Hover-revealed delete button. A single click is too easy to trigger
    // by accident for a destructive action, so it stays a deliberate
    // button rather than living on the same click/double-click gesture as
    // select/duplicate.
    Rectangle {
        x: 196
        y: 8
        height: 18
        width: 18
        radius: 4
        opacity: root.hovered ? 1 : 0
        visible: opacity > 0
        color: deleteArea.containsMouse ? "#3a2230" : "#1c2030"
        Behavior on opacity { NumberAnimation { duration: 100 } }
        Behavior on color { ColorAnimation { duration: 100 } }

        Text {
            anchors.centerIn: parent
            color: "#ff6b61"
            font.pixelSize: 10
            text: "✕"
        }

        MouseArea {
            id: deleteArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.deleteRequested()
        }
    }
}
