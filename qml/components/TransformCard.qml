import QtQuick

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

        NumberField {
            width: parent.width
            label: qsTr("Opacity (%)")
            value: root.opacityPercent
            step: 5
            decimals: 0
            minValue: 0
            maxValue: 100
            enabled: root.hasTargets
            showSlider: true
            onMoved: (v) => {
                root.notifyUndo()
                root.targets.forEach((t) => t.opacity = v / 100)
            }
        }
    }
}