import QtQuick
import VGRPresenterUI

// FreeShow's slide context menu (contextMenus.ts "slide" + ContextMenu.svelte): a big centred Edit on top, rows with a coloured icon and a
// chevron where a row opens a submenu (Change group, Slide actions, Specific outputs, Format, Transition), Disable, Duplicate with its
// shortcut, and a big red Delete at the bottom.
//
// It only draws and reports. `model` is a list of entries:
//   { divider: true }
//   { id, label, icon, color, big: true }                                     a big centred entry (Edit, Delete slide)
//   { id, label, icon, color, shortcut }                                      a row
//   { label, icon, color, children: [ { id, arg, label, checked, dot, disabled, divider } ] }   a row with a submenu
// and `chosen(id, arg)` says what was picked. Opened at a point of a source item; it lifts itself to the window so nothing clips it.
Item {
    id: root

    property var model: []
    signal chosen(string id, var arg)

    readonly property color cLighter: "#2f3542"
    readonly property color cDarkest: "#12121c"
    readonly property color cText: "#f0f0ff"
    readonly property string mono: "Consolas"

    property int openSub: -1          // the row whose submenu is showing
    property real subY: 0             // where that row is, in the panel

    function openAt(source, x, y) {
        root.openSub = -1
        const contentItem = source.Window.contentItem
        root.parent = contentItem
        root.anchors.fill = contentItem
        const p = source.mapToItem(contentItem, x, y)
        panel.x = Math.max(4, Math.min(p.x, contentItem.width - panel.width - 4))
        panel.y = Math.max(4, Math.min(p.y, contentItem.height - panel.height - 4))
        root.visible = true
    }
    function close() { root.visible = false; root.openSub = -1 }
    function pick(id, arg) { root.close(); root.chosen(id, arg) }

    visible: false
    z: 20000

    // a press anywhere else shuts the menu
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => { root.close(); mouse.accepted = true }
    }

    Rectangle {
        id: panel
        width: 250
        height: column.height + 12
        radius: 10; color: "#191923"; border.color: root.cLighter
        // (the panel and its submenu swallow their own clicks)
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onPressed: (mouse) => mouse.accepted = true }

        Column {
            id: column
            x: 0; y: 6; width: parent.width

            Repeater {
                model: root.model
                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool isDivider: modelData.divider === true
                    readonly property bool isBig: modelData.big === true
                    readonly property bool hasSub: modelData.children !== undefined
                    width: column.width
                    height: isDivider ? 9 : (isBig ? 70 : 36)

                    Rectangle { visible: row.isDivider; x: 0; y: 4; width: parent.width; height: 1; color: root.cLighter }

                    Rectangle {
                        visible: !row.isDivider
                        anchors.fill: parent; anchors.leftMargin: 4; anchors.rightMargin: 4; radius: 6
                        color: rowHover.hovered || root.openSub === row.index ? "#14ffffff" : "transparent"
                    }

                    // a big entry: the icon over the label, both centred
                    Column {
                        visible: row.isBig
                        anchors.centerIn: parent
                        spacing: 8
                        IconGlyph { anchors.horizontalCenter: parent.horizontalCenter; name: row.modelData.icon ?? ""; color: row.modelData.color ?? root.cText; fit: true; width: 24; height: 24 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: row.modelData.label ?? ""; color: root.cText; font.family: root.mono; font.pixelSize: 15; font.weight: Font.Bold }
                    }

                    // a row: icon, label, then the shortcut or the submenu's chevron
                    Item {
                        visible: !row.isDivider && !row.isBig
                        anchors.fill: parent
                        IconGlyph { x: 18; anchors.verticalCenter: parent.verticalCenter; name: row.modelData.icon ?? ""; color: row.modelData.color ?? root.cText; fit: true; width: 17; height: 17 }
                        Text {
                            x: 52; anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 52 - 46
                            text: row.modelData.label ?? ""; elide: Text.ElideRight
                            color: row.modelData.disabled === true ? "#6b7280" : root.cText
                            font.family: root.mono; font.pixelSize: 17; font.weight: Font.Medium
                        }
                        Text {
                            visible: !row.hasSub && (row.modelData.shortcut ?? "") !== ""
                            anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter
                            text: row.modelData.shortcut ?? ""; opacity: 0.4; color: root.cText; font.family: root.mono; font.pixelSize: 14
                        }
                        IconGlyph { visible: row.hasSub; anchors.right: parent.right; anchors.rightMargin: 18; anchors.verticalCenter: parent.verticalCenter; name: "chevronRight"; color: root.cText; opacity: 0.5; width: 8; height: 8 }
                    }

                    HoverHandler {
                        id: rowHover
                        enabled: !row.isDivider
                        cursorShape: Qt.PointingHandCursor
                        onHoveredChanged: if (hovered && !row.isDivider) {
                            root.openSub = row.hasSub ? row.index : -1
                            root.subY = row.y
                        }
                    }
                    TapHandler {
                        enabled: !row.isDivider && row.modelData.disabled !== true
                        onTapped: {
                            if (row.hasSub) { root.openSub = row.index; root.subY = row.y }
                            else root.pick(row.modelData.id ?? "", undefined)
                        }
                    }
                }
            }
        }
    }

    // the submenu, beside the row that opened it (on the other side when there is no room)
    Rectangle {
        id: sub
        visible: root.openSub >= 0 && root.model[root.openSub] !== undefined && root.model[root.openSub].children !== undefined
        readonly property var entries: visible ? root.model[root.openSub].children : []
        width: 230
        height: subColumn.height + 12
        radius: 10; color: "#191923"; border.color: root.cLighter
        x: panel.x + panel.width + width + 8 < root.width ? panel.x + panel.width - 2 : panel.x - width + 2
        y: Math.max(4, Math.min(panel.y + 6 + root.subY, root.height - height - 4))
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onPressed: (mouse) => mouse.accepted = true }

        Column {
            id: subColumn
            y: 6; width: parent.width
            Repeater {
                model: sub.entries
                delegate: Item {
                    id: subRow
                    required property var modelData
                    width: subColumn.width
                    height: modelData.divider === true ? 9 : 34
                    Rectangle { visible: subRow.modelData.divider === true; y: 4; width: parent.width; height: 1; color: root.cLighter }
                    Rectangle { visible: subRow.modelData.divider !== true; anchors.fill: parent; anchors.leftMargin: 4; anchors.rightMargin: 4; radius: 6; color: subHover.hovered && subRow.modelData.disabled !== true ? "#14ffffff" : "transparent" }
                    // the group's colour, or a check for the chosen one
                    Rectangle { visible: (subRow.modelData.dot ?? "") !== ""; x: 18; anchors.verticalCenter: parent.verticalCenter; width: 12; height: 12; radius: 6; color: subRow.modelData.dot ?? "transparent" }
                    IconGlyph { visible: subRow.modelData.checked === true; x: 17; anchors.verticalCenter: parent.verticalCenter; name: "check"; color: "#93f190"; width: 10; height: 8 }
                    Text {
                        visible: subRow.modelData.divider !== true
                        x: 44; anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 44 - 12
                        text: subRow.modelData.label ?? ""; elide: Text.ElideRight
                        color: subRow.modelData.disabled === true ? "#6b7280" : root.cText
                        font.family: root.mono; font.pixelSize: 16
                    }
                    HoverHandler { id: subHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        enabled: subRow.modelData.divider !== true && subRow.modelData.disabled !== true
                        onTapped: root.pick(subRow.modelData.id ?? "", subRow.modelData.arg)
                    }
                }
            }
        }
    }
}
