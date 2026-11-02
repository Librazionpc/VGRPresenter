import QtQuick
import VGRPresenterUI

Item {
    id: root

    property int gridRowBuilds: 0
    property int gridRowDrops: 0
    property int gridFitSolves: 0

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

    // ---- WHERE THE GRID WAS LEFT (user call: "make the app remember its state and
    property string stateKey: ""
    property bool placeRestored: false
    function placeIndex(id) {
        if (id === "") return -1
        for (let i = 0; i < gridRows.count; ++i)
            if (root.gridRowId(i) === String(id)) return i
        return -1
    }
    function savePlace() {
        if (root.stateKey === "" || !SettingsService.ready) return
        const at = gridRows.count > 0 ? grid.indexAt(8, grid.contentY + 8) : -1
        const top = at >= 0 ? root.gridRowId(at) : ""
        const text = JSON.stringify({ selection: root.selection, top: top })
        if (String(SettingsService.values[root.stateKey] ?? "") !== text)
            SettingsService.setValue(root.stateKey, text)
    }
    function touchPlace() {
        if (root.stateKey !== "") placeTimer.restart()
    }
    function restorePlace() {
        if (root.placeRestored || root.stateKey === "" || !SettingsService.ready) return
        // Read once: a pre-boot read sees nothing, and a later retry is what fixes it.
        root.placeRestored = true
        const raw = String(SettingsService.values[root.stateKey] ?? "")
        let state = null
        if (raw !== "") {
            try { state = JSON.parse(raw) } catch (e) { state = null }
        }
        if (state && typeof state === "object") {
            const wanted = typeof state.selection === "string" ? state.selection : "all"
            if (wanted !== root.selection
                    && (wanted === "all" || wanted === "unlabeled" || root.categoryExists(wanted)))
                root.selection = wanted        // its own handler reloads the grid
        }
        Qt.callLater(function () {
            const at = state ? root.placeIndex(state.top) : -1
            if (at >= 0) grid.positionViewAtIndex(at, GridView.Beginning)
            root.savePlace()
        })
    }
    Timer { id: placeTimer; interval: 350; onTriggered: root.savePlace() }

    property var gridItems: []
    // ---- the grid's MODEL: rows that are INSERTED, REMOVED and UPDATED, not reset --------
    ListModel { id: gridRows }
    function gridRowId(index) {
        const row = gridRows.get(index)
        return (row && row.design) ? String(row.design.id) : ""
    }
    property var gridRowSig: ({})
    function syncGridRows() {
        const want = gridItems
        const wanted = ({})
        const sigs = ({})
        for (let i = 0; i < want.length; ++i) {
            const id = String(want[i].id)
            wanted[id] = true
            sigs[id] = JSON.stringify(want[i])
        }
        for (let i = gridRows.count - 1; i >= 0; --i) {
            const id = root.gridRowId(i)
            if (wanted[id] !== true) {
                gridRows.remove(i)
                delete gridRowSig[id]
            }
        }
        for (let i = 0; i < want.length; ++i) {
            const row = want[i]
            const id = String(row.id)
            if (i >= gridRows.count) {
                gridRows.append({ design: row })          // new cards at the end
                gridRowSig[id] = sigs[id]
                continue
            }
            const here = root.gridRowId(i)
            if (here !== id) {
                let from = -1
                for (let j = i + 1; j < gridRows.count; ++j)
                    if (root.gridRowId(j) === id) { from = j; break }
                if (from >= 0)
                    gridRows.move(from, i, 1)             // it was below: bring it up
                else
                    gridRows.insert(i, { design: row })   // not in the model at all
                gridRowSig[id] = sigs[id]
                continue
            }
            if (gridRowSig[id] !== sigs[id]) {            // same card, different content
                gridRows.set(i, { design: row })
                gridRowSig[id] = sigs[id]
            }
        }
        checkGridRows()
    }
    function checkGridRows() {
        if (typeof SelfTestLayout === "undefined" || !SelfTestLayout)
            return
        let mismatch = ""
        if (gridRows.count !== gridItems.length) {
            mismatch = "count=" + gridRows.count + " want=" + gridItems.length
        } else {
            for (let i = 0; i < gridItems.length; ++i) {
                if (root.gridRowId(i) !== String(gridItems[i].id)) {
                    mismatch = "row=" + i + " id=" + root.gridRowId(i) + " want=" + gridItems[i].id
                    break
                }
            }
        }
        EngineBridge.log("info", "DesignRows",
                         root.noun + " rows=" + gridRows.count + " want=" + gridItems.length
                         + (mismatch === "" ? " match=1" : " MISMATCH " + mismatch))
    }
    function reload() {
        const filter = selection === "all" ? ""
                     : (selection === "unlabeled" ? service.unlabeledFilter : selection)
        gridItems = service.designs(filter, query)
        syncGridRows()
    }
    onSelectionChanged: { reload(); root.touchPlace() }
    onQueryChanged: reload()
    readonly property int serviceRevision: (service ? service.totalCount : 0)
                                         + (service ? service.unlabeledCount : 0)
                                         + (service ? service.categories.length : 0)
    onServiceRevisionChanged: { reload(); root.restorePlace() }
    Component.onCompleted: { reload(); root.restorePlace(); SettingsService.whenReady(root.restorePlace) }

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
            root.restorePlace()
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
        nameDialog.placeholder = (what === "category" || what === "renameCategory")
            ? qsTr("Category name") : qsTr("%1 name").arg(root.noun)
        nameDialog.confirmLabel = (what === "rename" || what === "renameCategory") ? qsTr("Rename") : qsTr("Create")
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
        } else if (nameFor === "renameCategory") {
            service.renameCategory(nameTarget, text)
        }
    }

    // ---- Category context menu (right-click a sidebar category): Rename
    // always, Delete only for a removable (non-default) one — the same
    // restriction its own remove button already honors.
    property var categoryMenuTarget: null
    function openCategoryContextMenu(category, removable, source, mx, my) {
        cardMenu.visible = false
        const items = [{ label: qsTr("Rename") }]
        if (removable) items.push({ label: qsTr("Delete"), danger: true })
        root.categoryMenuTarget = category
        categoryCtxMenu.model = items
        categoryCtxMenu.openAt(source, mx, my, root)
    }
    DropdownPanel {
        id: categoryCtxMenu
        objectName: "selfTestDesignCategoryMenu"
        visible: false
        z: 25
        // Same as cardMenu: hug the rows rather than render as a 480 px slab.
        fitContentWidth: true
        onItemActivated: (label) => {
            categoryCtxMenu.visible = false
            const cat = root.categoryMenuTarget
            if (!cat) return
            if (label === qsTr("Rename")) {
                root.askName("renameCategory", qsTr("Rename category"), cat.name, cat.id)
            } else if (label === qsTr("Delete")) {
                root.deleteTarget = cat.id
                root.deleteName = cat.name
                root.deletingCategory = true
                confirm.open()
            }
        }
    }
    MenuCatcher { menu: categoryCtxMenu }

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
    // A card was clicked / double-clicked (overlays only): the centre page shows it / the open project takes it.
    signal designActivated(string id, string name)
    signal designOpened(string id, string name)

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
            font.family: Theme.fontFamily; font.pixelSize: 12; font.bold: true
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
                        onContextMenuRequested: (source, mx, my) =>
                            root.openCategoryContextMenu(modelData, !modelData.isDefault, source, mx, my)
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
            objectName: "selfTestDesignGrid-" + root.noun
            anchors.fill: parent
            anchors.margins: 4
            anchors.rightMargin: 10
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true
            // ---- the filter cost, and what was done about it -----------------------
            model: gridRows

            // The card at the top of the grid is part of the remembered place.
            onMovementEnded: root.touchPlace()

            readonly property int columns: 4
            cellWidth: Math.floor(width / columns)
            // The card's 16:9 preview (its width less the 12 px of margins) plus the name bar.
            cellHeight: Math.round((cellWidth - 12) * 9 / 16) + 12 + 30

            delegate: DesignCard {
                required property var modelData
                // LAYOUT PROBE (VGR_LAYOUT_TEST).
                Component.onCompleted: root.gridRowBuilds++
                Component.onDestruction: root.gridRowDrops++
                objectName: "selfTestDesignCard_" + modelData.name
                width: grid.cellWidth
                height: grid.cellHeight
                design: modelData
                now: ticker.now
                probeSink: root   // (scroll-cost probe: the cards' fit solvers are counted here)
                dragKind: root.noun === "overlay" ? "overlay" : ""
                onActivated: root.designActivated(modelData.id, modelData.name)
                onOpened: root.designOpened(modelData.id, modelData.name)
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
            height: parent.height - 8   // tracks the grid, which now runs to the bottom
            flickable: grid
        }

        // Nothing to show.
        Column {
            visible: root.gridItems.length === 0
            anchors.horizontalCenter: parent.horizontalCenter
            y: (parent.height - height) / 2
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
                font.family: Theme.fontFamily; font.pixelSize: 14
            }
        }

        // (Restore what ships lives in Settings · General · Libraries.)

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
            color: newHover.hovered ? Qt.lighter(Theme.accent, 1.08) : Theme.accent

            // the soft shadow (drawn first, so it sits under the pill)
            Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 3; width: parent.width + 6; height: parent.height + 4; radius: 20; color: "#26000000" }
            Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 2; height: parent.height + 2; radius: 19; color: "#33000000" }

            Row {
                id: newRow
                anchors.centerIn: parent
                spacing: 8
                PlusGlyph {
                    anchors.verticalCenter: parent.verticalCenter
                    size: 12; thickness: 1.6; color: Theme.controlInk
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("New %1").arg(root.noun)
                    color: Theme.controlInk
                    font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold
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
                font.family: Theme.fontFamily; font.pixelSize: 12; font.bold: true
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
    // Edit / Rename / Duplicate, ONE "Move to" row (its category list lives in the flyout beside it —
    // see below), then Delete. Built when it opens, so the flyout always lists the current categories.
    property var cardMenuDesign: null
    property var cardMenuActions: ({})     // menu label -> "edit" | "rename" | "duplicate" | "delete" | "move"
    function openCardMenu(design, source, mx, my) {
        root.menuDesign = ""                 // the "file in" popup, if it is open, gives way
        moveMenu.closeFlyout()               // ...and so does a flyout from a previous menu
        categoryCtxMenu.visible = false
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
        add(root.moveLabel, "move", { trailing: "\u203a" })
        items.push({ divider: true })
        add(qsTr("Delete"), "delete", { danger: true })
        root.cardMenuDesign = design
        root.cardMenuActions = actions
        cardMenu.model = items
        cardMenu.openAt(source, mx, my, root.Window.contentItem)
    }

    // ---- the "Move to" flyout -------------------------------------------------------------
    readonly property string moveLabel: qsTr("Move to")
    property var moveRow: null            // the card menu's "Move to" row (the flyout's anchor)
    function openMoveMenu(rowItem) {
        const design = root.cardMenuDesign
        if (!rowItem || !design)
            return
        root.moveRow = rowItem
        const entry = (id, name, icon) => ({ label: name, icon: icon, payload: id,
                                             trailing: design.category === id ? "\u2713" : "" })
        moveMenu.model = [entry("", qsTr("Unlabeled"), "layers")]
            .concat(service.categories.map((c) => entry(c.id, c.name, c.icon)))
        moveMenu.openBeside(rowItem, root.Window.contentItem)
    }
    Timer {
        id: moveMenuGrace
        interval: 260
        onTriggered: if (!AppCursor.hovered(moveMenu) && !AppCursor.hovered(root.moveRow))
                         moveMenu.closeFlyout()
    }

    // Any click or scroll outside the menu closes it. The menu floats at WINDOW level (DropdownPanel lifts itself there), so its
    // catcher has to cover the whole window too - a pane-sized one left the menu hanging over other screens.
    MenuCatcher { menu: cardMenu }
    // ...and it goes when this tab does; a RE-SHOWN tab re-reads the grid
    // (the roster's order may have moved under it).
    onVisibleChanged: {
        if (!visible)
            cardMenu.visible = false
        else
            root.reload()
    }
    // A flyout goes when its menu does — same open/close life, so it can never
    // outlive the rows it belongs to.
    Connections {
        target: cardMenu
        function onVisibleChanged() { if (!cardMenu.visible) moveMenu.closeFlyout() }
    }
    DropdownPanel {
        id: cardMenu
        objectName: "selfTestDesignCardMenu"
        visible: false
        z: 25
        // A context menu sizes to its rows ("Move to <project>" is the widest), not the
        // 480 px default slab the menu bar's dropdowns use.
        fitContentWidth: true
        maxHeight: 320
        onItemHovered: (label, hovering, rowItem) => {
            if (label === root.moveLabel) {
                if (hovering) {
                    moveMenuGrace.stop()
                    root.openMoveMenu(rowItem)
                } else {
                    moveMenuGrace.restart()
                }
                return
            }
            if (hovering)
                moveMenu.closeFlyout()
        }
        onItemActivated: (label) => {
            const action = root.cardMenuActions[label]
            const design = root.cardMenuDesign
            if (action === "move") {
                if (!moveMenu.visible)
                    root.openMoveMenu(root.moveRow)
                return
            }
            cardMenu.visible = false           // first: the change below rebuilds the grid this menu was opened from
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
            }
        }
    }
    MenuCatcher { menu: moveMenu }
    DropdownPanel {
        id: moveMenu
        objectName: "selfTestDesignMoveMenu"
        visible: false
        z: 25
        fitContentWidth: true
        // The categories' own scroller: past this the list scrolls (with its
        // own AppScrollBar) instead of running past the window.
        maxHeight: 320
        // `payload` carries the category id, so a category whose name happens
        // to match another can't file the design in the wrong place.
        onItemPicked: (index, payload) => {
            const design = root.cardMenuDesign
            moveMenu.closeFlyout()
            cardMenu.visible = false
            if (design)
                service.setDesignCategory(design.id, payload === undefined ? "" : payload)
        }
    }

    // ---- dialogs ---------------------------------------------------------------------------
    NameDialog {
        id: nameDialog
        z: 30000
        onAccepted: (text) => root.nameAccepted(text)
    }
    ConfirmDialog {
        id: confirm
        z: 30000
        title: root.deletingCategory ? qsTr("Delete category?") : qsTr("Delete %1?").arg(root.noun)
        message: root.deletingCategory
                 ? qsTr("“%1” is removed. Its %2s are kept and become unlabeled.")
                       .arg(String(root.deleteName)).arg(String(root.noun))
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