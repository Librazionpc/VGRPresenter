import QtQuick

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