import QtQuick
import VGRPresenterUI

// The Edit screen's left panel while it edits a DESIGN (an overlay or a template) instead of a show's slides:
// where a slide edit shows the slide list, a design edit shows what the design IS - its name, category, accent
// colour and flags - and, below, what the selected canvas item does in it.
//
// Everything written here goes to the ENGINE through the library service (OverlayLibraryService /
// TemplateLibraryService -> bps::library::DesignLibrary), which validates and saves; the panel keeps no copy.
//
//   Overlay:   name, category, colour, "lock to output" (stays when the slide changes), "place under slide", and how many
//              seconds it stays before leaving by itself (0 = until cleared) - FreeShow's overlay actions.
//   Template:  name, category, colour and which kind of show category it fits.
//   Selected item:
//     text in a template ...... which slide field it SHOWS (title, text, line 1/2, reference, notes) - the block's `bind`
//     vignette / corners ....... how far in the tint / how round the corners are (meta.inset)
Rectangle {
    id: root

    // OverlayLibraryService | TemplateLibraryService
    property var service: null
    property string designId: ""
    property string kind: "overlay"          // "overlay" | "template"
    // The canvas item selected in the editor (a CanvasItem) or null.
    property var item: null
    // EditScreen's undo/history snapshot function and its keyboard-focus hand-back (see TextItemPanel).
    property var undoHook: null
    property var focusHook: null

    signal doneRequested()

    color: "#12131a"

    // The design, re-read whenever the library changes (its own edits included).
    property int rev: 0
    Connections {
        target: root.service
        function onChanged() { root.rev++ }
    }
    readonly property var design: {
        const _ = root.rev
        return root.service && root.designId !== "" ? root.service.design(root.designId) : ({})
    }
    readonly property bool isTemplate: kind === "template"

    function notifyUndo() {
        if (root.focusHook)
            root.focusHook()
        if (root.undoHook)
            root.undoHook()
    }
    // Reassigns the whole meta object (a plain var on a QtObject only notifies on assignment).
    function setItemMeta(key, value) {
        if (!root.item)
            return
        const m = Object.assign({}, root.item.meta)
        m[key] = value
        root.item.meta = m
    }

    readonly property var swatches: ["", "#0b57a2", "#2957ff", "#00b894", "#f9a825", "#e5484d", "#a855f7", "#747680", "#dddddd"]
    readonly property var bindOptions: [
        { label: qsTr("Nothing (fixed text)"), value: "" },
        { label: qsTr("Title"), value: "title" },
        { label: qsTr("Slide text"), value: "text" },
        { label: qsTr("Line 1"), value: "line1" },
        { label: qsTr("Line 2"), value: "line2" },
        { label: qsTr("Reference"), value: "ref" },
        { label: qsTr("Notes"), value: "notes" }
    ]
    readonly property var contentTypeOptions: [
        { label: qsTr("Any show category"), value: "" },
        { label: qsTr("Song"), value: "song" },
        { label: qsTr("Notes"), value: "notes" },
        { label: qsTr("Scripture"), value: "scripture" }
    ]
    function labelOf(options, value) {
        for (let i = 0; i < options.length; ++i)
            if (options[i].value === value) return options[i].label
        return options.length > 0 ? options[0].label : ""
    }
    // "Unlabeled" plus the library's categories, as SelectField options.
    readonly property var categoryOptions: {
        const _ = root.rev
        const list = [{ label: qsTr("Unlabeled"), value: "" }]
        const cats = root.service ? root.service.categories : []
        for (let i = 0; i < cats.length; ++i)
            list.push({ label: cats[i].name, value: cats[i].id })
        return list
    }

    // A text field stops following its binding once the user has typed in it, so a different design opening in the
    // same panel has to put its own values back.
    onDesignIdChanged: Qt.callLater(root.resetFields)
    function resetFields() {
        nameField.text = root.design.name !== undefined ? root.design.name : ""
        durationField.text = root.design.displayDuration ? String(root.design.displayDuration) : ""
    }

    // Swallows the pointer so the canvas behind the panel never sees it.
    MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton; cursorShape: Qt.ArrowCursor }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 12
        contentWidth: width
        contentHeight: column.height + 12
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: column
            width: flick.width
            spacing: 14

            // ---- back ----
            Rectangle {
                width: parent.width; height: 34
                radius: 8
                color: backHover.hovered ? "#20242f" : "#1a1c26"
                border.width: 1; border.color: "#2a3140"
                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Text { text: "‹"; color: "#eef1f8"; font.pixelSize: 16; anchors.verticalCenter: parent.verticalCenter }
                    Text {
                        text: root.isTemplate ? qsTr("Done - back to templates") : qsTr("Done - back to overlays")
                        color: "#eef1f8"; font.family: "Inter"; font.pixelSize: 12
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                PositionHoverArea {
                    id: backHover
                    objectName: "selfTestDesignDone"
                    anchors.fill: parent
                    onClicked: root.doneRequested()
                }
            }

            Text {
                text: root.isTemplate ? qsTr("EDITING TEMPLATE") : qsTr("EDITING OVERLAY")
                color: "#5c6475"; font.family: "Inter"; font.pixelSize: 10; font.weight: Font.Bold
            }

            // ---- name ----
            SettingsField {
                id: nameField
                width: parent.width
                label: qsTr("Name")
                // Follows the engine unless the user is typing.
                text: root.design.name !== undefined ? root.design.name : ""
                onTextEdited: nameTimer.restart()
                onAccepted: nameTimer.triggered()
            }
            Timer {
                id: nameTimer
                interval: 700
                // An empty name is not a name: leave the field to be finished (the engine would refuse it).
                onTriggered: {
                    const t = nameField.text.trim()
                    if (t !== "" && t !== root.design.name)
                        root.service.renameDesign(root.designId, t)
                }
            }

            // ---- category ----
            SelectField {
                width: parent.width
                label: qsTr("Category")
                value: root.labelOf(root.categoryOptions, root.design.category !== undefined ? root.design.category : "")
                options: root.categoryOptions
                onValuePicked: (v) => root.service.setDesignCategory(root.designId, v)
            }

            // ---- colour ----
            Column {
                width: parent.width
                spacing: 6
                Text { text: qsTr("Colour"); color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: Theme.textXs }
                Flow {
                    width: parent.width
                    spacing: 6
                    Repeater {
                        model: root.swatches
                        delegate: Rectangle {
                            required property string modelData
                            width: 24; height: 24; radius: 12
                            color: modelData === "" ? "transparent" : modelData
                            border.width: (root.design.color ?? "") === modelData ? 2 : 1
                            border.color: (root.design.color ?? "") === modelData ? "#ff4d3d" : "#3a4155"
                            // "none" is drawn as a slashed circle
                            Rectangle {
                                visible: modelData === ""
                                anchors.centerIn: parent
                                width: parent.width - 6; height: 1.5
                                rotation: -45
                                color: "#6b7280"
                            }
                            PositionHoverArea {
                                anchors.fill: parent
                                onClicked: root.service.setDesignColor(root.designId, modelData)
                            }
                        }
                    }
                }
            }

            // ---- overlay flags (FreeShow's overlay actions) ----
            Column {
                visible: !root.isTemplate
                width: parent.width
                spacing: 10

                Row {
                    width: parent.width
                    Text {
                        width: parent.width - 44
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Lock to output")
                        color: "#eef1f8"; font.family: "Inter"; font.pixelSize: 12
                    }
                    SettingsToggle {
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.design.locked === true
                        onToggled: root.service.setDesignLocked(root.designId, !(root.design.locked === true))
                    }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Stays on screen when the slide changes.")
                    color: "#5c6475"; font.family: "Inter"; font.pixelSize: 10
                }
                Row {
                    width: parent.width
                    Text {
                        width: parent.width - 44
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Place under slide")
                        color: "#eef1f8"; font.family: "Inter"; font.pixelSize: 12
                    }
                    SettingsToggle {
                        anchors.verticalCenter: parent.verticalCenter
                        checked: root.design.placeUnderSlide === true
                        onToggled: root.service.setDesignPlaceUnderSlide(root.designId, !(root.design.placeUnderSlide === true))
                    }
                }
                SettingsField {
                    id: durationField
                    width: parent.width
                    label: qsTr("Display duration")
                    suffix: "s"
                    placeholder: qsTr("0 = until cleared")
                    text: root.design.displayDuration ? String(root.design.displayDuration) : ""
                    onTextEdited: durationTimer.restart()
                    onAccepted: durationTimer.triggered()
                }
                Timer {
                    id: durationTimer
                    interval: 700
                    onTriggered: {
                        const v = durationField.text.trim() === "" ? 0 : Number(durationField.text)
                        if (!isNaN(v) && v >= 0 && v !== (root.design.displayDuration ?? 0))
                            root.service.setDesignDuration(root.designId, v)
                    }
                }
            }

            // ---- template: which kind of show category it fits ----
            SelectField {
                visible: root.isTemplate
                width: parent.width
                label: qsTr("Fits")
                value: root.labelOf(root.contentTypeOptions, root.design.contentType !== undefined ? root.design.contentType : "")
                options: root.contentTypeOptions
                onValuePicked: (v) => root.service.setDesignContentType(root.designId, v)
            }

            // ---- the selected canvas item ----
            Rectangle { width: parent.width; height: 1; color: "#232530"; visible: itemSection.visible }

            Column {
                id: itemSection
                readonly property bool isText: root.item !== null && root.item.kind === "text"
                readonly property bool isScreenWide: root.item !== null && (root.item.kind === "vignette" || root.item.kind === "corners")
                visible: (root.isTemplate && isText) || isScreenWide
                width: parent.width
                spacing: 12

                Text {
                    text: qsTr("SELECTED ITEM")
                    color: "#5c6475"; font.family: "Inter"; font.pixelSize: 10; font.weight: Font.Bold
                }

                // Templates: which slide field this text shows.
                SelectField {
                    visible: root.isTemplate && itemSection.isText
                    width: parent.width
                    label: qsTr("Shows")
                    value: root.labelOf(root.bindOptions, root.item ? root.item.bind : "")
                    options: root.bindOptions
                    onValuePicked: (v) => {
                        if (!root.item) return
                        root.notifyUndo()
                        root.item.bind = v
                    }
                }
                Text {
                    visible: root.isTemplate && itemSection.isText
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("A slide made with this template shows its own text here instead of the fixed text.")
                    color: "#5c6475"; font.family: "Inter"; font.pixelSize: 10
                }

                // Vignette / corners: how far in it reaches.
                LabeledSlider {
                    visible: itemSection.isScreenWide
                    width: parent.width
                    label: root.item && root.item.kind === "corners" ? qsTr("Corner radius") : qsTr("Edge depth")
                    minValue: 0
                    maxValue: 250
                    value: root.item ? (root.item.meta.inset ?? 40) : 0
                    onDragStarted: root.notifyUndo()
                    onMoved: (v) => root.setItemMeta("inset", Math.round(v))
                }
            }
        }
    }
}
