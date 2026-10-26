import QtQuick

// The Edit screen's Filters card, on the same collapsing-card shape as
// SizeStyleCard (see InspectorSection.qml).
//
// OPACITY IS NOT HERE — it moved to TransformCard with the rest of the item's
// transform (FreeShow's own split: opacity is how the whole item is presented,
// a filter is what happens to its pixels). This card is for pixel-level
// controls only; Media Fit is the one it has today, and FreeShow's filter rows
// (hue rotate, invert, blur, grayscale, brightness, contrast, saturate) will
// join it here once the render path can draw them — see the Filters scope note
// in docs/KNOWN_ISSUES.md's counterpart in this file's sibling cards.
//
// Same reusable shape SizeStyleCard.qml established: point `targets` at the
// style object(s) of the currently selected canvas item(s), every control binds
// to targets[0] and writes to all of them, undoHook/focusHook wire into the same
// undo-notification convention.
Column {
    id: root

    property list<CanvasItemStyle> targets: []

    readonly property CanvasItemStyle primary: targets.length > 0 ? targets[0] : null
    readonly property bool hasTargets: primary !== null

    // True only for an image/video item — Media Fit describes how a placed
    // PICTURE fills its box, and means nothing for text, a shape, a clock...
    property bool mediaFitSupported: false

    property var undoHook: null
    property var focusHook: null
    // Fit is a value the ENGINE reads too (SceneBuilder honours it on the placed
    // file), and the engine's copy rides the item's meta — so a host that owns
    // both writes (the Edit screen's applyMediaFit) sets this hook and the card
    // reports the pick instead of writing the style itself. Without a hook the
    // card keeps its original behaviour: write every target's style.
    property var fitHook: null
    function applyFit(fit) {
        if (root.fitHook) {
            root.fitHook(fit)
            return
        }
        root.targets.forEach((t) => t.mediaFit = fit)
    }
    function notifyUndo() {
        if (root.focusHook)
            root.focusHook()
        if (root.undoHook)
            root.undoHook()
    }

    readonly property string mediaFit: primary ? primary.mediaFit : "contain"

    // FreeShow's sectionColors entry for filters ("ceffbe"), the same green it
    // gives its own filters/backdrop-filters sections.
    readonly property color filterHue: "#ceffbe"

    spacing: 8

    InspectorSection {
        width: parent.width
        title: qsTr("Filters")
        icon: "sliders"
        iconColor: root.filterHue
        hasChanges: root.hasTargets && root.mediaFitSupported && root.mediaFit !== "contain"
        onResetRequested: {
            root.notifyUndo()
            if (root.mediaFitSupported)
                root.applyFit("contain")
        }

        // Contain / Cover / Fill segmented selector — same tile shape as
        // SizeStyleCard's Line/Dotted/Dashed row, gated on mediaFitSupported
        // instead of fillSupported (an image/video's own box always HAS
        // fill-able content; the gate here is about whether "fit" means
        // anything for this kind at all).
        Column {
            width: parent.width
            spacing: 8
            enabled: root.mediaFitSupported
            opacity: root.mediaFitSupported ? 1 : 0.4
            Behavior on opacity { NumberAnimation { duration: 100 } }

            Text {
                text: qsTr("Media Fit")
                color: Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium
            }

            Row {
                width: parent.width
                spacing: 8

                Repeater {
                    model: [
                        { key: "contain", label: qsTr("Contain") },
                        { key: "cover", label: qsTr("Cover") },
                        { key: "fill", label: qsTr("Fill") }
                    ]
                    delegate: Rectangle {
                        id: fitBtn
                        required property var modelData
                        readonly property bool active: root.mediaFit === fitBtn.modelData.key

                        width: (parent.width - 16) / 3
                        height: 32
                        radius: 8
                        color: fitBtn.active ? Theme.accent : (fitArea.containsMouse ? Theme.hoverBg : Theme.inset)
                        border.color: fitBtn.active ? Theme.accent : Theme.borderSubtle
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                        Text {
                            anchors.centerIn: parent
                            text: fitBtn.modelData.label
                            color: fitBtn.active ? Theme.controlInk : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }

                        MouseArea {
                            id: fitArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.notifyUndo()
                                root.applyFit(fitBtn.modelData.key)
                            }
                        }
                    }
                }
            }
        }
    }
}
