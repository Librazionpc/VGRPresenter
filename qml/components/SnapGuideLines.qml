import QtQuick

// Renders the thin alignment-guide lines produced by EditScreen.qml's
// snapMove/snapResize helpers while dragging/resizing a canvas object — the
// pink/blue lines a design tool shows when an edge lines up with the canvas
// center or another object. Reusable: just bind `guides` to whatever
// snapMove/snapResize last returned and drop this on top of the canvas it
// should span.
//
// Literal colors, not Theme.* — same house convention as DropdownPanel.qml
// at this nesting depth.
Item {
    id: root

    // Array of { axis: "x" | "y", pos: real } in this item's own coordinate
    // space (i.e. the canvas's — pos is an X offset for axis "x", a Y
    // offset for axis "y").
    property var guides: []

    anchors.fill: parent
    z: 1000

    Repeater {
        model: root.guides
        delegate: Rectangle {
            required property var modelData

            color: "#ff5c9c"
            x: modelData.axis === "x" ? modelData.pos - 0.5 : 0
            y: modelData.axis === "y" ? modelData.pos - 0.5 : 0
            width: modelData.axis === "x" ? 1 : root.width
            height: modelData.axis === "y" ? 1 : root.height
        }
    }
}
