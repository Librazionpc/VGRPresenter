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
    // Named for the header/caption-clearance scenario, which walks this item's
    // Edge children and proves none of their rects overlaps the caption cluster.
    objectName: "selfTestFrameGrips"
    // Keep the hit targets above screen content so controls flush to the
    // window edge cannot intercept the native resize gesture. Modal overlays
    // intentionally use a higher z value and remain interactive on top.
    z: 20000

    // ---- The one rect the frame must leave alone: the caption buttons -------
    // The frameless window's minimize/maximize/close (AppHeader's WindowControls)
    // live in the window's top-right corner, and these grips do NOT scale with the
    // app while the buttons DO (the buttons are drawn inside the scaled header;
    // these strips are the window's own frame — see Main.qml). At a full-size
    // window the 8px top/right strips only grazed the buttons, but the smaller the
    // window scaled the bigger a share of them the strips covered — at 820x560 an
    // 8px strip is nearly half of an 18px-tall close button, so its top and right
    // edges started a RESIZE (and showed the size cursor) instead of minimizing or
    // closing. "It doesn't happen at max scale, only when it scaled down" — a
    // scaled-down window is exactly when these strips grow past the buttons.
    // Main.qml passes the cluster's rect in WINDOW pixels (design size x uiScale),
    // so the reserve tracks the buttons at every scale.
    property real captionReserveWidth: 0
    property real captionReserveHeight: 0

    readonly property var win: Window.window
    readonly property int grip: 8
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
            // The first strip declared (the left edge) answers to this name; the
            // cursor scenario reads its hover/cursor state to tell "the grip is
            // not hovered" apart from "it is hovered and the OS still shows an
            // arrow" — the two faces of "the resize cursor doesn't show".
            objectName: "selfTestFrameGrip"
            cursorShape: edge.shape
            showCursor: root.windowed
            // priorityHandle, NOT the default hover claim: these strips sit ON
            // TOP of ordinary content that also claims the pointer (the header
            // and screen edges run right up to the window frame), and two
            // equal-priority claims over one point alternately win — which is
            // the user's "when I hover the app's border to rescale it the
            // cursor just FLASHES and never shows the size arrow". A grip is a
            // finer target than the body under it; it outranks it (see
            // AppCursor's priority block).
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
    // Corners. The SCALED caption buttons own the top-right one (no grip there: a
    // strip over the close button is a button that does not close the window);
    // the other three are ordinary frame corners.
    Edge { edges: Qt.LeftEdge | Qt.TopEdge;     shape: Qt.SizeFDiagCursor; x: 0; y: 0; width: root.grip; height: root.grip }
    Edge { edges: Qt.LeftEdge | Qt.BottomEdge;  shape: Qt.SizeBDiagCursor; x: 0; y: root.height - root.grip; width: root.grip; height: root.grip }
    Edge { edges: Qt.RightEdge | Qt.BottomEdge; shape: Qt.SizeFDiagCursor; x: root.width - root.grip; y: root.height - root.grip; width: root.grip; height: root.grip }
}
