import QtQuick
import VGRPresenterUI

// The Edit screen's MEDIA tab — the selected picture / video / audio item's own
// values, in the place the TEXT tab puts a selected text item's. Modelled on
// FreeShow's Media tool (InspirationOrResources/FreeShow-main/src/frontend/
// components/edit/values/media.ts + MediaTools.svelte), which is the reference
// for which values a placed file has and in what order:
//
//   Media          the file itself, with Change (swap it) and ✕ (take it off)
//   Fit            Contain / Cover / Fill / Blur fill
//   Blend mode     Normal / Screen / Multiply / Overlay / Soft Light / …
//   Flip           horizontally / vertically
//   Cropping       top / right / bottom / left, a percentage of the picture
//   Filters        hue rotate, invert, blur, grayscale, sepia, brightness,
//                  contrast, saturate
//
// WHAT OWNS THE VALUES. Fit is CanvasItemStyle.mediaFit (the field the canvas and
// the ITEMS tab's Filters card already speak). Everything else is the item's own
// `meta.media` bag, which is what makes it survive a save: the engine persists a
// block's meta JSON verbatim, while its ContentStyle has no room for these.
//
//   meta.media = {
//       blend:  "normal" | "screen" | …,           // FreeShow's own list
//       flipH: bool, flipV: bool,
//       crop:   { top, right, bottom, left },       // percent, 0–90
//       filter: { hueRotate, invert, blur, grayscale, sepia, brightness,
//                 contrast, saturate }              // the engine's units
//   }
//
// Fit, the flips, cropping and the filter set are ALL honoured on both surfaces:
// the canvas through the picture it draws, and the engine through SceneBuilder's
// media branch (crop/flip/fit/filter on the decoded pixels), so the preview and
// the HDMI/NDI frame are one picture.
//
// BLEND MODE is composited by the ENGINE: SceneBuilder puts the item's mode on
// the image object it builds, and the rasterizer blends that picture against the
// pixels already in the frame (the full CSS set — screen, multiply, overlay,
// soft light, the dodge/burn pair, the four non-separable modes…). The QML
// canvas keeps drawing the file plainly — a QQuickItem has no access to what is
// beneath it without a compatibility Blend effect this Qt build does not ship —
// so the canvas shows the picture unblended while the HDMI/NDI frame shows it
// composited. The value rides the item so it saves with the show and every
// output reads the same number.
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
    // A WINDOW (a bus item) REUSES THIS TAB (EditScreen::primaryIsMedia): what a
    // window shows is a picture in a box, so the same values describe it — Fit,
    // Blend, Flip, Cropping and Filters all apply to the bus's feed on the canvas
    // AND on the outputs. It has no FILE of its own, though: its source is the
    // bus it names, so the Media row shows that bus (the host passes its current
    // name in `busName`) and Change opens the BUS picker instead of a file picker.
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
    // A picture (or a video's poster) is the only thing these draw settings can
    // describe: an AUDIO item has no frame to fit, flip, crop or filter, so those
    // sections are not drawn for it at all — the tab keeps the file row (which is
    // the one thing an audio item has) and nothing else invented.
    readonly property bool isPicture: root.kind === "image" || root.kind === "video"
                                      || root.isWindow
    readonly property string mediaFit: root.style && root.style.mediaFit ? String(root.style.mediaFit) : "contain"
    readonly property var mediaMeta: root.meta && root.meta.media ? root.meta.media : ({})
    readonly property bool flipH: root.mediaMeta.flipH === true
    readonly property bool flipV: root.mediaMeta.flipV === true
    readonly property var crop: root.mediaMeta.crop ? root.mediaMeta.crop : ({ top: 0, right: 0, bottom: 0, left: 0 })
    readonly property var filter: root.mediaMeta.filter ? root.mediaMeta.filter : ({})

    // Named hasCropChanges/hasFilterChanges, not cropChanged/filterChanged:
    // this file also declares SIGNALS named cropChanged/filterChanged below
    // (a value the user just edited, for the host to apply) — a property and
    // a signal cannot share one name (build: "Duplicate signal name: invalid
    // override of property change signal or superclass signal", since every
    // property also owns an implicit change-notify slot QML has to generate).
    readonly property bool hasCropChanges: (root.crop.top ?? 0) !== 0 || (root.crop.right ?? 0) !== 0
        || (root.crop.bottom ?? 0) !== 0 || (root.crop.left ?? 0) !== 0
    readonly property bool hasFilterChanges: (root.filter.hueRotate ?? 0) !== 0
        || (root.filter.invert ?? 0) !== 0 || (root.filter.blur ?? 0) !== 0
        || (root.filter.grayscale ?? 0) !== 0 || (root.filter.sepia ?? 0) !== 0
        || (root.filter.brightness ?? 1) !== 1 || (root.filter.contrast ?? 1) !== 1
        || (root.filter.saturate ?? 1) !== 1 || (root.filter.opacity ?? 1) !== 1

    // FreeShow's own fit list for an image (its mediaFitOptions: the blur fill
    // is a real choice for a picture — the file fills the box, softened, and the
    // sharp copy sits on top of it). The host turns the picked label back into
    // the key the style holds (EditScreen's mediaFitMenu).
    readonly property string fitLabel: root.mediaFit === "cover" ? qsTr("Cover")
        : root.mediaFit === "fill" ? qsTr("Fill")
        : root.mediaFit === "blur" ? qsTr("Blur fill")
        : qsTr("Contain")
    // The Fit field itself, so the host can drop the app's one DropdownPanel
    // under it (the same hand-off TextItemPanel's weight/font fields make).
    readonly property alias fitFieldItem: fitField

    // FreeShow's own blend list (its boxes.ts mediaBlendOptions), in its order.
    // These keys are what SceneBuilder parses (rendering::ParseBlendMode) and
    // what the rasterizer composites, so the list and the compositor cannot
    // drift: a key here is a mode the engine can actually draw.
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

    // The eight filter rows, in FreeShow's order and with its ranges. `scale` is
    // display = stored * scale, the same single conversion SlidePanel's own rows
    // use, so a row can never disagree with the engine about units.
    readonly property var filterRows: [
        { key: "hueRotate", label: qsTr("Hue rotate (°)"), min: 0, max: 360, step: 5, scale: 1, decimals: 0 },
        { key: "invert", label: qsTr("Invert (%)"), min: 0, max: 100, step: 5, scale: 100, decimals: 0 },
        { key: "blur", label: qsTr("Blur (px)"), min: 0, max: 100, step: 1, scale: 1, decimals: 0 },
        { key: "grayscale", label: qsTr("Grayscale (%)"), min: 0, max: 100, step: 5, scale: 100, decimals: 0 },
        { key: "sepia", label: qsTr("Sepia (%)"), min: 0, max: 100, step: 5, scale: 100, decimals: 0 },
        { key: "brightness", label: qsTr("Brightness (%)"), min: 0, max: 200, step: 5, scale: 100, decimals: 0 },
        { key: "contrast", label: qsTr("Contrast (%)"), min: 0, max: 200, step: 5, scale: 100, decimals: 0 },
        { key: "saturate", label: qsTr("Saturate (%)"), min: 0, max: 200, step: 5, scale: 100, decimals: 0 },
        // FreeShow's filter list ends with opacity (its filters.ts filterSection) —
        // the file's own transparency, separate from the item's Transform opacity
        // in the ITEMS tab (that one fades the whole box, borders included).
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

                // The picture glyph — Change: a different file on this item, or
                // a different BUS for a window (one row, two answers, because a
                // window's "file" IS the bus it names).
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
    // The shared FieldBox + the app's one DropdownPanel (the host owns the menu:
    // see EditScreen's mediaFitMenu), the same pair every other value picker in
    // this inspector uses. A plain labelled row, not a section card: the name is the
    // label and the value sits under it; once the fit is not the default, a reset
    // arrow says so.
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
    // FreeShow puts this directly under Fit, and so it is here. The value is
    // persisted with the item (`meta.media.blend`) AND composited on air:
    // SceneBuilder sets it on the picture object and the rasterizer blends that
    // object against the frame underneath it.
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
                // The same field the Transform card's Opacity uses: typed value,
                // a track to drag AND the ^ / v steppers — a crop edge is exactly
                // the kind of value an operator sets by eye (FreeShow's own
                // cropping rows are a slider with a number, not a bare box).
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
