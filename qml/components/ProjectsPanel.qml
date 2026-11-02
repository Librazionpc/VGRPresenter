import QtQuick
import VGRPresenterUI

Item {
    id: root

    // The item to show on the centre page (a project item map, or null). The Show screen owns the centre page; this reports.
    signal itemActivated(var item)
    signal itemOpened(var item)     // double-click: open it for editing
    signal importRequested()        // the + button's Import: the app's Import screen
    signal searchRequested()        // the + button's Show: the app-wide search
    signal dockTabRequested(string tab)   // the + button's Scripture / Media: that tab of the library dock

    readonly property color cLighter: Theme.borderSubtle
    readonly property color cDarker: Theme.rowBg
    readonly property color cDarkest: Theme.panelBg
    readonly property color cText: Theme.textPrimary
    readonly property color cSecondary: Theme.accent

    readonly property bool inProject: ProjectService.activeProjectId !== ""
    readonly property var items: ProjectService.activeProject.items ?? []
    property var collapsed: ({})     // folder id -> true while it is folded away
    property bool showArchived: false
    property bool addMenuOpen: false
    property string dropdown: ""     // the header's dots menu: "" (shut) | "view" (the tree's) | "project" (an open project's)
    property int dropIndex: -1       // where the drag over the list would land (an insertion line is drawn there)
    property string dropFolder: ""   // the tree row a project / folder drag is over

    onInProjectChanged: { root.addMenuOpen = false; root.dropdown = "" }

    // ---- what the tree shows: rows whose folders are all open ----
    readonly property var visibleTree: {
        const hidden = {}
        const out = []
        ProjectService.tree.forEach((row) => {
            if (row.archived && !root.showArchived) return
            if (hidden[row.parent] === true || root.collapsed[row.parent] === true) {
                if (row.type === "folder") hidden[row.id] = true
                return
            }
            out.push(row)
        })
        return out
    }
    // FreeShow's cards: every root folder with what is inside it, then one card for the projects that are in no folder.
    readonly property var treeCards: {
        const cards = []
        const loose = []
        let current = null
        root.visibleTree.forEach((row) => {
            if (row.depth === 0 && row.type === "folder") { current = { title: "", rows: [row] }; cards.push(current) }
            else if (row.depth === 0) loose.push(row)
            else if (current) current.rows.push(row)
        })
        if (loose.length > 0) cards.push({ title: qsTr("No Folder"), rows: loose })
        return cards
    }
    // An open project's cards: one starts at each section.
    readonly property var itemCards: {
        const cards = []
        let current = null
        root.items.forEach((item, index) => {
            const entry = Object.assign({}, item, { index: index })
            if (item.type === "section" || current === null) { current = { color: item.type === "section" ? item.color : "", rows: [] }; cards.push(current) }
            current.rows.push(entry)
        })
        return cards
    }
    // Where every item sits in the list (card margins, borders and row heights are fixed), for the drop line and the drop position.
    readonly property real sectionHeight: 28
    readonly property real itemHeight: 30
    readonly property var itemLayout: {
        const out = []
        let y = 0
        root.itemCards.forEach((card) => {
            y += 8 + 1                                   // the card's margin and its top border
            card.rows.forEach((row) => {
                const h = row.type === "section" ? root.sectionHeight : root.itemHeight
                out.push({ index: row.index, y: y, h: h })
                y += h
            })
            y += 1 + 8                                   // its bottom border and margin
        })
        return out
    }

    function toggleFolder(id) {
        const next = Object.assign({}, root.collapsed)
        next[id] = !(next[id] === true)
        root.collapsed = next
    }

    function typeIcon(type) {
        switch (type) {
        // The shows glyph (qml/assets/shows icon.svg, added to IconGlyph) fronts every show
        // in this list; a project row keeps the document mark below.
        case "show": return "shows"
        case "media": case "image": case "video": return "play"
        case "audio": return "music"
        case "overlay": return "layers"
        case "scripture": return "bible"
        case "camera": return "camera"
        case "screen": case "ndi": return "presentation"
        case "pdf": return "fileText"
        case "section": return "layoutDashboard"
        default: return "shape"
        }
    }

    // ---- menus: the dots menu, right-click menus, and what each entry does ----------------------------------------------------
    property var menuTarget: null       // the tree row / item the menu is for
    property string menuKind: ""        // "view" | "project" (the dots) | "tree" | "item" (right-click)
    function entriesFor(kind, target) {
        const entries = []
        if (kind === "view") {
            entries.push({ label: qsTr("Collapse all folders") }, { label: qsTr("Expand all folders") }, { divider: true },
                         { label: root.showArchived ? qsTr("Hide archived projects") : qsTr("Show archived projects") })
        } else if (kind === "tree") {
            if (target.type === "project") entries.push({ label: qsTr("Rename") }, { label: qsTr("Duplicate"), trailing: "Ctrl+D" }, { label: qsTr("Delete"), trailing: "Del", danger: true },
                                                        { divider: true }, { label: target.archived ? qsTr("Restore from archive") : qsTr("Archive") })
            else entries.push({ label: qsTr("New project here") }, { label: qsTr("New folder here") }, { label: qsTr("Rename") }, { divider: true }, { label: qsTr("Delete folder"), danger: true })
        } else if (kind === "item") {
            entries.push({ label: qsTr("Rename") }, { label: qsTr("Add section above") }, { divider: true }, { label: qsTr("Remove from project"), danger: true })
        } else if (kind === "projectTemplate") {
            const projects = ProjectService.tree.filter((r) => r.type === "project")
            if (projects.length === 0) entries.push({ label: qsTr("No projects yet to start from") })
            else projects.forEach((p) => entries.push({ label: p.name }))
        } else {
            const p = ProjectService.activeProject
            entries.push({ label: qsTr("Add section") }, { label: p.sectionsLocked ? qsTr("Unlock sections") : qsTr("Lock sections") }, { divider: true },
                         { label: qsTr("Rename project") }, { label: qsTr("Duplicate project") }, { label: p.archived ? qsTr("Restore from archive") : qsTr("Archive project") },
                         { divider: true }, { label: qsTr("Delete project"), danger: true })
        }
        return entries
    }
    // a right-click menu at a point of `source`
    function openMenu(kind, target, source, x, y) {
        root.menuKind = kind
        root.menuTarget = target
        menu.model = root.entriesFor(kind, target)
        menu.openAt(source, x, y, Window.window.contentItem)
    }
    function menuChosen(label) {
        menu.visible = false
        root.dropdown = ""
        const t = root.menuTarget
        if (root.menuKind === "view") {
            if (label === qsTr("Collapse all folders")) {
                const all = {}
                ProjectService.tree.forEach((r) => { if (r.type === "folder") all[r.id] = true })
                root.collapsed = all
            } else if (label === qsTr("Expand all folders")) root.collapsed = ({})
            else root.showArchived = !root.showArchived
        } else if (root.menuKind === "tree") {
            if (label === qsTr("Open")) ProjectService.openProject(t.id)
            else if (label === qsTr("Rename")) nameDialog.ask("rename", qsTr("Rename"), t.name, t.id)
            else if (label === qsTr("Duplicate")) ProjectService.duplicateProject(t.id)
            else if (label === qsTr("Archive") || label === qsTr("Restore from archive")) ProjectService.setArchived(t.id, !t.archived)
            else if (label === qsTr("Delete") || label === qsTr("Delete folder")) confirm.ask(t.id, t.name)
            else if (label === qsTr("New project here")) nameDialog.ask("project", qsTr("New project"), "", t.id)
            else if (label === qsTr("New folder here")) nameDialog.ask("folder", qsTr("New folder"), "", t.id)
        } else if (root.menuKind === "item") {
            if (label === qsTr("Rename")) nameDialog.ask("item", qsTr("Rename"), t.name, String(t.index))
            else if (label === qsTr("Add section above")) ProjectService.addSection(qsTr("Section"), t.index)
            else if (label === qsTr("Remove from project")) ProjectService.removeItem(t.index)
        } else if (root.menuKind === "projectTemplate") {
            const projects = ProjectService.tree.filter((r) => r.type === "project")
            const picked = projects.find((r) => r.name === label)
            if (picked) {
                const newId = ProjectService.duplicateProject(picked.id)
                if (newId) nameDialog.ask("rename", qsTr("Name the new project"), picked.name, newId)
            }
        } else {
            const p = ProjectService.activeProject
            if (label === qsTr("Add section")) nameDialog.ask("section", qsTr("New section"), "", "")
            else if (label === qsTr("Lock sections") || label === qsTr("Unlock sections")) ProjectService.setSectionsLocked(!p.sectionsLocked)
            else if (label === qsTr("Rename project")) nameDialog.ask("rename", qsTr("Rename project"), p.name, p.id)
            else if (label === qsTr("Duplicate project")) ProjectService.duplicateProject(p.id)
            else if (label === qsTr("Archive project") || label === qsTr("Restore from archive")) ProjectService.setArchived(p.id, !p.archived)
            else if (label === qsTr("Delete project")) confirm.ask(p.id, p.name)
        }
    }
    // the + button's menu
    function addChosen(what) {
        root.addMenuOpen = false
        if (what === "project") nameDialog.ask("project", qsTr("New project"), "", "")
        else if (what === "folder") nameDialog.ask("folder", qsTr("New folder"), "", "")
        else if (what === "projectTemplate") root.openProjectTemplateMenu()
        else if (what === "import") root.importRequested()
        else if (what === "show") root.searchRequested()
        else if (what === "scripture") root.dockTabRequested("scripture")
        else if (what === "media") root.dockTabRequested("media")
        else if (what === "section") nameDialog.ask("section", qsTr("New section"), "", "")
    }
    // "Project template": the shared dots/right-click menu (`menu`), reused for a THIRD kind of list - the tray already closed
    // itself (addChosen), so this opens where the tray was.
    function openProjectTemplateMenu() {
        root.menuKind = "projectTemplate"
        root.menuTarget = null
        menu.model = root.entriesFor("projectTemplate", null)
        menu.openAt(addTray, 0, 0, Window.window.contentItem)
    }

    component ProjectDoc: Item {
        readonly property color pillColor: Qt.lighter(root.cSecondary, 1.4)
        width: 16; height: 20
        Rectangle { anchors.fill: parent; radius: 3; color: parent.pillColor }
        Rectangle { x: 3; y: 6; width: 10; height: 1.6; radius: 0.8; color: Qt.darker(parent.pillColor, 2.2) }
        Rectangle { x: 3; y: 10; width: 10; height: 1.6; radius: 0.8; color: Qt.darker(parent.pillColor, 2.2) }
        Rectangle { x: 3; y: 14; width: 6; height: 1.6; radius: 0.8; color: Qt.darker(parent.pillColor, 2.2) }
    }

    Rectangle { anchors.fill: parent; color: Theme.workspacePaneBg }

    // ---- the tree: cards of folders and projects -------------------------------------------------------------------------------
    Flickable {
        id: treeFlick
        visible: !root.inProject
        anchors.fill: parent
        topMargin: 30
        bottomMargin: 70   // the + button floats over the bottom
        clip: true
        contentHeight: treeColumn.height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: treeColumn
            width: parent.width
            // The first folder/project card should meet the header divider;
            // keep bottom breathing room for the floating add button only.
            topPadding: 0; bottomPadding: 10
            spacing: 5

            Repeater {
                model: root.treeCards
                delegate: Item {
                    id: card
                    required property var modelData
                    width: treeColumn.width
                    height: cardBody.height

                    Column {
                        id: cardBody
                        width: parent.width
                        spacing: 0
                        // "No Folder": the heading of the projects that are in no folder
                        Rectangle {
                            visible: card.modelData.title !== ""
                            width: parent.width; height: 26
                            color: "transparent"
                            Text { x: 14; anchors.verticalCenter: parent.verticalCenter; text: card.modelData.title; opacity: 0.8; color: root.cText; font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.Medium }
                        }

                        Repeater {
                            model: card.modelData.rows
                            delegate: Rectangle {
                                id: treeRow
                                required property var modelData
                                required property int index
                                readonly property bool isFolder: modelData.type === "folder"
                                readonly property bool active: !isFolder && modelData.id === ProjectService.activeProjectId
                                readonly property real indent: 14 * modelData.depth
                                x: 0
                                width: card.width
                                height: isFolder ? 34 : 30
                                radius: 0
                                color: root.dropFolder === modelData.id ? Qt.alpha(Theme.accent, 0.19)
                                     : (isFolder || active ? root.cDarkest : root.cDarker)
                                opacity: modelData.archived ? 0.55 : 1

                                HoverGlow {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    hovered: treeHover.hovered
                                    glowColor: Qt.alpha(Theme.accent, 0.18)
                                }

                                Rectangle { width: 4; height: parent.height; color: treeRow.active || treeRow.isFolder ? root.cSecondary : root.cDarker }

                                Row {
                                    x: 14 + treeRow.indent
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 8
                                    IconGlyph {
                                        visible: treeRow.isFolder
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: "folder"
                                        color: root.cText; width: 16; height: 14
                                    }
                                    ProjectDoc { visible: !treeRow.isFolder; anchors.verticalCenter: parent.verticalCenter }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: Math.max(0, treeRow.width - treeRow.indent - (treeRow.isFolder ? 100 : 32))
                                        text: treeRow.modelData.name
                                        color: root.cText; elide: Text.ElideRight
                                        font.family: Theme.fontFamily; font.pixelSize: 16
                                        font.weight: Font.Medium
                                    }
                                }
                                // a folder's count
                                Text {
                                    visible: treeRow.isFolder && treeRow.modelData.itemCount > 0
                                    anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter
                                    width: 28; horizontalAlignment: Text.AlignRight
                                    text: treeRow.modelData.itemCount
                                    opacity: 0.5; color: root.cText; font.family: Theme.fontFamily; font.pixelSize: 13
                                }

                                HoverHandler { id: treeHover; cursorShape: Qt.PointingHandCursor }
                                DragSource {
                                    anchors.fill: parent
                                    payload: ({ kind: treeRow.modelData.type, items: [{ ref: treeRow.modelData.id, name: treeRow.modelData.name }], reorder: true })
                                    label: treeRow.modelData.name
                                    onActivated: treeRow.isFolder ? root.toggleFolder(treeRow.modelData.id) : ProjectService.openProject(treeRow.modelData.id)
                                    onOpened: if (!treeRow.isFolder) ProjectService.openProject(treeRow.modelData.id)
                                    onContextRequested: (x, y) => root.openMenu("tree", treeRow.modelData, treeRow, x, y)
                                }

                                // a project or folder dragged onto this row goes into it (onto a project: next to it)
                                DropArea {
                                    anchors.fill: parent
                                    keys: ["app-drag"]
                                    onEntered: (drag) => {
                                        const payload = drag.source.payload
                                        drag.accepted = payload && payload.reorder === true && ProjectService.acceptsDrop("projects", payload.kind, true)
                                                        && payload.items[0].ref !== treeRow.modelData.id
                                        if (drag.accepted) root.dropFolder = treeRow.modelData.id
                                    }
                                    onExited: if (root.dropFolder === treeRow.modelData.id) root.dropFolder = ""
                                    onDropped: (drop) => {
                                        root.dropFolder = ""
                                        ProjectService.moveNode(drop.source.payload.items[0].ref, treeRow.isFolder ? treeRow.modelData.id : treeRow.modelData.parent)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Text {
                visible: root.visibleTree.length === 0
                width: parent.width; topPadding: 40
                text: qsTr("Empty")
                opacity: 0.5; color: root.cText; horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontFamily; font.pixelSize: 16
            }
        }

        // dropping on the empty space takes the project out of its folder
        DropArea {
            anchors.fill: parent
            z: -1
            keys: ["app-drag"]
            onEntered: (drag) => { const p = drag.source.payload; drag.accepted = p && p.reorder === true && ProjectService.acceptsDrop("projects", p.kind, true) }
            onDropped: (drop) => ProjectService.moveNode(drop.source.payload.items[0].ref, "")
        }
    }
    AppScrollBar {
        visible: !root.inProject
        anchors.right: parent.right; anchors.rightMargin: 1
        y: 34; height: parent.height - 40
        flickable: treeFlick
    }

    // ---- an open project: cards of its items --------------------------------------------------------------------------------------
    Flickable {
        id: itemFlick
        visible: root.inProject
        anchors.fill: parent
        topMargin: 30
        bottomMargin: 70
        clip: true
        contentHeight: itemColumn.height
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: itemColumn
            width: parent.width

            Repeater {
                model: root.itemCards
                delegate: Item {
                    id: itemCard
                    required property var modelData
                    width: itemColumn.width - 5
                    height: itemCardBody.height + 2 * 8 + 2
                    Item {
                        y: 8; width: parent.width; height: itemCardBody.height + 2
                        Column {
                            id: itemCardBody
                            y: 1; width: parent.width

                            Repeater {
                                model: itemCard.modelData.rows
                                delegate: Rectangle {
                                    id: itemRow
                                    required property var modelData
                                    required property int index
                                    readonly property bool isSection: modelData.type === "section"
                                    readonly property bool selected: modelData.index === ProjectService.activeIndex
                                    readonly property color sectionColor: modelData.color !== "" && modelData.color !== undefined ? modelData.color : ""
                                    width: itemCardBody.width
                                    height: isSection ? root.sectionHeight : root.itemHeight
                                    radius: 0
                                    color: isSection ? (sectionColor !== "" ? Qt.rgba(Qt.color(sectionColor).r, Qt.color(sectionColor).g, Qt.color(sectionColor).b, 0.2) : root.cDarkest)
                                                     : (selected ? root.cDarkest : root.cDarker)

                                    HoverGlow {
                                        anchors.fill: parent
                                        radius: parent.radius
                                        hovered: itemHover.hovered && !itemRow.selected && !itemRow.isSection
                                        glowColor: Theme.hoverBg
                                    }

                                    // Flush left accent bar — the reference
                                    // design's marker for the selected item.
                                    Rectangle { visible: !itemRow.isSection; width: 4; height: parent.height; color: itemRow.selected ? root.cSecondary : root.cDarker }

                                    Row {
                                        x: itemRow.isSection ? 16 : 15
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 8
                                        IconGlyph {
                                            visible: !itemRow.isSection
                                            anchors.verticalCenter: parent.verticalCenter
                                            name: root.typeIcon(itemRow.modelData.type)
                                            color: root.cText; width: 14; height: 14
                                        }
                                        Text {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: itemRow.width - (itemRow.isSection ? 32 : 60) - (itemRow.modelData.layout ? 70 : 0)
                                            text: itemRow.modelData.name !== "" ? itemRow.modelData.name : qsTr("Unnamed")
                                            color: itemRow.isSection && itemRow.sectionColor !== "" ? Qt.lighter(itemRow.sectionColor, 1.4) : root.cText
                                            elide: Text.ElideRight
                                            font.family: Theme.fontFamily; font.pixelSize: itemRow.isSection ? 13 : 14
                                            font.weight: itemRow.isSection ? Font.Bold : Font.Medium
                                        }
                                    }
                                    // the layout a show is used with
                                    Text {
                                        visible: !itemRow.isSection && itemRow.modelData.layout !== undefined && itemRow.modelData.layout !== ""
                                        anchors.right: parent.right; anchors.rightMargin: 10; anchors.verticalCenter: parent.verticalCenter
                                        text: itemRow.modelData.layout ?? ""
                                        opacity: 0.8; color: root.cText; font.family: Theme.fontFamily; font.pixelSize: 13
                                    }

                                    HoverHandler { id: itemHover }
                                    DragSource {
                                        anchors.fill: parent
                                        payload: ({ kind: "show", items: [{ ref: itemRow.modelData.ref, name: itemRow.modelData.name }], reorder: true, indexes: [itemRow.modelData.index] })
                                        label: itemRow.modelData.name
                                        onActivated: { ProjectService.selectItem(itemRow.modelData.index); root.itemActivated(itemRow.modelData) }
                                        onOpened: root.itemOpened(itemRow.modelData)
                                        onContextRequested: (x, y) => root.openMenu("item",
                                            { index: itemRow.modelData.index, name: itemRow.modelData.name }, itemRow, x, y)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // an empty project
            Text {
                visible: root.items.length === 0
                width: parent.width; topPadding: 40
                text: qsTr("Empty")
                opacity: 0.5; color: root.cText; horizontalAlignment: Text.AlignHCenter
                font.family: Theme.fontFamily; font.pixelSize: 16
            }
        }

        // the insertion line while something is dragged over the list
        Rectangle {
            visible: root.dropIndex >= 0
            x: 0; width: parent.width - 6; height: 2; radius: 1
            color: root.cSecondary
            y: {
                const l = root.itemLayout
                if (root.dropIndex < 0) return 0      // (not shown then, but the position is still worked out)
                if (l.length === 0) return 10
                if (root.dropIndex >= l.length) return l[l.length - 1].y + l[l.length - 1].h
                return l[root.dropIndex].y - 1
            }
        }
    }
    AppScrollBar {
        visible: root.inProject
        anchors.right: parent.right; anchors.rightMargin: 1
        y: 34; height: parent.height - 40
        flickable: itemFlick
    }

    // the whole list is one drop target: things from the library panes, and items being moved
    DropArea {
        anchors.fill: itemFlick
        enabled: root.inProject
        keys: ["app-drag"]

        function indexAt(y) {
            const l = root.itemLayout
            const target = y + itemFlick.contentY - itemFlick.topMargin
            for (let i = 0; i < l.length; ++i)
                if (target < l[i].y + l[i].h / 2) return l[i].index
            return root.items.length
        }
        function acceptable(payload) {
            if (!payload) return false
            return payload.reorder === true ? ProjectService.acceptsDrop("project", payload.kind, true) && payload.indexes !== undefined
                                            : ProjectService.acceptsDrop("project", payload.kind)
        }
        onEntered: (drag) => { drag.accepted = acceptable(drag.source.payload); root.dropIndex = drag.accepted ? indexAt(drag.y) : -1 }
        onPositionChanged: (drag) => { if (drag.accepted) root.dropIndex = indexAt(drag.y) }
        onExited: root.dropIndex = -1
        onDropped: (drop) => {
            const at = indexAt(drop.y)
            root.dropIndex = -1
            const payload = drop.source.payload
            if (payload.reorder === true) ProjectService.moveItems(payload.indexes, at)
            else ProjectService.dropOnProject(payload.kind, payload.items, at)
        }
    }

    // ---- the header (FreeShow's .tabs .header): 30px, over the list, the title centred, back on the left and the dots on the right ----
    Item {
        id: header
        z: 10
        width: parent.width; height: 30
        clip: true
        Rectangle { y: -10; width: parent.width; height: 40; radius: 10; color: Qt.alpha(Theme.panelBg, 0.94) }

        // back (inside a project): 42px wide, full height
        Item {
            visible: root.inProject
            width: 42; height: parent.height
            Rectangle { anchors.fill: parent; color: backHover.hovered ? Theme.hoverBg : "transparent" }
            IconGlyph { anchors.centerIn: parent; name: "arrowLeft"; color: root.cText; width: 15; height: 12 }
            HoverHandler { id: backHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: ProjectService.closeProject() }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            x: root.inProject ? 46 : 0
            width: root.inProject ? parent.width - 46 - 36 : parent.width - 20   // (FreeShow: the tree's title is 20px off centre)
            horizontalAlignment: Text.AlignHCenter
            text: root.inProject ? (ProjectService.activeProject.name ?? "") : qsTr("Projects")
            color: root.cText; elide: Text.ElideRight
            font.family: Theme.fontFamily; font.pixelSize: 17; font.weight: Font.DemiBold
            TapHandler { enabled: root.inProject; onDoubleTapped: nameDialog.ask("rename", qsTr("Rename project"), ProjectService.activeProject.name, ProjectService.activeProject.id) }
        }
        // the dots: 32px wide, full height
        Item {
            id: moreBtn
            x: parent.width - width; width: 32; height: parent.height
            Rectangle { anchors.fill: parent; color: moreHover.hovered ? Theme.hoverBg : "transparent" }
            IconGlyph { anchors.centerIn: parent; name: "moreVertical"; color: root.cText; opacity: root.dropdown !== "" ? 1 : 0.8; width: 4; height: 14 }
            HoverHandler { id: moreHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: { root.addMenuOpen = false; root.dropdown = root.dropdown === "" ? (root.inProject ? "project" : "view") : "" } }
        }
    }

    // the dots' menu (FreeShow's .projectDropdown)
    Rectangle {
        id: dropdownCard
        visible: root.dropdown !== ""
        z: 30
        x: root.width - width - 5; y: 31
        width: 214; height: dropdownColumn.height + 2
        radius: 6; color: root.cDarkest; border.color: root.cLighter
        Column {
            id: dropdownColumn
            x: 1; y: 1; width: parent.width - 2
            Repeater {
                model: root.dropdown !== "" ? root.entriesFor(root.dropdown, null) : []
                delegate: Item {
                    id: entry
                    required property var modelData
                    width: dropdownColumn.width
                    height: modelData.divider === true ? 1 : 34
                    Rectangle { visible: entry.modelData.divider === true; width: parent.width; height: 1; color: root.cLighter }
                    Rectangle { visible: entry.modelData.divider !== true; anchors.fill: parent; color: entryHover.hovered ? Theme.hoverBg : "transparent" }
                    Text {
                        visible: entry.modelData.divider !== true
                        x: 12; anchors.verticalCenter: parent.verticalCenter
                        text: entry.modelData.label ?? ""
                        color: entry.modelData.danger === true ? Theme.dangerLight : root.cText
                        // Medium for the same reason as the names behind it: a
                        // Normal mono stroke at 15 px reads grey, not white.
                        font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.Medium
                    }
                    HoverHandler { id: entryHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        enabled: entry.modelData.divider !== true
                        onTapped: { root.menuKind = root.dropdown; root.menuTarget = null; root.menuChosen(entry.modelData.label) }
                    }
                }
            }
        }
    }

    // a press anywhere else in the panel shuts the dots menu and the + menu
    MouseArea {
        anchors.fill: parent
        z: 9
        enabled: root.dropdown !== "" || root.addMenuOpen
        onPressed: (mouse) => { root.dropdown = ""; root.addMenuOpen = false; mouse.accepted = true }
    }

    // ---- the + menu: pill buttons in a rounded tray above the + button (FreeShow's .addMenu) ------------------------------------------
    // one pill: a leading icon, the label, and an optional faint icon at the right
    component AddPill: Rectangle {
        id: pill
        property string label: ""
        property string glyph: ""      // an IconGlyph name, or "project" for the accent-purple document
        property string hint: ""       // a small icon at the right
        signal picked()
        width: parent ? parent.width : 190; height: 35; radius: 17.5
        color: pillHover.hovered ? Theme.hoverBg : Theme.panelBg
        border.color: root.cLighter
        ProjectDoc { visible: pill.glyph === "project"; x: 18; anchors.verticalCenter: parent.verticalCenter; scale: 0.8 }
        IconGlyph { visible: pill.glyph !== "project" && pill.glyph !== ""; x: 18; anchors.verticalCenter: parent.verticalCenter; name: pill.glyph; color: root.cText; fit: true; width: 15; height: 15 }
        Text {
            x: 46; anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 46 - (pill.hint !== "" ? 40 : 16)
            text: pill.label; color: root.cText; elide: Text.ElideRight
            font.family: Theme.fontFamily; font.pixelSize: 16; font.weight: Font.Medium
        }
        IconGlyph { visible: pill.hint !== ""; anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; name: pill.hint; color: root.cText; opacity: 0.25; fit: true; width: 12; height: 12 }
        HoverHandler { id: pillHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: pill.picked() }
    }

    Rectangle {
        id: addTray
        visible: root.addMenuOpen
        z: 20
        x: root.width - width - 12; y: root.height - 70 - height
        width: 202; height: trayColumn.height + 12
        radius: 25; color: Qt.alpha(Theme.panelBg, 0.94); border.color: Theme.borderSubtle
        Column {
            id: trayColumn
            x: 6; y: 6; width: parent.width - 12
            spacing: 2

            // in the tree: Project, Folder, Project template | Import
            AddPill { visible: !root.inProject; label: qsTr("Project"); glyph: "project"; onPicked: root.addChosen("project") }
            AddPill { visible: !root.inProject; label: qsTr("Folder"); glyph: "folder"; onPicked: root.addChosen("folder") }
            AddPill { visible: !root.inProject; label: qsTr("Project template"); glyph: "layoutTemplate"; onPicked: root.addChosen("projectTemplate") }
            Item { visible: !root.inProject; width: 1; height: 6 }
            AddPill { visible: !root.inProject; label: qsTr("Import"); glyph: "download"; hint: "folder"; onPicked: root.addChosen("import") }

            // in a project: Show, Scripture, Media | Import | Section
            AddPill { visible: root.inProject; label: qsTr("Show"); glyph: "shows"; hint: "search"; onPicked: root.addChosen("show") }
            AddPill { visible: root.inProject; label: qsTr("Scripture"); glyph: "bible"; hint: "search"; onPicked: root.addChosen("scripture") }
            AddPill { visible: root.inProject; label: qsTr("Media"); glyph: "layoutTemplate"; hint: "search"; onPicked: root.addChosen("media") }
            Item { visible: root.inProject; width: 1; height: 6 }
            AddPill { visible: root.inProject; label: qsTr("Import"); glyph: "download"; onPicked: root.addChosen("import") }
            Item { visible: root.inProject; width: 1; height: 6 }
            AddPill { visible: root.inProject; label: qsTr("Section"); glyph: "layoutDashboard"; opacity: ProjectService.activeProject.sectionsLocked ? 0.5 : 1; onPicked: if (!ProjectService.activeProject.sectionsLocked) root.addChosen("section") }
        }
    }

    // ---- the + button (FreeShow's FloatingInputs gradient): a 50px ring; the plus turns 135 degrees into an x while the menu is open -------
    Item {
        id: addButton
        z: 21
        x: root.width - width - 12; y: root.height - height - 10
        width: 50; height: 50

        // the soft shadow of a floating button
        Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 4; height: width; radius: width / 2; color: "#40000000" }
        // Derive the whole ring from the chosen accent; fixed violet stops made
        // most of this button stay purple when the user picked another color.
        Rectangle {
            anchors.fill: parent; radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.accentLight }
                GradientStop { position: 0.25; color: root.cSecondary }
                GradientStop { position: 0.65; color: Qt.darker(root.cSecondary, 1.25) }
                GradientStop { position: 1.0; color: root.cSecondary }
            }
            Rectangle {
                anchors.fill: parent; anchors.margins: 2; radius: width / 2
                color: Qt.alpha(addHover.hovered ? Theme.hoverBg : Theme.rowBg, 0.94)
                PlusGlyph {
                    anchors.centerIn: parent
                    size: 21; thickness: 2.4; color: root.cText
                    rotation: root.addMenuOpen ? 135 : 0
                    Behavior on rotation { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                }
            }
        }
        HoverHandler { id: addHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            onTapped: { root.dropdown = ""; root.addMenuOpen = !root.addMenuOpen }
            onDoubleTapped: { if (!root.addMenuOpen) return; root.addMenuOpen = false; root.addChosen(root.inProject ? "section" : "project") }
        }
    }

    // ---- shared pieces ---------------------------------------------------------------------------------------------------------
    DropdownPanel {
        id: menu
        visible: false
        z: 25
        width: 200
        // Harmless for every short menu this is also used for (view/tree/item/project) - only "projectTemplate" (one entry per
        // existing project, however many there are) ever grows past this and needs the scroll it turns on.
        maxHeight: 320
        onItemActivated: (label) => root.menuChosen(label)
    }
    MenuCatcher { menu: menu }

    // Ask for a name (new project / folder / section, rename): what it is for rides along in `what`.
    NameDialog {
        id: nameDialog
        parent: Window.window ? Window.window.contentItem : null
        z: 30000
        property string what: ""
        property string target: ""
        function ask(kind, title, initial, id) {
            what = kind
            target = id
            nameDialog.title = title
            nameDialog.confirmLabel = kind === "rename" || kind === "item" ? qsTr("Rename") : qsTr("Create")
            nameDialog.allowEmpty = kind === "project" || kind === "folder" || kind === "section"
            open(initial)
        }
        onAccepted: (text) => {
            if (what === "project") ProjectService.createProject(text, target)
            else if (what === "folder") ProjectService.createFolder(text, target)
            else if (what === "section") ProjectService.addSection(text)
            else if (what === "rename") ProjectService.rename(target, text)
            else if (what === "item") ProjectService.renameItem(Number(target), text)
        }
    }
    ConfirmDialog {
        id: confirm
        parent: Window.window ? Window.window.contentItem : null
        z: 30000
        property string target: ""
        title: qsTr("Delete?")
        message: qsTr("“%1” is deleted. Its shows and media are not touched. This can't be undone.").arg(confirm.name)
        property string name: ""
        confirmLabel: qsTr("Delete")
        function ask(id, itemName) { target = id; name = itemName; open() }
        onConfirmed: { confirm.close(); ProjectService.deleteNode(confirm.target) }
    }
}