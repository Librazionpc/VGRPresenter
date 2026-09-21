import QtQuick
import VGRPresenterUI

// A DESIGN LIBRARY tab: a resizable sidebar (All / Unlabeled / your categories) and a grid of design
// cards. One component serves every library the app keeps - the dock's OVERLAYS tab and TEMPLATES tab
// are two instances with different `service` singletons:
//
//   Overlays:  OverlayLibraryService   (ships the "Visuals" category and its overlays)
//   Templates: TemplateLibraryService  (ships FreeShow's starter set: Song, Presentation, Scripture)
//
// The ENGINE owns everything shown here (bps::library::DesignLibrary): the categories, the designs, the
// rules for what may be renamed or removed. This file only lists them and asks the service to change them.
//
//   All / Unlabeled / a category ... the cards in it; the tab bar's search narrows them by name.
//   New category ................... the sidebar's foot button.
//   New <design> ................... the floating button; it files the new design in the selected category.
//   On a card ...................... open it in the Edit screen, file it in a category, rename,
//                                    duplicate, delete (see DesignCard).
//
// Every pane always shows All, Unlabeled and the CATEGORIES line — the sample layout.
Item {
    id: root

    // The singleton this tab browses (OverlayLibraryService or TemplateLibraryService).
    property var service: null
    // The noun in the UI's strings ("overlay", "template").
    property string noun: "overlay"

    // The dock tab bar's search: only designs whose name matches (the engine's search).
    property string filter: ""
    readonly property string query: filter.trim()

    // ---- what is selected ----------------------------------------------------
    property string selection: "all"   // "all" | "unlabeled" | a category's id
    readonly property bool categorySelected: selection !== "all" && selection !== "unlabeled"
    readonly property string listFilter: selection === "all" ? ""
                                       : (selection === "unlabeled" ? service.unlabeledFilter : selection)


    property var gridItems: []
    function reload() { gridItems = service.designs(listFilter, query) }
    onSelectionChanged: reload()
    onQueryChanged: reload()
    Component.onCompleted: reload()

    function categoryExists(id) {
        const list = service.categories
        for (let i = 0; i < list.length; ++i)
            if (list[i].id === id) return true
        return false
    }
    function categoryName(id) {
        const list = service.categories
        for (let i = 0; i < list.length; ++i)
            if (list[i].id === id) return list[i].name
        return ""
    }
    Connections {
        target: root.service
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
    // What the name dialog is for: "category" (new), "design" (new), "rename" (a design).
    property string nameFor: ""
    property string nameTarget: ""   // the design being renamed
    function askName(what, title, initial, target) {
        nameFor = what
        nameTarget = target === undefined ? "" : target
        nameDialog.title = title
        nameDialog.placeholder = what === "category" ? qsTr("Category name") : qsTr("%1 name").arg(root.noun)
        nameDialog.confirmLabel = what === "rename" ? qsTr("Rename") : qsTr("Create")
        nameDialog.allowEmpty = what === "design"   // an empty name gets "Overlay", "Overlay 2", ...
        nameDialog.open(initial)
    }
    function nameAccepted(text) {
        if (nameFor === "category") {
            const id = service.createCategory(text)
            if (id !== "") root.selection = id
        } else if (nameFor === "design") {
            service.createDesign(text, root.categorySelected ? root.selection : "")
        } else if (nameFor === "rename") {
            service.renameDesign(nameTarget, text)
        }
    }

    property string deleteTarget: ""     // a design id, or a category id when deletingCategory
    property bool deletingCategory: false
    property string deleteName: ""

    // The "file it in..." menu: opened by a card's folder button, positioned under it.
    property string menuDesign: ""
    property string menuCurrent: ""
    function openCategoryMenu(design, anchorItem) {
        const p = anchorItem.mapToItem(root, 0, anchorItem.height + 4)
        cardMenu.visible = false
        menuDesign = design.id
        menuCurrent = design.category
        categoryMenu.x = Math.max(4, Math.min(root.width - categoryMenu.width - 4, p.x + anchorItem.width - categoryMenu.width))
        categoryMenu.y = Math.max(4, Math.min(root.height - categoryMenu.height - 4, p.y))
    }

    // What opening a design in the Edit screen means here (the host - the main screen - overrides it).
    signal designOpenRequested(string id)

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
                objectName: "selfTestDesignRow_all"
                width: parent.width
                icon: "layoutDashboard"
                label: qsTr("All")
                count: String(service.totalCount)
                selected: root.selection === "all"
                onClicked: root.selection = "all"
            }
            SidebarRow {
                objectName: "selfTestDesignRow_unlabeled"
                width: parent.width
                icon: "layers"
                label: qsTr("Unlabeled")
                count: String(service.unlabeledCount)
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
                    model: service.categories
                    delegate: SidebarRow {
                        required property var modelData
                        objectName: "selfTestDesignRow_" + modelData.name
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
                                service.deleteCategory(modelData.id)
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
            objectName: "selfTestDesignNewCategory"
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

            delegate: DesignCard {
                required property var modelData
                objectName: "selfTestDesignCard_" + modelData.name
                width: grid.cellWidth
                height: grid.cellHeight
                design: modelData
                now: ticker.now
                onEditRequested: root.designOpenRequested(modelData.id)
                onCategoryRequested: (anchorItem) => root.openCategoryMenu(modelData, anchorItem)
                onRenameRequested: root.askName("rename", qsTr("Rename %1").arg(root.noun), modelData.name, modelData.id)
                onDuplicateRequested: service.duplicateDesign(modelData.id)
                onRenameCommitted: (name) => service.renameDesign(modelData.id, name)
                onContextMenuRequested: (source, mx, my) => root.openCardMenu(modelData, source, mx, my)
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
                         ? qsTr("No %1s yet. Make one with the button below.").arg(root.noun)
                         : (root.selection === "unlabeled"
                            ? qsTr("Every %1 is in a category.").arg(root.noun)
                            : qsTr("Nothing in “%1” yet. The button below adds one here.").arg(root.categoryName(root.selection))))
                color: Theme.textMuted
                font.family: Theme.fontFamily; font.pixelSize: 12
            }
        }

        // (Restore what ships lives in Settings · General · Libraries.)

        // The floating "New <design>" button.
        Rectangle {
            id: newDesign
            objectName: "selfTestDesignNew"
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
                    text: qsTr("New %1").arg(root.noun)
                    color: "#ffffff"
                    font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.DemiBold
                }
            }
            PositionHoverArea {
                id: newHover
                anchors.fill: parent
                onClicked: root.askName("design", qsTr("New %1").arg(root.noun), "")
            }
        }
    }

    // ---- "File it in..." menu ---------------------------------------------------------
    MouseArea {
        // Click anywhere else to close it.
        anchors.fill: parent
        visible: root.menuDesign !== ""
        z: 20
        onClicked: root.menuDesign = ""
    }
    Rectangle {
        id: categoryMenu
        objectName: "selfTestDesignCategoryMenu"
        visible: root.menuDesign !== ""
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
                model: [{ id: "", name: qsTr("Unlabeled"), icon: "layers" }].concat(service.categories)
                delegate: SidebarRow {
                    required property var modelData
                    objectName: "selfTestDesignMenu_" + modelData.name
                    width: menuColumn.width
                    icon: modelData.icon
                    label: modelData.name
                    selected: root.menuCurrent === modelData.id
                    onClicked: {
                        // Close first: the change rebuilds this menu's rows (this one included), and nothing
                        // after the call may reach into a row that is gone.
                        const designId = root.menuDesign
                        root.menuDesign = ""
                        service.setDesignCategory(designId, modelData.id)
                    }
                }
            }
        }
    }

    // ---- Right-click menu on a card: every action of the hover buttons ----------------------------------
    // Edit / Rename / Duplicate, then "Move to ..." for each category (a tick marks where it is now), then Delete. Built when it
    // opens, so it always lists the current categories.
    property var cardMenuDesign: null
    property var cardMenuActions: ({})     // menu label -> "edit" | "rename" | "duplicate" | "delete" | "file:<category id>"
    function openCardMenu(design, source, mx, my) {
        root.menuDesign = ""                 // the "file in" popup, if it is open, gives way
        const items = []
        const actions = {}
        const add = (label, action, extra) => {
            items.push(Object.assign({ label: label }, extra || {}))
            actions[label] = action
        }
        add(qsTr("Edit"), "edit")
        add(qsTr("Rename"), "rename")
        add(qsTr("Duplicate"), "duplicate")
        items.push({ divider: true })
        add(qsTr("Move to Unlabeled"), "file:", design.category === "" ? { trailing: "\u2713" } : {})
        const cats = service.categories
        for (let i = 0; i < cats.length; ++i)
            add(qsTr("Move to %1").arg(cats[i].name), "file:" + cats[i].id, design.category === cats[i].id ? { trailing: "\u2713" } : {})
        items.push({ divider: true })
        add(qsTr("Delete"), "delete", { danger: true })
        root.cardMenuDesign = design
        root.cardMenuActions = actions
        cardMenu.model = items
        cardMenu.openAt(source, mx, my, root)
        // The panel's height is only known once its rows have laid out; placing it again then keeps a tall menu inside the window.
        Qt.callLater(function () { if (cardMenu.visible) cardMenu.openAt(source, mx, my, root) })
    }

    // Any click or scroll outside the menu closes it. The menu floats at WINDOW level (DropdownPanel lifts itself there), so its
    // catcher has to cover the whole window too - a pane-sized one left the menu hanging over other screens.
    MenuCatcher { menu: cardMenu }
    // ...and it goes when this tab does.
    onVisibleChanged: if (!visible) cardMenu.visible = false
    DropdownPanel {
        id: cardMenu
        objectName: "selfTestDesignCardMenu"
        visible: false
        z: 25
        maxHeight: 320
        onItemActivated: (label) => {
            cardMenu.visible = false           // first: the change below rebuilds the grid this menu was opened from
            const action = root.cardMenuActions[label]
            const design = root.cardMenuDesign
            if (!action || !design)
                return
            if (action === "edit") {
                root.designOpenRequested(design.id)
            } else if (action === "rename") {
                root.askName("rename", qsTr("Rename %1").arg(root.noun), design.name, design.id)
            } else if (action === "duplicate") {
                service.duplicateDesign(design.id)
            } else if (action === "delete") {
                root.deleteTarget = design.id
                root.deleteName = design.name
                root.deletingCategory = false
                confirm.open()
            } else if (action.indexOf("file:") === 0) {
                service.setDesignCategory(design.id, action.substring(5))
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
        title: root.deletingCategory ? qsTr("Delete category?") : qsTr("Delete %1?").arg(root.noun)
        message: root.deletingCategory
                 ? qsTr("“%1” is removed. Its %2s are kept and become unlabeled.").arg(root.deleteName, root.noun)
                 : (qsTr("“%1” is deleted. This can't be undone.").arg(root.deleteName)
                    + qsTr(" A default %1 can be brought back in Settings · General.").arg(root.noun))
        confirmLabel: qsTr("Delete")
        onConfirmed: {
            confirm.close()
            if (root.deletingCategory) service.deleteCategory(root.deleteTarget)
            else service.deleteDesign(root.deleteTarget)
        }
        onDismissed: confirm.close()
    }
}
