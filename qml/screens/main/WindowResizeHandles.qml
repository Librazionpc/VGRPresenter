import QtQuick
import VGRPresenterUI
import "../../components"

Item {
    id: root
    // Named for the header/caption-clearance scenario, which walks this item's
    // Edge children and proves none of their rects overlaps the caption cluster.
    objectName: "selfTestFrameGrips"
    z: 20000

    // ---- The one rect the frame must leave alone: the caption buttons -------
    property real captionReserveWidth: 0
    property real captionReserveHeight: 0

    readonly property var win: Window.window
    readonly property int grip: 8
    readonly property bool windowed: root.win !== null && root.win.visibility === Window.Windowed

    component Edge: Item {
        id: edge
        property int edges: 0
        property int shape: Qt.SizeHorCursor

        PositionHoverArea {
            anchors.fill: parent
            objectName: "selfTestFrameGrip"
            cursorShape: edge.shape
            showCursor: root.windowed
            cursorPriority: AppCursor.priorityHandle
        }
        MouseArea {
            anchors.fill: parent
            enabled: root.windowed
            acceptedButtons: Qt.LeftButton
            onPressed: if (root.win) root.win.startSystemResize(edge.edges)
        }
    }

    // Sides. The top and right strips stop short of the caption cluster.
    Edge { edges: Qt.LeftEdge;   shape: Qt.SizeHorCursor; x: 0; y: root.grip; width: root.grip; height: root.height - 2 * root.grip }
    Edge {
        edges: Qt.RightEdge;  shape: Qt.SizeHorCursor
        x: root.width - root.grip
        y: root.grip + root.captionReserveHeight
        width: root.grip
        height: Math.max(0, root.height - 2 * root.grip - root.captionReserveHeight)
    }
    Edge {
        edges: Qt.TopEdge;    shape: Qt.SizeVerCursor
        x: root.grip; y: 0
        width: Math.max(0, root.width - 2 * root.grip - root.captionReserveWidth)
        height: root.grip
    }
    Edge { edges: Qt.BottomEdge; shape: Qt.SizeVerCursor; x: root.grip; y: root.height - root.grip; width: root.width - 2 * root.grip; height: root.grip }
    Edge { edges: Qt.LeftEdge | Qt.TopEdge;     shape: Qt.SizeFDiagCursor; x: 0; y: 0; width: root.grip; height: root.grip }
    Edge { edges: Qt.LeftEdge | Qt.BottomEdge;  shape: Qt.SizeBDiagCursor; x: 0; y: root.height - root.grip; width: root.grip; height: root.grip }
    Edge { edges: Qt.RightEdge | Qt.BottomEdge; shape: Qt.SizeFDiagCursor; x: root.width - root.grip; y: root.height - root.grip; width: root.grip; height: root.grip }
}