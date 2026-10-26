import QtQuick
import VGRPresenterUI

// The right panel's "SLIDE" tab — the slide's own settings, in FreeShow's edit
// panel language (its Slide tab: a background row group, then "Arrange items",
// see InspirationOrResources/FreeShow-main/src/frontend/components/edit/tools/
// ItemsList.svelte).
//
// TWO REAL THINGS, no placeholders:
//
//   * the slide's background colour, opening the same shared picker the ITEMS
//     tab's own Background row uses (`backgroundPickRequested`);
//   * the item STACK, topmost first, exactly as FreeShow lists it, with the two
//     reorder arrows — and it is the live canvas stack, not a copy: rows come
//     from `items` (SlideCanvasStore's working set for the open slide), so
//     adding, deleting, renaming or restyling an item shows up here with no
//     second list to keep in sync. Moving a row rewrites that same array, which
//     IS the canvas paint order (a Repeater draws its delegates in model order)
//     and the order the engine receives the slide's blocks in, so one arrow
//     reorders the canvas, the output and the saved show together.
//
// FreeShow's list shows the TOP of the stack first (its array is reversed), and
// numbers the rows in that order; so does this one's. `moveRequested(key, +1)`
// means "forward" (towards the viewer), `-1` "backward" — the consumer owns the
// array rewrite and the undo step, this panel only reports the intent.
//
// Background media and Slide template — the two other rows in FreeShow's tab —
// are deliberately absent: a slide here carries a colour only (the engine's
// Slide has `background`, no image field), and templates are assigned per
// CATEGORY, not per slide. Rows that cannot move real data would read as broken
// controls; they belong here the day those two fields exist.
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

    // The slide's filter set (SlideCanvasStore's current.filters): hueRotate,
    // invert, blur, grayscale, brightness, contrast, saturate. These are the
    // SLIDE's values — the on-air pass applies them to the composed frame for
    // this slide (see Effects.hpp), and the canvas previews them through
    // SlideFilterEffect — so a slide can be tinted, softened or turned to
    // greyscale without touching a single item's style.
    property var filters: ({})
    // Bracket one slider gesture into a single undo step (SlideCanvasStore's
    // owner pushes the history snapshot; the store itself has no history).
    property var undoHook: null

    signal backgroundPickRequested()
    // One filter value moved: `key` is the engine's own field name
    // (hueRotate / invert / blur / grayscale / brightness / contrast /
    // saturate) and `value` is in ITS units, not the row's display ones — the
    // row already divided its percent back out (see filterRows).
    signal filterChanged(string key, real value)
    // The Filters card's Reset chip: put all seven back to their defaults.
    signal filtersResetRequested()
    // A row was clicked; `modifiers` carries Ctrl/Shift through so the list
    // selects exactly like a canvas click does.
    signal rowClicked(string key, int modifiers)
    // Reorder one item: +1 = forward (towards the viewer), -1 = backward.
    signal moveRequested(string key, int direction)

    spacing: 8

    // Top-first keys — the stack as FreeShow shows it. Reads `root.items` so a
    // reassignment (add / delete / reorder, all of which replace the array)
    // rebuilds the list, while a property edit on one item (typed text, a new
    // picture) is picked up by the row's own bindings below.
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
        // Kinds the Add Content menu lists under a family (a picture is added
        // through "Media" but lands as kind "image") fall back to their own
        // name and a glyph that suits them.
        return {
            kind: kind,
            icon: kind === "image" || kind === "video" || kind === "audio" ? kind
                : kind === "bus" ? "activity" : "shape",
            label: kind.charAt(0).toUpperCase() + kind.slice(1)
        }
    }

    // The greyed identifier FreeShow puts after the kind name: the item's first
    // words, else its file name, else the bus it shows. Read from each row's own
    // item (not precomputed here) so typing into a text item updates its row live.
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

    // The seven filter rows, in FreeShow's own order and ranges (its
    // edit/values/filters.ts): hue in degrees with a 5° step, invert/blur and
    // grayscale/levels where the engine stores a fraction or a multiplier but
    // the row reads as a percentage or a pixel count. `scale` is the only
    // conversion there is — display = value * scale — so a row can never
    // disagree with the engine about which units the slide holds.
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
                    // FreeShow disables the arrow that would move the row past its
                    // end of the stack: forward is unavailable at the TOP (row 1),
                    // backward at the BOTTOM (last row).
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

                    // Row hover AND the whole-row click: declared before the
                    // arrows so the arrows' own MouseAreas sit above it and keep
                    // their click (later siblings take the press first).
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

                    // Backward (send behind) then forward (bring in front) —
                    // FreeShow's own order, so the pair means the same thing
                    // here as there.
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

                            // FreeShow's own pair (its ItemsList.svelte:
                            // icon="down" / icon="up" — the single-step arrows),
                            // not the chevrons this used to draw.
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
    // The slide's filter set — the same seven rows FreeShow's own Slide ·
    // Filters tool lists, applied to the whole slide (its background and every
    // item on it) rather than to one selected object. Values ride the slide, so
    // they travel with a duplicate, save into the show file, and reach the
    // on-air frame through the engine's own filter pass; the canvas reads the
    // same numbers through SlideFilterEffect, which is what makes the preview
    // and the output one picture.
    //
    // Rows are the shared NumberField (the same editable value field the Text,
    // Size & Style and Transform cards use): every one of these is a number an
    // operator reads and types — 25°, 40 px, 100% — and the ^ / v pair nudges it
    // by one step. A track under the label made the exact value harder to hit
    // than the number next to it already told you. The unit rides the LABEL
    // ("Blur (px)") instead of trailing the value, so the field itself stays a
    // plain number in every card.
    InspectorSection {
        width: parent.width
        title: qsTr("Filters")
        icon: "sliders"
        // FreeShow's sectionColors entry for filters.
        iconColor: "#ceffbe"
        hasChanges: root.filtersChanged
        onResetRequested: root.filtersResetRequested()

        // ONE line, only on a machine whose scene graph cannot run the canvas's
        // filter effect (GraphicsInfo.api === Software — no GPU drivers): the
        // canvas then shows the slide unfiltered, and saying so is the difference
        // between "the filters are broken" and "the PREVIEW cannot show them".
        // The output is filtered on every machine — the engine's filter pass is
        // CPU pixel work (rendering::ApplyFilters), not a shader.
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
                // The unit belongs with the name ("Hue rotate (°)"): the field
                // itself is the value, and appending " px" inside it would have
                // to be parsed back off again on every commit.
                label: filterRow.modelData.label + " (" + String(filterRow.modelData.suffix).trim() + ")"
                minValue: filterRow.modelData.min * filterRow.modelData.scale
                maxValue: filterRow.modelData.max * filterRow.modelData.scale
                step: filterRow.modelData.step * filterRow.modelData.scale
                decimals: 0
                // The row's readout is the engine value scaled for display —
                // the binding goes back through the store every time it moves,
                // so this is also what keeps a reset or an undo visible here.
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
