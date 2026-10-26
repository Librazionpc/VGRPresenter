import QtQuick

// One canvas item's style: padding + border, as a plain value object.
//
// Style lives here as a real object: create one per canvas item and hand
// the same instance to the item, the inspector card, and anything else —
// every consumer binds to the object, so a slider move updates the canvas
// immediately with no mirroring layer.
//
// Opacity and mediaFit were added by a later user request (a "Filters"
// section: "opacity, media fit") — SizeStyleCard.qml's header still carries
// the ORIGINAL reasoning for why opacity was left out (it only ever made
// sense alongside slide backgrounds, and faded TEXT in a confusing way);
// this time it is scoped to its own card instead of folding into Size &
// Style, and Media Fit is only meaningful for an image/video item in the
// first place (see FiltersCard.qml's mediaFitSupported). If a new style
// field is ever added, it gets one property here and one row in whichever
// card fits it — but SlideCanvasStore.qml's snapshotOf/itemFromBlock and
// EditScreen.qml's applyItemColor each list every field by name (undo/.vgr
// round-tripping, color-change rebuilding), so a new field needs those three
// updated too or it silently resets on the next unrelated edit.
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

    // The item's own transparency (1 = fully opaque), applied to the WHOLE
    // item — content included — not a second "background alpha" on top of
    // backgroundColor's own alpha channel.
    property real opacity: 1.0
    // How a placed picture/video fills its box: "contain" (whole frame,
    // letterboxed — Image.PreserveAspectFit), "cover" (fills the box,
    // cropped — Image.PreserveAspectCrop), "fill" (stretched — Image.Stretch).
    // Meaningless for any other kind; the Filters card greys the row out then.
    property string mediaFit: "contain"
}
