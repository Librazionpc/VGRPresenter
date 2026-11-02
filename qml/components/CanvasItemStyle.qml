import QtQuick

QtObject {
    id: style

    property string kind: "text"

    // Empty space between the item's bounds and its content, in px.
    property real padding: 0

    property color backgroundColor: "transparent"

    property real cornerRadius: 0

    // Border stroke drawn just inside the item's bounds.
    property bool borderEnabled: false
    property real borderWidth: 2
    // "line" | "dotted" | "dashed"
    property string borderStyle: "line"
    // Single hex string; gradients were only ever surfaced by the old
    // opacity/color-selection plumbing and are not supported here.
    property color borderColor: "#ffffff"

    property real opacity: 1.0
    property string mediaFit: "contain"
}