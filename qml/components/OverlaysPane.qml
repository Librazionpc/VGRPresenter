import QtQuick
import VGRPresenterUI

// The Overlays library tab: a resizable sidebar (All / Unlabeled / your categories) and a grid of
// overlay cards.
//
// The ENGINE owns everything shown here (OverlayLibraryService -> bps::overlays::OverlayLibrary): the
// categories (Visuals comes with the app), the overlays, the rules for what may be renamed or removed. This file
// only lists them and asks the service to change them.
//
//   All / Unlabeled / a category ... the cards in it; the tab bar's search narrows them by name.
//   New category ................... the sidebar's foot button.
//   New overlay .................... the floating button; it files the new overlay in the selected category.
//   On a card ...................... file it in a category, rename, duplicate, delete (see OverlayCard).
//
// Same building blocks as the Media tab: LibrarySidebar, SidebarRow, SidebarAddButton.
Item {
    id: root

    // The dock tab bar's Overlays search: only overlays whose name matches (the engine's search).
    property string filter: ""
    readonly property string query: filter.trim()

    // ---- what is selected ----------------------------------------------------
    property string selection: "all"   // "all" | "unlabeled" | a category's id
    readonly property bool categorySelected: selection !== "all" && selection !== "unlabeled"
    readonly property string listFilter: selection === "all" ? ""
                                       : (selection === "unlabeled" ? OverlayLibraryService.unlabeledFilter : selection)

    property var gridItems: []
    function reload() { gridItems = OverlayLibraryService.overlays(listFilter, query) }
    onSelectionChanged: reload()
    onQueryChanged: reload()
    Component.onCompleted: reload()

    function categoryExists(id) {
        const list = OverlayLibraryService.categories
        for (let i = 0; i < list.length; ++i)
            if (list[i].id === id) return true
        return false
    }
    function categoryName(id) {
        const list = OverlayLibraryService.categories
        for (let i = 0; i < list.length; ++i)
            if (list[i].id === id) return list[i].name
        return ""
    }
    Connections {
        target: OverlayLibraryService
        function onChanged() {
            // A category that was removed can't stay selected.
            if (root.categorySelected && !root.categoryExists(root.selection))
                root.selection = "all"
            root.reload()
        }
    }

    // One shared ticker for every clock on a card, only while the tab is showing.
    LiveClock { id: ticker; running: root.visible }

    // ---- the dialogs and menu the actions use -------------------------------------
    // What the name dialog is for: "category" (new), "overlay" (new), "rename" (an overlay).
    property string nameFor: ""
    property string nameTarget: ""   // the overlay being renamed
    function askName(what, title, initial, target) {
        nameFor = what
        nameTarget = target === undefined ? "" : target
        nameDialog.title = title
        nameDialog.placeholder = what === "category" ? qsTr("Category name") : qsTr("Overlay name")
        nameDialog.confirmLabel = what === "rename" ? qsTr("Rename") : qsTr("Create")
        nameDialog.allowEmpty = what === "overlay"   // an empty name gets "Overlay", "Overlay 2", ...
        nameDialog.open(initial)
    }
    function nameAccepted(text) {
        if (nameFor === "category") {
            const id = OverlayLibraryService.createCategory(text)
            if (id !== "") root.selection = id
        } else if (nameFor === "overlay") {
            OverlayLibraryService.createOverlay(text, root.categorySelected ? root.selection : "")
        } else if (nameFor === "rename") {
            OverlayLibraryService.renameOverlay(nameTarget, text)
        }
    }

    property string deleteTarget: ""     // an overlay id, or a category id when deletingCategory
    property bool deletingCategory: false
    property string deleteName: ""

    // The "file it in..." menu: opened by a card's folder button, positioned under it.
    property string menuOverlay: ""
    property string menuCurrent: ""
    function openCategoryMenu(overlay, anchorItem) {
        const p = anchorItem.mapToItem(root, 0, anchorItem.height + 4)
        menuOverlay = overlay.id
        menuCurrent = overlay.category
        categoryMenu.x = Math.max(4, Math.min(root.width - categoryMenu.width - 4, p.x + anchorItem.width - categoryMenu.width))
        categoryMenu.y = Math.max(4, Math.min(root.height - categoryMenu.height - 4, p.y))
    }

    // ---- Sidebar --------------------------------------------------------------
    LibrarySidebar {
        id: sidebar
        height: parent.height
        defaultWidth: 260

        Column {
            id: fixedRows
            x: 8; y: 8
            width: parent.width - 16
            spacing: 2

            SidebarRow {
                objectName: "selfTestOverlayRow_all"
                width: parent.width
                icon: "layoutDashboard"
                label: qsTr("All")
                count: String(OverlayLibraryService.totalCount)
                selected: root.selection === "all"
                onClicked: root.selection = "all"
            }
            SidebarRow {
                objectName: "selfTestOverlayRow_unlabeled"
                width: parent.width
                icon: "layers"
                label: qsTr("Unlabeled")
                count: String(OverlayLibraryService.unlabeledCount)
                selected: root.selection === "unlabeled"
                onClicked: root.selection = "unlabeled"
            }
        }

        Text {
            id: categoriesHeading
            x: 16
            y: fixedRows.y + fixedRows.height + 14
            text: qsTr("CATEGORIES")
            color: Theme.textMuted
            font.family: Theme.fontFamily; font.pixelSize: 10; font.bold: true
        }

        Flickable {
            id: categoryFlick
            x: 8
            y: categoriesHeading.y + categoriesHeading.height + 6
            width: parent.width - 16
            height: parent.height - y - 48
            clip: true
            contentWidth: width
            contentHeight: categoryColumn.height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: categoryColumn
                width: categoryFlick.width
                spacing: 2

                Repeater {
                    model: OverlayLibraryService.categories
                    delegate: SidebarRow {
                        required property var modelData
                        objectName: "selfTestOverlayRow_" + modelData.name
                        width: categoryColumn.width
                        icon: modelData.icon
                        label: modelData.name
                        count: String(modelData.count)
                        // The categories that came with the app stay; the user's can be removed.
                        removable: !modelData.isDefault
                        selected: root.selection === modelData.id
                        onClicked: root.selection = modelData.id
                        onRemoveRequested: {
                            if (modelData.count === 0) {
                                OverlayLibraryService.deleteCategory(modelData.id)
                            } else {
                                root.deleteTarget = modelData.id
                                root.deleteName = modelData.name
                                root.deletingCategory = true
                                confirm.open()
                            }
                        }
                    }
                }
            }
        }

        SidebarAddButton {
            objectName: "selfTestOverlayNewCategory"
            x: 8
            y: parent.height - 40
            width: parent.width - 16
            text: qsTr("New category")
            onClicked: root.askName("category", qsTr("New category"), "")
        }
    }

    // ---- The grid -------------------------------------------------------------------
    Item {
        id: area
        x: sidebar.width
        width: parent.width - x
        height: parent.height

        GridView {
            id: grid
            anchors.fill: parent
            anchors.margins: 4
            anchors.rightMargin: 10   // room for the scrollbar
            anchors.bottomMargin: 64  // and for the floating button
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.gridItems

            readonly property int columns: Math.max(2, Math.floor(width / 230))
            cellWidth: Math.floor(width / columns)
            // The card's 16:9 preview (its width less the 12 px of margins) plus the name bar.
            cellHeight: Math.round((cellWidth - 12) * 9 / 16) + 12 + 30

            delegate: OverlayCard {
                required property var modelData
                objectName: "selfTestOverlayCard_" + modelData.name
                width: grid.cellWidth
                height: grid.cellHeight
                overlay: modelData
                now: ticker.now
                onCategoryRequested: (anchorItem) => root.openCategoryMenu(modelData, anchorItem)
                onRenameRequested: root.askName("rename", qsTr("Rename overlay"), modelData.name, modelData.id)
                onDuplicateRequested: OverlayLibraryService.duplicateOverlay(modelData.id)
                onDeleteRequested: {
                    root.deleteTarget = modelData.id
                    root.deleteName = modelData.name
                    root.deletingCategory = false
                    confirm.open()
                }
            }
        }
        AppScrollBar {
            anchors.right: parent.right
            anchors.rightMargin: 3
            height: parent.height - 64
            flickable: grid
        }

        // Nothing to show.
        Column {
            visible: root.gridItems.length === 0
            anchors.horizontalCenter: parent.horizontalCenter
            y: (parent.height - 64 - height) / 2
            spacing: 12
            width: Math.min(parent.width - 40, 380)

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: root.query !== ""
                      ? qsTr("Nothing named “%1”.").arg(root.query)
                      : (root.selection === "all"
                         ? qsTr("No overlays yet. Make one with New overlay below.")
                         : (root.selection === "unlabeled"
                            ? qsTr("Every overlay is in a category.")
                            : qsTr("Nothing in “%1” yet. New overlay adds one here.").arg(root.categoryName(root.selection))))
                color: Theme.textMuted
                font.family: Theme.fontFamily; font.pixelSize: 12
            }
        }

        // Brings back default overlays that were deleted (a no-op toast when nothing is missing).
        Text {
            objectName: "selfTestOverlayRestore"
            x: 12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 22
            text: qsTr("Restore default overlays")
            color: restoreHover.hovered ? Theme.textPrimary : Theme.textMuted
            font.family: Theme.fontFamily; font.pixelSize: 11
            font.underline: restoreHover.hovered
            PositionHoverArea {
                id: restoreHover
                anchors.fill: parent
                onClicked: {
                    const n = OverlayLibraryService.restoreDefaults()
                    EventBus.notify(n > 0 ? qsTr("Brought back %n default overlay(s).", "", n)
                                          : qsTr("All the default overlays are already here."),
                                    "info", qsTr("Overlays"), "overlays.restore")
                }
            }
        }

        // The floating "New overlay" button.
        Rectangle {
            id: newOverlay
            objectName: "selfTestOverlayNew"
            anchors.right: parent.right
            anchors.rightMargin: 22
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            height: 36
            width: newRow.width + 32
            radius: 18
            color: newHover.hovered ? "#e5484d" : Theme.danger

            Row {
                id: newRow
                anchors.centerIn: parent
                spacing: 8
                IconGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "plus"
                    color: "#ffffff"
                    width: 12; height: 12
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("New overlay")
                    color: "#ffffff"
                    font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.DemiBold
                }
            }
            PositionHoverArea {
                id: newHover
                anchors.fill: parent
                onClicked: root.askName("overlay", qsTr("New overlay"), "")
            }
        }
    }

    // ---- "File it in..." menu ---------------------------------------------------------
    MouseArea {
        // Click anywhere else to close it.
        anchors.fill: parent
        visible: root.menuOverlay !== ""
        z: 20
        onClicked: root.menuOverlay = ""
    }
    Rectangle {
        id: categoryMenu
        objectName: "selfTestOverlayCategoryMenu"
        visible: root.menuOverlay !== ""
        z: 21
        width: 200
        height: menuColumn.height + 12
        radius: 8
        color: Theme.surface
        border.width: 1
        border.color: Theme.border

        Column {
            id: menuColumn
            x: 6; y: 6
            width: parent.width - 12
            spacing: 2

            Text {
                width: parent.width
                leftPadding: 8; topPadding: 4; bottomPadding: 4
                text: qsTr("FILE IN")
                color: Theme.textMuted
                font.family: Theme.fontFamily; font.pixelSize: 10; font.bold: true
            }
            Repeater {
                // "" is "Unlabeled".
                model: [{ id: "", name: qsTr("Unlabeled"), icon: "layers" }].concat(OverlayLibraryService.categories)
                delegate: SidebarRow {
                    required property var modelData
                    objectName: "selfTestOverlayMenu_" + modelData.name
                    width: menuColumn.width
                    icon: modelData.icon
                    label: modelData.name
                    selected: root.menuCurrent === modelData.id
                    onClicked: {
                        // Close first: the change rebuilds this menu's rows (this one included), and nothing
                        // after the call may reach into a row that is gone.
                        const overlayId = root.menuOverlay
                        root.menuOverlay = ""
                        OverlayLibraryService.setOverlayCategory(overlayId, modelData.id)
                    }
                }
            }
        }
    }

    // ---- dialogs ---------------------------------------------------------------------------
    NameDialog {
        id: nameDialog
        z: 30
        onAccepted: (text) => root.nameAccepted(text)
    }
    ConfirmDialog {
        id: confirm
        z: 30
        title: root.deletingCategory ? qsTr("Delete category?") : qsTr("Delete overlay?")
        message: root.deletingCategory
                 ? qsTr("“%1” is removed. Its overlays are kept and become unlabeled.").arg(root.deleteName)
                 : qsTr("“%1” is deleted. This can't be undone (a default overlay can be brought back with Restore default overlays).").arg(root.deleteName)
        confirmLabel: qsTr("Delete")
        onConfirmed: {
            confirm.close()
            if (root.deletingCategory) OverlayLibraryService.deleteCategory(root.deleteTarget)
            else OverlayLibraryService.deleteOverlay(root.deleteTarget)
        }
        onDismissed: confirm.close()
    }
}
