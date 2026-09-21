import QtQuick
import VGRPresenterUI

// One row of a library sidebar: icon, label, a count, and (optionally) a remove button
// that shows while the pointer is over the row. The selected row gets the red edge and
// a lifted background. Used by the Media sidebar's "All" / "Inputs" / folders.
Rectangle {
    id: root

    property string icon: "folder"
    property string label: ""
    // Shown at the right; leave empty to hide.
    property string count: ""
    property bool selected: false
    // A row of a TREE (nested folders): indented by `depth`, with a chevron column that shows an expander when
    // the row has children. Rows that are not part of a tree leave these alone.
    property bool tree: false
    property int depth: 0
    property bool expandable: false
    property bool expanded: false
    readonly property int inset: tree ? 16 + depth * 14 : 0

    // Something is loading for this row: a spinner takes the place of the count.
    property bool busy: false
    // Adds an "x" at the right while hovered; clicking it emits removeRequested().
    property bool removable: false

    signal clicked()
    signal removeRequested()
    signal toggleRequested()

    height: 30
    radius: 6
    color: selected ? "#1e1f28" : (rowHover.hovered ? "#16171e" : "transparent")

    Rectangle {
        visible: root.selected
        x: 0; y: 5; width: 3; height: 20
        color: Theme.danger; radius: 1.5
    }
    IconGlyph {
        name: root.icon
        color: root.selected ? Theme.danger : Theme.textSecondary
        x: 12 + root.inset; y: 8; width: 14; height: 14
    }
    Text {
        x: 34 + root.inset
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - 34 - root.inset - (removeButton.visible ? 30 : ((root.count !== "" || root.busy) ? 44 : 10))
        text: root.label
        color: root.selected ? Theme.textPrimary : Theme.textSecondary
        elide: Text.ElideRight
        font.family: Theme.fontFamily; font.pixelSize: 12
    }
    Text {
        visible: root.count !== "" && !root.busy && !removeButton.visible
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: root.count
        color: Theme.textMuted
        font.family: Theme.fontFamily; font.pixelSize: 11
    }

    BusySpinner {
        visible: root.busy && !removeButton.visible
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        width: 14; height: 14
        thickness: 2
        color: Theme.textMuted
    }

    // The expander (tree rows that have something inside).
    Item {
        visible: root.tree && root.expandable
        x: 2 + root.depth * 14
        anchors.verticalCenter: parent.verticalCenter
        width: 16; height: 22
        z: 2
        IconGlyph {
            anchors.centerIn: parent
            name: "chevronRight"
            color: Theme.textSecondary
            width: 3; height: 6
            scale: 1.4
            rotation: root.expanded ? 90 : 0
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.toggleRequested()
        }
    }

    // The whole row selects...
    PositionHoverArea {
        id: rowHover
        anchors.fill: parent
        onClicked: root.clicked()
    }

    // ...except the remove button, which sits above it. (A plain MouseArea, not a PositionHoverArea:
    // the button's visibility follows the row's hover, and a hover area reads its ancestors'
    // visibility - that would be a binding loop. It has no hover styling of its own to need one.)
    Rectangle {
        id: removeButton
        visible: root.removable && rowHover.hovered
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        width: 22; height: 22; radius: 5
        color: removeArea.pressed ? "#2a2d3a" : "transparent"
        IconGlyph {
            anchors.centerIn: parent
            name: "close"
            color: Theme.textSecondary
            width: 10; height: 10
        }
        MouseArea {
            id: removeArea
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.removeRequested()
        }
    }
}
