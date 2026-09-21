import QtQuick
import VGRPresenterUI
import "../../components"

// Resize borders for the frameless window (a native frame gives these for free; without
// it the window would be stuck at its size). Eight thin strips around the window edge —
// press one and the OS takes over the resize (startSystemResize), so it behaves exactly
// like a native resize: snapping, min/max size, and so on. Inactive while maximized.
//
// The resize cursor comes from PositionHoverArea (pointer-position truth), the same
// mechanism every other cursor in the app uses — MouseArea.cursorShape sticks in this
// build. A plain MouseArea above it takes the press.
Item {
    id: root

    readonly property var win: Window.window
    readonly property int grip: 6
    // Not `visible`: PositionHoverArea walks its ancestors' visible/enabled, and a binding
    // on this item that reads the window's state closes a loop through that walk. A plain
    // flag gating the cursor and the press does the same job.
    readonly property bool windowed: root.win !== null && root.win.visibility === Window.Windowed

    component Edge: Item {
        id: edge
        property int edges: 0
        property int shape: Qt.SizeHorCursor

        PositionHoverArea {
            anchors.fill: parent
            cursorShape: edge.shape
            showCursor: root.windowed
        }
        MouseArea {
            anchors.fill: parent
            enabled: root.windowed
            acceptedButtons: Qt.LeftButton
            onPressed: if (root.win) root.win.startSystemResize(edge.edges)
        }
    }

    // Sides.
    Edge { edges: Qt.LeftEdge;   shape: Qt.SizeHorCursor; x: 0; y: root.grip; width: root.grip; height: root.height - 2 * root.grip }
    Edge { edges: Qt.RightEdge;  shape: Qt.SizeHorCursor; x: root.width - root.grip; y: root.grip; width: root.grip; height: root.height - 2 * root.grip }
    Edge { edges: Qt.TopEdge;    shape: Qt.SizeVerCursor; x: root.grip; y: 0; width: root.width - 2 * root.grip; height: root.grip }
    Edge { edges: Qt.BottomEdge; shape: Qt.SizeVerCursor; x: root.grip; y: root.height - root.grip; width: root.width - 2 * root.grip; height: root.grip }
    // Corners.
    Edge { edges: Qt.LeftEdge | Qt.TopEdge;     shape: Qt.SizeFDiagCursor; x: 0; y: 0; width: root.grip; height: root.grip }
    Edge { edges: Qt.RightEdge | Qt.TopEdge;    shape: Qt.SizeBDiagCursor; x: root.width - root.grip; y: 0; width: root.grip; height: root.grip }
    Edge { edges: Qt.LeftEdge | Qt.BottomEdge;  shape: Qt.SizeBDiagCursor; x: 0; y: root.height - root.grip; width: root.grip; height: root.grip }
    Edge { edges: Qt.RightEdge | Qt.BottomEdge; shape: Qt.SizeFDiagCursor; x: root.width - root.grip; y: root.height - root.grip; width: root.grip; height: root.grip }
}
