import QtQuick
import VGRPresenterUI

// The left sidebar of a library pane (Shows, Media, Scripture / The Table): the dark
// column with its 1 px divider on the right, that the user can DRAG wider or narrower.
// Every library pane builds its sidebar from this, so they look and behave the same:
//
//   LibrarySidebar {
//       id: sidebar
//       height: parent.height
//       SidebarRow { ... }            // children fill the sidebar (default property)
//   }
//   Item { x: sidebar.width; ... }    // the content beside it follows sidebar.width
//
// The width is state this component owns - drag the divider (a SplitHandle) to change it,
// double-click it to go back to `defaultWidth`. Panes stay instantiated when their tab is
// hidden, so each keeps its own width for the session.
Rectangle {
    id: root

    property real defaultWidth: 320
    property real minWidth: 200
    // Leave the content beside the sidebar at least 360 px.
    property real maxWidth: parent ? Math.max(minWidth, parent.width - 360) : 520
    property bool resizable: true

    // What the pane puts in the sidebar.
    default property alias content: body.data

    width: defaultWidth
    color: "#0f1015"
    // Above the content beside it, so the drag strip's outer half is reachable.
    z: 10

    Item {
        id: body
        anchors.fill: parent
    }

    // The divider line.
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: "#232530"
    }

    SplitHandle {
        visible: root.resizable
        value: root.width
        minValue: root.minWidth
        maxValue: root.maxWidth
        onDragged: (v) => root.width = v
        onResetRequested: root.width = root.defaultWidth
    }
}
