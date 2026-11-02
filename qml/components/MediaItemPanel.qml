import QtQuick
import VGRPresenterUI

Column {
    id: root

    // The selected canvas item (a picture / video / audio item) and its style.
    property var item: null
    property CanvasItemStyle style: item !== null && item.style !== undefined ? item.style : null

    property var undoHook: null
    property var focusHook: null
    function notifyUndo() {
        if (root.focusHook)
            root.focusHook()
        if (root.undoHook)
            root.undoHook()
    }

    readonly property string kind: root.item ? String(root.item.kind || "") : ""
    readonly property bool hasItem: root.item !== null && root.style !== null
    readonly property var meta: root.item && root.item.meta ? root.item.meta : ({})
    readonly property string filePath: root.meta && root.meta.src !== undefined && root.meta.src !== null
        ? String(root.meta.src) : ""
    readonly property string fileName: {
        const named = root.item && root.item.text ? String(root.item.text) : ""
        if (named !== "")
            return named
        const p = root.filePath
        if (p === "")
            return ""
        const slash = Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\"))
        return slash >= 0 ? p.slice(slash + 1) : p
    }
    property string busName: ""
    readonly property bool isWindow: root.kind === "bus"
    readonly property string kindLabel: root.kind === "video" ? qsTr("Video")
        : root.kind === "audio" ? qsTr("Audio")
        : root.isWindow ? qsTr("Window bus") : qsTr("Picture")
    readonly property string kindIcon: root.kind === "audio" ? "audio"
        : (root.kind === "video" || root.isWindow) ? "camera" : "image"
    // What the Media row names: the bus a window shows, the file otherwise.
    readonly property string displayName: root.isWindow
        ? (root.busName !== "" ? root.busName : root.fileName) : root.fileName
    // FreeShow's section colour for its media-adjacent sections (#97c7ff).
    readonly property color mediaHue: "#97c7ff"

    // ---- the values themselves -------------------------------------------
    readonly property bool isPicture: root.kind === "image" || root.kind === "video"
                                      || root.isWindow
    readonly property string mediaFit: root.style && root.style.mediaFit ? String(root.style.mediaFit) : "contain"
    readonly property var mediaMeta: root.meta && root.meta.media ? root.meta.media : ({})
    readonly property bool flipH: root.mediaMeta.flipH === true
    readonly property bool flipV: root.mediaMeta.flipV === true
    readonly property var crop: root.mediaMeta.crop ? root.mediaMeta.crop : ({ top: 0, right: 0, bottom: 0, left: 0 })
    readonly property var filter: root.mediaMeta.filter ? root.mediaMeta.filter : ({})

    readonly property bool hasCropChanges: (root.crop.top ?? 0) !== 0 || (root.crop.right ?? 0) !== 0
        || (root.crop.bottom ?? 0) !== 0 || (root.crop.left ?? 0) !== 0
    readonly property bool hasFilterChanges: (root.filter.hueRotate ?? 0) !== 0
        || (root.filter.invert ?? 0) !== 0 || (root.filter.blur ?? 0) !== 0
        || (root.filter.grayscale ?? 0) !== 0 || (root.filter.sepia ?? 0) !== 0
        || (root.filter.brightness ?? 1) !== 1 || (root.filter.contrast ?? 1) !== 1
        || (root.filter.saturate ?? 1) !== 1 || (root.filter.opacity ?? 1) !== 1

    readonly property string fitLabel: root.mediaFit === "cover" ? qsTr("Cover")
        : root.mediaFit === "fill" ? qsTr("Fill")
        : root.mediaFit === "blur" ? qsTr("Blur fill")
        : qsTr("Contain")
    // The Fit field itself, so the host can drop the app's one DropdownPanel
    // under it (the same hand-off TextItemPanel's weight/font fields make).
    readonly property alias fitFieldItem: fitField

    readonly property var blendOptions: [
        { key: "normal", label: qsTr("Normal") },
        { key: "screen", label: qsTr("Screen") },
        { key: "multiply", label: qsTr("Multiply") },
        { key: "overlay", label: qsTr("Overlay") },
        { key: "soft-light", label: qsTr("Soft Light") },
        { key: "hard-light", label: qsTr("Hard Light") },
        { key: "color-dodge", label: qsTr("Color Dodge") },
        { key: "color-burn", label: qsTr("Color Burn") },
        { key: "lighten", label: qsTr("Lighten") },
        { key: "darken", label: qsTr("Darken") },
        { key: "difference", label: qsTr("Difference") },
        { key: "exclusion", label: qsTr("Exclusion") },
        { key: "hue", label: qsTr("Hue") },
        { key: "saturation", label: qsTr("Saturation") },
        { key: "color", label: qsTr("Color") },
        { key: "luminosity", label: qsTr("Luminosity") }
    ]
    readonly property string blend: root.mediaMeta.blend ? String(root.mediaMeta.blend) : "normal"
    readonly property string blendLabel: {
        for (let i = 0; i < root.blendOptions.length; ++i) {
            if (root.blendOptions[i].key === root.blend)
                return root.blendOptions[i].label
        }
        return qsTr("Normal")
    }
    readonly property alias blendFieldItem: blendField

    // The four cropping sides, in FreeShow's order (top, right, bottom, left).
    readonly property var cropRows: [
        { key: "top", label: qsTr("Top") },
        { key: "right", label: qsTr("Right") },
        { key: "bottom", label: qsTr("Bottom") },
        { key: "left", label: qsTr("Left") }
    ]

    readonly property var filterRows: [
        { key: "hueRotate", label: qsTr("Hue rotate (°)"), min: 0, max: 360, step: 5, scale: 1, decimals: 0 },
        { key: "invert", label: qsTr("Invert (%)"), min: 0, max: 100, step: 5, scale: 100, decimals: 0 },
        { key: "blur", label: qsTr("Blur (px)"), min: 0, max: 100, step: 1, scale: 1, decimals: 0 },
        { key: "grayscale", label: qsTr("Grayscale (%)"), min: 0, max: 100, step: 5, scale: 100, decimals: 0 },
        { key: "sepia", label: qsTr("Sepia (%)"), min: 0, max: 100, step: 5, scale: 100, decimals: 0 },
        { key: "brightness", label: qsTr("Brightness (%)"), min: 0, max: 200, step: 5, scale: 100, decimals: 0 },
        { key: "contrast", label: qsTr("Contrast (%)"), min: 0, max: 200, step: 5, scale: 100, decimals: 0 },
        { key: "saturate", label: qsTr("Saturate (%)"), min: 0, max: 200, step: 5, scale: 100, decimals: 0 },
        { key: "opacity", label: qsTr("Opacity (%)"), min: 0, max: 100, step: 5, scale: 100, decimals: 0 }
    ]

    function filterStored(key, fallback) {
        const v = root.filter ? root.filter[key] : undefined
        return v === undefined || v === null ? fallback : v
    }
    function filterDefault(key) {
        return key === "brightness" || key === "contrast" || key === "saturate"
            || key === "opacity" ? 1 : 0
    }

    // ---- the host's answers ----------------------------------------------
    signal changeFileRequested()
    // A WINDOW's Change: pick a different bus for this window (the same picker
    // the window's own row used to open).
    signal changeBusRequested()
    // ✕: the file comes off the item (the box, its style and its place stay —
    // this is "not this picture", not "remove the item").
    signal clearFileRequested()
    signal fitPicked(string fit)
    signal fitFieldTapped()
    signal blendPicked(string blend)
    signal blendFieldTapped()
    signal flagChanged(string key, bool value)
    signal cropEdited(string side, real percent)
    signal resetCropRequested()
    signal filterEdited(string key, real value)
    signal resetFiltersRequested()

    spacing: 8

    // ---- Media: the file --------------------------------------------------
    InspectorSection {
        width: parent.width
        title: qsTr("Media")
        icon: root.kindIcon
        iconColor: root.mediaHue

        Item {
            width: parent.width
            height: 34

            Column {
                anchors.left: parent.left
                anchors.right: fileButtons.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    width: parent.width
                    text: root.displayName !== "" ? root.displayName
                        : (root.isWindow ? qsTr("No bus") : qsTr("No file"))
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                    elide: Text.ElideMiddle
                }
                Text {
                    width: parent.width
                    text: root.kindLabel
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                }
            }

            Row {
                id: fileButtons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // ✕ — take the file off the item (a window has no file to take
                // off: its box stays and it simply has no bus yet).
                Rectangle {
                    width: 24
                    height: 24
                    radius: 6
                    visible: !root.isWindow && root.filePath !== ""
                    color: clearArea.containsMouse ? Theme.hoverBg : "transparent"

                    IconGlyph {
                        anchors.centerIn: parent
                        name: "close"
                        color: clearArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                        width: 10
                        height: 10
                    }
                    MouseArea {
                        id: clearArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.clearFileRequested()
                    }
                }

                Rectangle {
                    width: 24
                    height: 24
                    radius: 6
                    color: changeArea.containsMouse ? Theme.hoverBg : "transparent"

                    IconGlyph {
                        anchors.centerIn: parent
                        name: root.kindIcon
                        color: changeArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                        width: 12
                        height: 12
                    }
                    MouseArea {
                        id: changeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.isWindow ? root.changeBusRequested()
                                                 : root.changeFileRequested()
                    }
                }
            }
        }
    }

    // ---- Fit -------------------------------------------------------------
    FieldBox {
        id: fitField
        width: parent.width
        visible: root.isPicture
        label: qsTr("Fit")
        value: root.fitLabel
        chevron: true
        modified: root.hasItem && root.mediaFit !== "contain"
        onTapped: root.fitFieldTapped()
        onResetRequested: {
            root.notifyUndo()
            root.fitPicked("contain")
        }
    }

    // ---- Blend mode ------------------------------------------------------
    FieldBox {
        id: blendField
        width: parent.width
        visible: root.isPicture
        label: qsTr("Blend mode")
        value: root.blendLabel
        chevron: true
        modified: root.hasItem && root.blend !== "normal"
        onTapped: root.blendFieldTapped()
        onResetRequested: {
            root.notifyUndo()
            root.blendPicked("normal")
        }
    }

    // ---- Flip ------------------------------------------------------------
    Column {
        width: parent.width
        visible: root.isPicture
        spacing: 6

        Repeater {
            model: [
                { key: "flipH", label: qsTr("Flip horizontally"), on: root.flipH },
                { key: "flipV", label: qsTr("Flip vertically"), on: root.flipV }
            ]
            delegate: Rectangle {
                id: flipRow
                required property var modelData
                width: parent.width
                height: 40
                radius: 9
                color: Theme.card
                border.width: 1
                border.color: Theme.borderSubtle

                Rectangle {
                    id: flipBox
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    radius: 4
                    color: flipRow.modelData.on ? Theme.accent : "transparent"
                    border.width: 1
                    border.color: flipRow.modelData.on ? Theme.accent : Theme.border
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    IconGlyph {
                        anchors.centerIn: parent
                        visible: flipRow.modelData.on
                        name: "check"
                        color: Theme.controlInk
                        width: 11
                        height: 11
                    }
                }

                Text {
                    anchors.left: flipBox.right
                    anchors.leftMargin: 10
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: flipRow.modelData.label
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.notifyUndo()
                        root.flagChanged(flipRow.modelData.key, !flipRow.modelData.on)
                    }
                }
            }
        }
    }

    // ---- Cropping --------------------------------------------------------
    InspectorSection {
        width: parent.width
        title: qsTr("Cropping")
        icon: "image"
        iconColor: root.mediaHue
        visible: root.isPicture
        expanded: false
        hasChanges: root.hasCropChanges
        onResetRequested: {
            root.notifyUndo()
            root.resetCropRequested()
        }

        Repeater {
            model: root.cropRows
            delegate: NumberField {
                id: cropRow
                required property var modelData

                width: parent.width
                label: qsTr("%1 (%)").arg(cropRow.modelData.label)
                value: root.crop[cropRow.modelData.key] ?? 0
                minValue: 0
                maxValue: 90
                step: 1
                decimals: 0
                showSlider: true
                onMoved: (v) => {
                    root.notifyUndo()
                    root.cropEdited(cropRow.modelData.key, v)
                }
            }
        }
    }

    // ---- Filters ---------------------------------------------------------
    InspectorSection {
        width: parent.width
        title: qsTr("Filters")
        icon: "sliders"
        iconColor: "#ceffbe"
        visible: root.isPicture
        hasChanges: root.hasFilterChanges
        onResetRequested: {
            root.notifyUndo()
            root.resetFiltersRequested()
        }

        Text {
            width: parent.width
            visible: GraphicsInfo.api === GraphicsInfo.Software
            text: qsTr("This machine has no GPU preview path — the canvas shows these unfiltered. The output is still filtered.")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        Repeater {
            model: root.filterRows
            delegate: NumberField {
                id: filterRow
                required property var modelData

                width: parent.width
                label: filterRow.modelData.label
                value: root.filterStored(filterRow.modelData.key, root.filterDefault(filterRow.modelData.key))
                       * filterRow.modelData.scale
                minValue: filterRow.modelData.min
                maxValue: filterRow.modelData.max
                step: filterRow.modelData.step
                decimals: filterRow.modelData.decimals
                onMoved: (v) => {
                    root.notifyUndo()
                    root.filterEdited(filterRow.modelData.key, v / filterRow.modelData.scale)
                }
            }
        }
    }
}