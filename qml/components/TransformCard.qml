import QtQuick

// The Edit screen's Transform card — FreeShow's own grouping (its edit panel's
// "transform" section: Opacity, Rotation, Flip, Tilt), on the shared collapsing
// card (see InspectorSection.qml).
//
// Opacity lives HERE now, not in Filters: it is a transform of the item's whole
// output (content included), not a filter over its pixels — the same split
// FreeShow makes. FiltersCard therefore keeps only the pixel-level controls
// (Media Fit today, the filter rows once the engine can draw them).
//
// Rotation/Flip/Tilt are not built yet: `CanvasItemStyle` carries no such field
// and the engine's render objects ignore rotation at composite time, so a row
// here would move a number nothing draws. They belong in this card when that
// plumbing exists — one property on CanvasItemStyle, one row below Opacity.
//
// Same reusable contract as FiltersCard/SizeStyleCard: point `targets` at the
// style object(s) of the selected canvas item(s); every control binds to
// targets[0] and writes to all of them, and undoHook/focusHook plug into the
// shared undo convention.
Column {
    id: root

    property list<CanvasItemStyle> targets: []

    readonly property CanvasItemStyle primary: targets.length > 0 ? targets[0] : null
    readonly property bool hasTargets: primary !== null

    property var undoHook: null
    property var focusHook: null
    function notifyUndo() {
        if (root.focusHook)
            root.focusHook()
        if (root.undoHook)
            root.undoHook()
    }

    // 0-100 on screen; CanvasItemStyle.opacity stores 0-1 (the fraction every
    // drawing surface applies directly — no second unit conversion anywhere).
    readonly property real opacityPercent: primary ? Math.round(primary.opacity * 100) : 100

    // FreeShow's sectionColors entry for transform (#f079b2).
    readonly property color transformHue: "#f079b2"

    spacing: 8

    InspectorSection {
        width: parent.width
        title: qsTr("Transform")
        icon: "transition"
        iconColor: root.transformHue
        hasChanges: root.hasTargets && root.opacityPercent !== 100
        onResetRequested: {
            root.notifyUndo()
            root.targets.forEach((t) => t.opacity = 1)
        }

        // The SAME editable value field every other card uses (NumberField.qml):
        // an opacity is normally 100, or one of a few numbers an operator types
        // (40, 70), and the 0–100 range is short enough that dragging a track to
        // find a number was the slow way to set one. One commit — Enter,
        // focus-out, or a stepper press — is one undo step.
        NumberField {
            width: parent.width
            label: qsTr("Opacity (%)")
            value: root.opacityPercent
            step: 5
            decimals: 0
            minValue: 0
            maxValue: 100
            enabled: root.hasTargets
            // Opacity is the one NumberField value an operator genuinely
            // hunts for by eye (user call: "add the slider[] between the
            // number field and the up and down arrow[s]") — every other
            // card's fields keep the typed-or-nudged-only default.
            showSlider: true
            onMoved: (v) => {
                root.notifyUndo()
                root.targets.forEach((t) => t.opacity = v / 100)
            }
        }
    }
}
