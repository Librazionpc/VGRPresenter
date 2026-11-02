import QtQuick
import VGRPresenterUI

Column {
    id: root

    // The open slide's live items, bottom of the stack first (SlideCanvasStore's
    // own order) — see the header: this panel does not copy them.
    property var items: []
    // Every selected canvas item's key, so a row can show the same selection the
    // canvas does (accent bar + accent number).
    property var selectedKeys: []
    // The consumer's kind table (EditScreen's `contentTypes`: { kind, icon, label }),
    // so a row's icon and name match the Add Content menu it came from.
    property var kinds: []
    // The slide's current background, "transparent" or a hex string.
    property color backgroundColor: "transparent"

    property var filters: ({})
    // Bracket one slider gesture into a single undo step (SlideCanvasStore's
    // owner pushes the history snapshot; the store itself has no history).
    property var undoHook: null

    signal backgroundPickRequested()
    signal filterChanged(string key, real value)
    // The Filters card's Reset chip: put all seven back to their defaults.
    signal filtersResetRequested()
    // A row was clicked; `modifiers` carries Ctrl/Shift through so the list
    // selects exactly like a canvas click does.
    signal rowClicked(string key, int modifiers)
    // Reorder one item: +1 = forward (towards the viewer), -1 = backward.
    signal moveRequested(string key, int direction)

    spacing: 8

    readonly property var rows: {
        const list = root.items || []
        const keys = []
        for (let i = list.length - 1; i >= 0; --i)
            keys.push(list[i].key)
        return keys
    }

    function itemFor(key) {
        const list = root.items || []
        for (let i = 0; i < list.length; ++i)
            if (list[i].key === key)
                return list[i]
        return null
    }

    function isSelected(key) {
        return root.selectedKeys && root.selectedKeys.indexOf(key) >= 0
    }

    function kindEntry(kind) {
        const table = root.kinds || []
        for (let i = 0; i < table.length; ++i)
            if (table[i].kind === kind)
                return table[i]
        return {
            kind: kind,
            icon: kind === "image" || kind === "video" || kind === "audio" ? kind
                : kind === "bus" ? "activity" : "shape",
            label: kind.charAt(0).toUpperCase() + kind.slice(1)
        }
    }

    function previewFor(item) {
        if (!item)
            return ""
        const text = item.text ? String(item.text) : ""
        if (item.kind === "text" && text !== "")
            return text.replace(/\s+/g, " ").trim()
        const meta = item.meta || {}
        const src = meta.src ? String(meta.src) : ""
        if (src !== "") {
            const parts = src.split(/[\\/]/)
            return parts[parts.length - 1]
        }
        if (meta.busId !== undefined) {
            const bus = BusListModel.getBus(meta.busId)
            if (bus && bus.name)
                return bus.name
        }
        if (text !== "")
            return text.replace(/\s+/g, " ").trim()
        return ""
    }

    readonly property var filterRows: [
        { key: "hueRotate", label: qsTr("Hue rotate"), min: 0, max: 360, step: 5, scale: 1, suffix: "°" },
        { key: "invert", label: qsTr("Invert"), min: 0, max: 1, step: 0.05, scale: 100, suffix: "%" },
        { key: "blur", label: qsTr("Blur"), min: 0, max: 100, step: 5, scale: 1, suffix: " px" },
        { key: "grayscale", label: qsTr("Grayscale"), min: 0, max: 1, step: 0.05, scale: 100, suffix: "%" },
        { key: "brightness", label: qsTr("Brightness"), min: 0, max: 2, step: 0.05, scale: 100, suffix: "%" },
        { key: "contrast", label: qsTr("Contrast"), min: 0, max: 2, step: 0.05, scale: 100, suffix: "%" },
        { key: "saturate", label: qsTr("Saturate"), min: 0, max: 2, step: 0.05, scale: 100, suffix: "%" }
    ]

    function filterValue(key, fallback) {
        if (!root.filters)
            return fallback
        const v = root.filters[key]
        return v === undefined || v === null ? fallback : v
    }

    // True when any row differs from its default — the state that lights the
    // card's Reset chip.
    readonly property bool filtersChanged: {
        const f = root.filters || ({})
        return (f.hueRotate ?? 0) !== 0 || (f.invert ?? 0) !== 0 || (f.blur ?? 0) !== 0
            || (f.grayscale ?? 0) !== 0 || (f.brightness ?? 1) !== 1
            || (f.contrast ?? 1) !== 1 || (f.saturate ?? 1) !== 1
    }

    // ---- The slide's own settings ---------------------------------------
    // One headerless block, like FreeShow's Slide tab: these apply to the slide,
    // not to whatever is selected on it, so they never fold away.
    InspectorSection {
        width: parent.width
        alwaysOpen: true
        title: qsTr("Slide")

        Item {
            width: parent.width
            height: 26

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Background color")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: 13
            }

            Rectangle {
                anchors.right: changeChip.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 24
                radius: 5
                color: root.backgroundColor
                border.color: Theme.borderSubtle
                border.width: 1
            }

            Rectangle {
                id: changeChip
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 24
                width: 60
                radius: 12
                color: bgChangeArea.containsMouse ? Theme.hoverBg : Theme.inset
                border.color: Theme.borderSubtle
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Change")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: bgChangeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.backgroundPickRequested()
                }
            }
        }
    }

    // ---- Arrange items ---------------------------------------------------
    // FreeShow's "edit.arrange_items": the stack, top first, each row numbered,
    // named by kind, tagged with its first words, and moved one step at a time.
    InspectorSection {
        width: parent.width
        // Nothing to arrange is a state, not an empty card.
        visible: root.rows.length > 0
        title: qsTr("Arrange items")
        // FreeShow's own card header (its ItemsList.svelte: `<Icon id="rearrange" />`).
        icon: "rearrange"
        iconColor: "#a16adb"

        Column {
            width: parent.width
            spacing: 0

            Repeater {
                model: root.rows
                delegate: Rectangle {
                    id: arrangeRow
                    required property string modelData
                    required property int index
                    readonly property var item: root.itemFor(arrangeRow.modelData)
                    readonly property var entry: root.kindEntry(arrangeRow.item ? arrangeRow.item.kind : "")
                    readonly property bool selected: root.isSelected(arrangeRow.modelData)
                    readonly property bool hasDivider: arrangeRow.index < root.rows.length - 1
                    readonly property bool canForward: arrangeRow.index > 0
                    readonly property bool canBackward: arrangeRow.hasDivider

                    width: parent.width
                    // 34px rows with a 1px hairline between them, drawn by each
                    // row's own bottom edge (no separator after the last one).
                    height: 34 + (arrangeRow.hasDivider ? 1 : 0)
                    color: arrangeRow.selected ? Qt.alpha(Theme.accent, 0.14)
                           : (rowArea.hovered ? Theme.hoverBg : "transparent")
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    // The selection bar on the row's leading edge, and the hairline
                    // under it — FreeShow marks the active row the same way.
                    Rectangle {
                        x: 0
                        y: 0
                        width: 3
                        height: arrangeRow.height - (arrangeRow.hasDivider ? 1 : 0)
                        color: Theme.accent
                        visible: arrangeRow.selected
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: arrangeRow.hasDivider ? 1 : 0
                        color: Theme.borderSubtle
                    }

                    PositionHoverArea {
                        id: rowArea
                        anchors.fill: parent
                        onClicked: (mouse) => root.rowClicked(arrangeRow.modelData, mouse.modifiers)
                    }

                    Text {
                        x: 10
                        width: 18
                        anchors.verticalCenter: parent.verticalCenter
                        text: arrangeRow.index + 1
                        color: arrangeRow.selected ? Theme.accent : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }

                    IconGlyph {
                        id: kindIcon
                        x: 32
                        anchors.verticalCenter: parent.verticalCenter
                        name: arrangeRow.entry.icon
                        color: arrangeRow.selected ? Theme.accent : Theme.textSecondary
                        fit: true
                        width: 13
                        height: 13
                    }

                    Text {
                        id: kindLabel
                        anchors.left: kindIcon.right
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: arrangeRow.entry.label
                        color: arrangeRow.selected ? Theme.accent : Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: Font.Medium
                    }

                    Text {
                        anchors.left: kindLabel.right
                        anchors.leftMargin: 8
                        anchors.right: arrows.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.previewFor(arrangeRow.item)
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }

                    Row {
                        id: arrows
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2

                        component ArrowButton: Rectangle {
                            id: arrow
                            property bool up: false
                            property bool available: true
                            signal nudged()

                            width: 22
                            height: 22
                            radius: 5
                            opacity: arrow.available ? 1 : 0.3
                            color: arrowArea.containsMouse && arrow.available ? Theme.hoverBg : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                            IconGlyph {
                                anchors.centerIn: parent
                                name: arrow.up ? "moveUp" : "moveDown"
                                color: arrow.available ? Theme.textSecondary : Theme.textMuted
                                width: 13
                                height: 13
                            }

                            MouseArea {
                                id: arrowArea
                                anchors.fill: parent
                                hoverEnabled: true
                                enabled: arrow.available
                                cursorShape: Qt.PointingHandCursor
                                onClicked: arrow.nudged()
                            }
                        }

                        ArrowButton {
                            up: false
                            available: arrangeRow.canBackward
                            onNudged: root.moveRequested(arrangeRow.modelData, -1)
                        }
                        ArrowButton {
                            up: true
                            available: arrangeRow.canForward
                            onNudged: root.moveRequested(arrangeRow.modelData, 1)
                        }
                    }
                }
            }
        }
    }

    // ---- Filters ---------------------------------------------------------
    InspectorSection {
        width: parent.width
        title: qsTr("Filters")
        icon: "sliders"
        // FreeShow's sectionColors entry for filters.
        iconColor: "#ceffbe"
        hasChanges: root.filtersChanged
        onResetRequested: root.filtersResetRequested()

        Text {
            width: parent.width
            visible: GraphicsInfo.api === GraphicsInfo.Software
            text: qsTr("This machine has no GPU preview path — the canvas shows the slide unfiltered. The output is still filtered.")
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
                label: filterRow.modelData.label + " (" + String(filterRow.modelData.suffix).trim() + ")"
                minValue: filterRow.modelData.min * filterRow.modelData.scale
                maxValue: filterRow.modelData.max * filterRow.modelData.scale
                step: filterRow.modelData.step * filterRow.modelData.scale
                decimals: 0
                value: root.filterValue(filterRow.modelData.key, filterRow.modelData.key === "brightness"
                                        || filterRow.modelData.key === "contrast"
                                        || filterRow.modelData.key === "saturate" ? 1 : 0)
                       * filterRow.modelData.scale
                onMoved: (v) => {
                    if (root.undoHook)
                        root.undoHook()
                    // Reports the ENGINE value (display / scale) — one
                    // conversion, done where the scale is known.
                    root.filterChanged(filterRow.modelData.key, v / filterRow.modelData.scale)
                }
            }
        }
    }
}