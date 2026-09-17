import QtQuick

// One canvas item's style: padding + border, as a plain value object.
//
// Style lives here as a real object: create one per canvas item and hand
// the same instance to the item, the inspector card, and anything else —
// every consumer binds to the object, so a slider move updates the canvas
// immediately with no mirroring layer.
//
// Intentionally NOT opacity: item transparency is out of scope (see
// SizeStyleCard.qml for the rationale). If a new style field is ever added,
// it gets one property here and automatically works everywhere — no new
// plumbing in any consumer.
QtObject {
    id: style

    // The content kind this item is, matching EditScreen.qml's Add Content
    // menu ("text" | "camera" | "media" | "audio" | "shape" | "timer" |
    // "clock"). Purely descriptive metadata for now (nothing branches on it
    // yet); it exists so a kind-specific default or control can be added
    // later without restructuring this object.
    property string kind: "text"

    // Empty space between the item's bounds and its content, in px.
    property real padding: 0

    // Fill behind the item's content. No separate on/off flag — "transparent"
    // (the default) *is* "no background", same one-state convention as the
    // slide's own background (see BackgroundColorModal's transparentValue).
    property color backgroundColor: "transparent"

    // Corner radius of the item's box — an INDEPENDENT control, deliberately
    // not part of the border section: it rounds the box itself (its fill,
    // outline, and selection chrome all follow), so it applies with the
    // border off. 0 = square corners, and 0 is the default — a fresh box is
    // square until the user rounds it.
    property real cornerRadius: 0

    // Border stroke drawn just inside the item's bounds.
    property bool borderEnabled: false
    property real borderWidth: 2
    // "line" | "dotted" | "dashed"
    property string borderStyle: "line"
    // Single hex string; gradients were only ever surfaced by the old
    // opacity/color-selection plumbing and are not supported here.
    property color borderColor: "#ffffff"
}
