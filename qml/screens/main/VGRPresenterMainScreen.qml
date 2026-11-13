import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../../components"
// The app's action-named keyboard binding (imported AFTER QtQuick so `Shortcut`
// is the shortcuts/ wrapper; the chords live in qml/settings/Hotkeys.qml).
import "../../shortcuts"

Rectangle {
    id: vGRPresenter_Main_Screen

    // ---- Menus this screen lifts into the window overlay ------------------
    // A DropdownPanel reparents itself to Overlay.overlay when it opens, which puts
    // it ABOVE the Settings card (z 30000 in the window). Settings calls this as it
    // opens so no row menu from this screen can float over it.
    function closeContextMenus() {
        showContextMenu.visible = false
        showMoveMenu.closeFlyout()
        catContextMenu.visible = false
    }

    Component.onCompleted: EngineBridge.log("info", "StartupTrace", "main screen component completed")
    onVisibleChanged: if (visible) EngineBridge.log("info", "StartupTrace", "main screen became visible")

    height: 900
    width: 1440

    border.color: Theme.border
    border.width: 1
    clip: true
    color: Theme.windowBg

    property real projectsColumnOverride: Number(SettingsService.values["session.projectsColumnWidth"] ?? -1)
    property real outputsColumnOverride: Number(SettingsService.values["session.outputsColumnWidth"] ?? -1)
    property real templateColumnOverride: Number(SettingsService.values["session.templateColumnWidth"] ?? -1)
    function saveColumnWidth(key, value) {
        const stored = SettingsService.values[key]
        if (stored !== undefined && stored !== null && Number(stored) === value)
            return
        SettingsService.setValue(key, value)
    }
    Timer {
        id: saveColumnWidthsTimer
        interval: 500
        onTriggered: {
            if (!SettingsService.ready) {
                saveColumnWidthsTimer.restart()
                return
            }
            const s = vGRPresenter_Main_Screen
            saveColumnWidth("session.projectsColumnWidth", Math.round(s.projectsColumnOverride))
            saveColumnWidth("session.outputsColumnWidth", Math.round(s.outputsColumnOverride))
            saveColumnWidth("session.templateColumnWidth", Math.round(s.templateColumnOverride))
            saveColumnWidth("session.dockHeightTop", Math.round(workspace_body.topOverride))
        }
    }
    onProjectsColumnOverrideChanged: saveColumnWidthsTimer.restart()
    onOutputsColumnOverrideChanged: saveColumnWidthsTimer.restart()
    onTemplateColumnOverrideChanged: saveColumnWidthsTimer.restart()
    readonly property real projectsColumnMaxWidth: Math.min(560, Math.max(160,
        width - Math.min(560, Math.max(240, width * 0.278)) - 400))
    readonly property real projectsColumnWidth: Math.max(160, Math.min(projectsColumnMaxWidth,
        projectsColumnOverride > 0 ? projectsColumnOverride : Math.min(280, Math.max(160, width * 0.195))))
    readonly property real outputsColumnMaxWidth: Math.min(560,
        Math.max(240, width - projectsColumnWidth - 400))
    readonly property real outputsColumnWidth: Math.max(240, Math.min(outputsColumnMaxWidth,
        outputsColumnOverride > 0 ? outputsColumnOverride : width * 0.278))
    readonly property real templateColumnMaxWidth: Math.min(560, Math.max(220, width - projectsColumnWidth - 400))
    readonly property real templateColumnWidth: Math.max(220, Math.min(templateColumnMaxWidth,
        templateColumnOverride > 0 ? templateColumnOverride
                                   : (width - projectsColumnWidth - outputsColumnWidth) * 0.36))

    signal newShowRequested()
    // A shows-table row was clicked — `path` is the .vgr to open (Main.qml
    // routes it into the engine with the unsaved-changes guard).
    signal openShowRequested(string path)
    // The Projects panel's search box or the "Quick search" button was used — Main.qml
    // opens the app-wide search (SearchService) for it.
    signal searchRequested()
    // The Projects panel's + button > Import: Main.qml opens the Import screen.
    signal importRequested()
    // A card's Edit action in the Overlays / Templates tab: open design `id` (`kind` = "overlay" | "template") in the
    // Edit screen (Main.qml switches to it).
    signal designEditRequested(string kind, string id)
    // ShowCenter can create a template from a show's text boxes and hand it to
    // the same editor route used by the Templates library.
    signal showTemplateEditRequested(string id)
    signal actionRunRequested(string actionId)
    // What the centre page shows (FreeShow's activeShow): a project item, or a show clicked in the Shows table. null = the splash.
    property var centerItem: null
    // The two reference panes load asynchronously (see their Loaders): null until loaded, then the pane itself.
    readonly property var scripturePane: scriptureLoader.item
    readonly property var tablePane: tableLoader.item
    // ---- the Shows table's arrow keys (from the Hotkeys map) ---------------
    readonly property bool typingInField: {
        const w = Window.window
        const f = w ? w.activeFocusItem : null
        return !!f && f.text !== undefined && f.cursorPosition !== undefined
    }
    readonly property bool showsKeysMine: vGRPresenter_Main_Screen.visible
        && media_tab_bar.currentPane === "shows" && !vGRPresenter_Main_Screen.typingInField
    Shortcut { action: "previousPassage"; enabled: vGRPresenter_Main_Screen.showsKeysMine; onActivated: table_body.moveShowCursor(-1) }
    Shortcut { action: "nextPassage"; enabled: vGRPresenter_Main_Screen.showsKeysMine; onActivated: table_body.moveShowCursor(1) }
    Shortcut {
        action: "accept"
        enabled: vGRPresenter_Main_Screen.showsKeysMine && !Hotkeys.canAccept
        onActivated: table_body.openCursorShow()
    }

    // Opening a project puts its first item there; clicking another item in the panel puts that one.
    Connections {
        target: ProjectService
        function onActiveChanged() {
            if (ProjectService.activeProjectId !== "" && ProjectService.activeIndex >= 0)
                vGRPresenter_Main_Screen.centerItem = ProjectService.activeItem
        }
    }
    // The Scripture tab's "Convert to show": the show's name and its slides ([{ title, background, blocks }]), built by the engine.
    signal scriptureShowRequested(string name, var slides)

    function escapeBack() { return showCenter.escapeBack() }
    // Opens a dock tab with a search already typed in it ("media" + a name = that media file, found).
    // The app-wide search uses this to take you to what it found.
    function showInLibrary(pane, query) {
        for (let i = 0; i < media_tab_bar.tabs.length; ++i) {
            if (media_tab_bar.tabs[i].pane === pane) {
                media_tab_bar.currentTab = i
                media_tab_bar.setSearch(query)
                return
            }
        }
    }
    function openSermonAt(bookId, chapter, verse, matchText) {
        const t0 = Date.now()
        showInLibrary("table", "")          // the tab + a cleared query
        const t1 = Date.now()
        if (!tablePane) return              // (still loading: the tab is switched, there is no pane to point yet)
        tablePane.clearCitationMatches()    // (a stale filter list must not cover the sermon)
        tablePane.goTo({ bookId: bookId, chapter: chapter, verseStart: verse, verseEnd: verse,
                         matchText: matchText || "" })
        // Where the click's time actually goes: the tab/pane switch vs. the goTo
        // (the engine's chapter read + the verses column's rebuild).
        EngineBridge.log("info", "OpenTrace",
                         "openSermonAt switch=" + (t1 - t0) + "ms goto=" + (Date.now() - t1) + "ms")
    }
    function openVerseAt(r) {
        showInLibrary("scripture", "")      // the tab + a cleared query
        if (!scripturePane) return
        scripturePane.clearCitationMatches()
        if (r.bookId !== undefined && r.bookId !== "") {
            const bible = r.bibleId !== undefined && r.bibleId !== "" ? r.bibleId : scripturePane.sourceId
            if (bible !== scripturePane.sourceId)
                scripturePane.openSource(bible)
            scripturePane.goTo({ bookId: r.bookId, chapter: r.chapter, verseStart: r.verse, verseEnd: r.verse })
        } else {
            const ref = scripturePane.adapter.resolve(r.title, scripturePane.sourceId)
            if (ref.bookId !== undefined)
                scripturePane.goTo(ref)
        }
    }

    Rectangle {
        id: workspace_body

        y: 48

        height: parent.height - 48
        width: parent.width
        property real topOverride: Number(SettingsService.values["session.dockHeightTop"] ?? -1)
        readonly property real minTop: 320
        readonly property real maxTop: Math.max(minTop, height - 220)
        readonly property real topHeight: Math.max(minTop, Math.min(maxTop,
            topOverride > 0 ? topOverride : Math.max(460, Math.round(height * 0.54))))

        color: "transparent"

        Rectangle {
            id: left_column

            // The project pane ends where the top workspace ends; the library
            // dock below spans the full window independently.
            height: workspace_body.topHeight
            width: vGRPresenter_Main_Screen.projectsColumnWidth

            border.color: Theme.border
            border.width: 1
            color: Theme.workspacePaneBg

            Rectangle {
                id: projects_section

                height: workspace_body.topHeight
                width: parent.width

                color: "transparent"

                // The real Projects panel: the engine's folders and projects, the open project's items, drag and drop.
                ProjectsPanel {
                    id: projects_list
                    objectName: "selfTestProjectsPanel"

                    x: 0
                    y: 0

                    height: workspace_body.topHeight
                    width: parent.width

                    onItemActivated: (item) => vGRPresenter_Main_Screen.centerItem = item
                    onItemOpened: {}
                    onImportRequested: vGRPresenter_Main_Screen.importRequested()
                    onSearchRequested: vGRPresenter_Main_Screen.searchRequested()
                    onDockTabRequested: (tab) => {
                        const at = media_tab_bar.tabs.findIndex((t) => t.pane === tab)
                        if (at >= 0) media_tab_bar.currentTab = at
                    }
                }
            }
        }
        Rectangle {
            id: middle_column

            x: left_column.width

            height: parent.height
            width: parent.width - left_column.width - right_column.width

            color: "transparent"

            Rectangle {
                id: presentation_slate

                height: workspace_body.topHeight
                width: parent.width

                border.color: Theme.border
                border.width: 1
                color: Theme.windowBg

                Rectangle {
                    id: logo_group

                    // (Content sits centred in the slate: the offsets keep it in the middle as the slate grows.)
                    x: 40
                    y: 76 + (workspace_body.topHeight - 460) / 2

                    height: 80
                    width: parent.width - 80

                    color: "transparent"

                    Row {
                        id: vGRPresenter_1

                        x: (parent.width - width) / 2

                        height: 60
                        spacing: 0

                        Text {
                            height: 60
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.letterSpacing: 0.48
                            font.pixelSize: 55
                            font.weight: Font.Black
                            text: qsTr("VGR")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                        Text {
                            height: 60
                            color: Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.letterSpacing: 0.48
                            font.pixelSize: 55
                            font.weight: Font.Black
                            text: qsTr("Presenter")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                    }
                    Text {
                        id: v1_0_5_beta_2

                        x: (parent.width - width) / 2
                        y: 64

                        height: 16
                        width: 83

                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("v%1").arg(SettingsService.appVersion)
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                }
                Rectangle {
                    id: scripture_quote_container

                    x: (parent.width - width) / 2
                    y: 188 + (workspace_body.topHeight - 460) / 2

                    height: 40
                    width: Math.min(520, parent.width - 40)

                    color: "transparent"

                    Text {
                        id: for_everything_there_is_an_appointed_time_and_an

                        height: 40
                        width: parent.width

                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.Normal
                        horizontalAlignment: Text.AlignHCenter
                        lineHeight: 19.50
                        lineHeightMode: Text.FixedHeight
                        text: qsTr("\"For everything there is an appointed time, and an appropriate time for every activity on earth: - Ecclesiastes 3:1\"")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                        wrapMode: Text.Wrap
                    }
                }
                Rectangle {
                    id: core_actions

                    x: (parent.width - width) / 2
                    y: 260 + (workspace_body.topHeight - 460) / 2

                    height: 124
                    width: 240

                    color: "transparent"

                    Rectangle {
                        id: action_Quick_search

                        height: 36
                        width: 240

                        border.color: Theme.border
                        border.width: 1
                        color: actionQuickSearchMouse.pressed ? Theme.activeBg : Theme.rowBg
                        radius: 8
                        HoverGlow {
                            anchors.fill: parent
                            radius: parent.radius
                            hovered: actionQuickSearchHover.hovered || actionQuickSearchPosition.hovered
                            glowColor: Theme.hoverBg
                            edge: true
                            edgeColor: Theme.borderSubtle
                        }

                        HoverHandler {
                            id: actionQuickSearchHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        PositionHoverArea {
                            id: actionQuickSearchPosition
                            anchors.fill: parent
                            showCursor: false
                            clickEnabled: false
                        }

                        MouseArea {
                            id: actionQuickSearchMouse
                            anchors.fill: parent
                            onClicked: vGRPresenter_Main_Screen.searchRequested()
                        }

                        Rectangle {
                            id: search_2

                            x: 20
                            y: 11

                            height: 14
                            width: 14

                            clip: true
                            color: "transparent"

                            IconGlyph {
                                anchors.centerIn: parent
                                name: "search"
                                color: Theme.accent
                                strokeWidth: 2
                                width: 14; height: 14
                            }
                        }
                        Text {
                            id: quick_search

                            x: 42
                            y: 10

                            height: 16
                            width: 159

                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 15
                            font.weight: Font.Medium
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("Quick search")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignVCenter
                            wrapMode: Text.Wrap
                        }
                        Rectangle {
                            id: chevron_right

                            x: 208
                            y: 12

                            height: 12
                            width: 12

                            clip: true
                            color: "transparent"

                            IconGlyph {
                                anchors.centerIn: parent
                                name: "chevronRight"
                                color: Theme.textMuted
                                strokeWidth: 2
                                width: 12; height: 12
                            }
                        }
                    }
                    Rectangle {
                        id: action_New_project

                        y: 44

                        height: 36
                        width: 240

                        border.color: Theme.border
                        border.width: 1
                        color: actionNewProjectMouse.pressed ? Theme.activeBg : Theme.rowBg
                        radius: 8
                        HoverGlow {
                            anchors.fill: parent
                            radius: parent.radius
                            hovered: actionNewProjectHover.hovered || actionNewProjectPosition.hovered
                            glowColor: Theme.hoverBg
                            edge: true
                            edgeColor: Theme.borderSubtle
                        }

                        HoverHandler {
                            id: actionNewProjectHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        PositionHoverArea {
                            id: actionNewProjectPosition
                            anchors.fill: parent
                            showCursor: false
                            clickEnabled: false
                        }

                        MouseArea {
                            id: actionNewProjectMouse
                            anchors.fill: parent
                            onClicked: EventBus.notify(
                                qsTr("Project creation isn't wired yet."),
                                "info", "New project", "ui.main.newProject")
                        }

                        Rectangle {
                            id: plus

                            x: 20
                            y: 11

                            height: 14
                            width: 14

                            clip: true
                            color: "transparent"

                            IconGlyph {
                                anchors.centerIn: parent
                                name: "plus"
                                color: Theme.accent
                                strokeWidth: 2
                                width: 14; height: 14
                            }
                        }
                        Text {
                            id: new_project

                            x: 42
                            y: 10

                            height: 16
                            width: 159

                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 15
                            font.weight: Font.Medium
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("New project")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignVCenter   // (see quick_search: glyph/icon alignment)
                            wrapMode: Text.Wrap
                        }
                        Rectangle {
                            id: chevron_right_1

                            x: 208
                            y: 12

                            height: 12
                            width: 12

                            clip: true
                            color: "transparent"

                            IconGlyph {
                                anchors.centerIn: parent
                                name: "chevronRight"
                                color: Theme.textMuted
                                strokeWidth: 2
                                width: 12; height: 12
                            }
                        }
                    }
                    Rectangle {
                        id: action_New_show

                        y: 88

                        height: 36
                        width: 240

                        border.color: Theme.border
                        border.width: 1
                        color: actionNewShowMouse.pressed ? Theme.activeBg : Theme.rowBg
                        radius: 8
                        HoverGlow {
                            anchors.fill: parent
                            radius: parent.radius
                            hovered: actionNewShowHover.hovered || actionNewShowPosition.hovered
                            glowColor: Theme.hoverBg
                            edge: true
                            edgeColor: Theme.borderSubtle
                        }

                        HoverHandler {
                            id: actionNewShowHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        PositionHoverArea {
                            id: actionNewShowPosition
                            anchors.fill: parent
                            showCursor: false
                            clickEnabled: false
                        }

                        MouseArea {
                            id: actionNewShowMouse
                            anchors.fill: parent
                            onClicked: vGRPresenter_Main_Screen.newShowRequested()
                        }

                        Rectangle {
                            id: new_show_glyph

                            x: 20
                            y: 11

                            height: 14
                            width: 14

                            clip: true
                            color: "transparent"

                            IconGlyph {
                                anchors.centerIn: parent
                                name: "newShow"
                                color: Theme.accent
                                strokeWidth: 2
                                width: 14; height: 14
                            }
                        }
                        Text {
                            id: new_show

                            x: 42
                            y: 10

                            height: 16
                            width: 159

                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 15
                            font.weight: Font.Medium
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("New show")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignVCenter   // (see quick_search: glyph/icon alignment)
                            wrapMode: Text.Wrap
                        }
                        Rectangle {
                            id: chevron_right_2

                            x: 208
                            y: 12

                            height: 12
                            width: 12

                            clip: true
                            color: "transparent"

                            IconGlyph {
                                anchors.centerIn: parent
                                name: "chevronRight"
                                color: Theme.textMuted
                                strokeWidth: 2
                                width: 12; height: 12
                            }
                        }
                    }
                }
                // What the clicked show / item looks like (FreeShow's centre page); the splash above stays until something is clicked.
                ShowCenter {
                    id: showCenter
                    objectName: "selfTestShowCenter"
                    anchors.fill: parent
                    visible: vGRPresenter_Main_Screen.centerItem !== null
                    item: vGRPresenter_Main_Screen.centerItem
                    slideHotkeysEnabled: media_tab_bar.currentPane === "shows"
                    onEditRequested: (path) => vGRPresenter_Main_Screen.openShowRequested(path)
                    onTemplateEditRequested: (id) => vGRPresenter_Main_Screen.showTemplateEditRequested(id)
                    onCloseRequested: vGRPresenter_Main_Screen.centerItem = null
                }
            }
        }
            Rectangle {
                id: media_resource_dock

                x: 0
                y: workspace_body.topHeight

                height: parent.height - workspace_body.topHeight
                width: parent.width

                color: Theme.workspacePaneBg

                Rectangle {
                    id: dock_tab_bar

                    height: 39
                    width: parent.width

                    border.color: Theme.border
                    border.width: 1
                    color: Theme.panelBg

                    // ---- Live tab bar ---- one shared component
                    LibraryTabBar {
                        id: media_tab_bar
                        x: 0
                        y: 0
                        width: parent.width
                        height: 39
                    }
                }
                Rectangle {
                    id: media_table

                    objectName: "selfTestShowsTable"

                    y: 39

                    // Tracks the dock's taller body so every pane (shows
                    // table, browsers, media grid) fills to the bottom.
                    height: parent.height - 39
                    width: parent.width

                    clip: true
                    color: "transparent"

                    // ---- Per-tab panes ---- Shows keeps the original table
                    Loader {
                        id: scriptureLoader
                        asynchronous: true
                        width: parent.width; height: parent.height
                        sourceComponent: Component {
                    ScripturePane {
                        id: scripturePane
                        objectName: "selfTestScripturePane"
                        visible: media_tab_bar.currentPane === "scripture"
                        onVisibleChanged: if (visible) EngineBridge.log("info", "StartupTrace", "Scripture pane became visible")
                        Component.onCompleted: {
                            EngineBridge.log("info", "StartupTrace", "Scripture pane component completed")
                            media_tab_bar.registerSuggester("scripture", scripturePane)
                        }
                        width: parent.width; height: parent.height
                        filter: media_tab_bar.searches.scripture !== undefined ? media_tab_bar.searches.scripture : ""
                        onTemplateEditRequested: (id) => vGRPresenter_Main_Screen.designEditRequested("template", id)
                        onConvertToShowRequested: (name, slides) => vGRPresenter_Main_Screen.scriptureShowRequested(name, slides)
                        // Live reference autocomplete: the pane builds the rows (its suggest()),
                        // the tab bar feeds them to the search box and routes picks back here.
                        onSuggestionChosen: (ref) => scripturePane.applySuggestion(ref)
                        Connections {
                            target: LiveOutputService
                            function onPassageStepRequested(direction) {
                                if (scripturePane.adapter.liveIsOurs(scripturePane.referenceText)
                                        || scripturePane.adapter.stagedIsOurs(scripturePane.referenceText))
                                    scripturePane.stepAndPlay(direction)
                            }
                        }
                    }
                        }
                    }
                    // Loaded ASYNCHRONOUSLY: the pane is a ~4500-line component and the one the user is NOT looking at
                    // is instantiated in idle slices after the window is up, not inside the blocking first load.
                    Loader {
                        id: tableLoader
                        asynchronous: true
                        width: parent.width; height: parent.height
                        sourceComponent: Component {
                    TheTablePane {
                        id: tablePane
                        objectName: "selfTestTablePane"
                        visible: media_tab_bar.currentPane === "table"
                        onVisibleChanged: if (visible) EngineBridge.log("info", "StartupTrace", "The Table pane became visible")
                        Component.onCompleted: {
                            EngineBridge.log("info", "StartupTrace", "The Table pane component completed")
                            media_tab_bar.registerSuggester("table", tablePane)
                        }
                        width: parent.width; height: parent.height
                        filter: media_tab_bar.searches.table !== undefined ? media_tab_bar.searches.table : ""
                        // The Table autocompletes the same way (its suggest() offers sermons).
                        onSuggestionChosen: (ref) => tablePane.applySuggestion(ref)
                        // Same passage-step relay as Scripture (see there).
                        Connections {
                            target: LiveOutputService
                            function onPassageStepRequested(direction) {
                                // Ownership, not visibility (see Scripture's own
                                // relay above for why).
                                if (tablePane.adapter.liveIsOurs(tablePane.referenceText)
                                        || tablePane.adapter.stagedIsOurs(tablePane.referenceText))
                                    tablePane.stepAndPlay(direction)
                            }
                        }
                        // The same show-building path Scripture uses (Main.qml's
                        // handler is generic: name + slides -> a new show).
                        onConvertToShowRequested: (name, slides) => vGRPresenter_Main_Screen.scriptureShowRequested(name, slides)
                        // The template card's edit pencil opens it in the Edit screen.
                        onTemplateEditRequested: (id) => vGRPresenter_Main_Screen.designEditRequested("template", id)
                    }
                        }
                    }
                    MediaLibraryPane {
                        id: mediaPane
                        objectName: "selfTestMediaPane"
                        onVisibleChanged: if (visible) EngineBridge.log("info", "StartupTrace", "Media pane became visible")
                        Component.onCompleted: EngineBridge.log("info", "StartupTrace", "Media pane component completed")
                        onItemActivated: (item) => vGRPresenter_Main_Screen.centerItem = item
                        onItemOpened: (item) => {
                            ProjectService.dropOnProject("media", [{ ref: item.ref, name: item.name }])
                            LiveOutputService.takeMedia(item.ref, item.name)
                        }
                        visible: media_tab_bar.currentPane === "media"
                        filter: media_tab_bar.searches.media !== undefined ? media_tab_bar.searches.media : ""
                        width: parent.width - shows_template_sidebar.width; height: parent.height
                    }
                    DesignLibraryPane {
                        objectName: "selfTestOverlaysPane"
                        visible: media_tab_bar.currentPane === "overlays"
                        service: OverlayLibraryService
                        noun: "overlay"
                        stateKey: "session.overlaysReading"
                        onDesignActivated: (id, name) => vGRPresenter_Main_Screen.centerItem = { type: "overlay", ref: id, name: name }
                        onDesignOpened: (id, name) => {
                            ProjectService.dropOnProject("overlay", [{ ref: id, name: name }])
                            if (LiveOutputService.overlayIsOnAir(id))
                                LiveOutputService.clearOverlay(id)
                            else
                                LiveOutputService.takeOverlay(id, name)
                        }
                        onDesignOpenRequested: (id) => vGRPresenter_Main_Screen.designEditRequested("overlay", id)
                        filter: media_tab_bar.searches.overlays !== undefined ? media_tab_bar.searches.overlays : ""
                        width: parent.width - shows_template_sidebar.width; height: parent.height
                    }
                    DesignLibraryPane {
                        objectName: "selfTestTemplatesPane"
                        visible: media_tab_bar.currentPane === "templates"
                        service: TemplateLibraryService
                        noun: "template"
                        stateKey: "session.templatesReading"
                        onDesignOpenRequested: (id) => vGRPresenter_Main_Screen.designEditRequested("template", id)
                        filter: media_tab_bar.searches.templates !== undefined ? media_tab_bar.searches.templates : ""
                        width: parent.width - shows_template_sidebar.width; height: parent.height
                    }
                    LibraryComingSoonPane {
                        objectName: "selfTestSoonPane"
                        visible: media_tab_bar.currentPane === "soon"
                        width: parent.width - shows_template_sidebar.width; height: parent.height
                        title: media_tab_bar.tabs[media_tab_bar.currentTab].label
                        message: qsTr("This library is next on the roadmap.")
                        newLabel: qsTr("New ") + media_tab_bar.tabs[media_tab_bar.currentTab].label
                        onNewRequested: EventBus.notify(
                            qsTr("%1 creation isn't wired yet — the pane shell is ready.")
                                .arg(media_tab_bar.tabs[media_tab_bar.currentTab].label),
                            "info", media_tab_bar.tabs[media_tab_bar.currentTab].label, "library.soon.new")
                    }
                    // Functions — the service-flow automation pane (the engine's
                    // Flow model: load/run/pause/skip/stop whole services).
                    FunctionsPane {
                        objectName: "selfTestFunctionsPane"
                        visible: media_tab_bar.currentPane === "functions"
                        width: parent.width - shows_template_sidebar.width; height: parent.height
                        onRunActionRequested: (actionId) => vGRPresenter_Main_Screen.actionRunRequested(actionId)
                    }

                    // ---- Categories sidebar (Shows tab) ---- the sample's
                    property int currentCategory: 0   // 0 = All, -1 = Unlabeled, 1..n = category
                    property bool placeRestored: false
                    function saveShowsPlace() {
                        if (!SettingsService.ready || table_scroll.count === 0)
                            return
                        const at = table_scroll.indexAt(4, table_scroll.contentY + 4)
                        const rows = table_body.shownShows
                        const top = (at >= 0 && at < rows.length) ? String(rows[at].path) : ""
                        const text = JSON.stringify({ category: currentCategory, top: top })
                        if (String(SettingsService.values["session.showsReading"] ?? "") !== text)
                            SettingsService.setValue("session.showsReading", text)
                    }
                    function touchShowsPlace() {
                        if (SettingsService.ready) showsPlaceTimer.restart()
                    }
                    function restoreShowsPlace() {
                        if (media_table.placeRestored || !SettingsService.ready || table_scroll.count === 0)
                            return
                        // Read once for this build; a roster that loads later re-asks.
                        media_table.placeRestored = true
                        const raw = String(SettingsService.values["session.showsReading"] ?? "")
                        let state = null
                        if (raw !== "") {
                            try { state = JSON.parse(raw) } catch (e) { state = null }
                        }
                        if (state && typeof state === "object") {
                            const cat = Number(state.category)
                            if (!isNaN(cat) && cat !== media_table.currentCategory
                                    && (cat === 0 || cat === -1
                                        || (cat >= 1 && cat <= media_table.showCategories.length)))
                                media_table.currentCategory = cat
                        }
                        Qt.callLater(function () {
                            const rows = table_body.shownShows
                            const at = state
                                ? rows.findIndex((s) => String(s.path) === String(state.top)) : -1
                            if (at >= 0)
                                table_scroll.positionViewAtIndex(at, ListView.Beginning)
                            media_table.saveShowsPlace()
                        })
                    }
                    onCurrentCategoryChanged: media_table.touchShowsPlace()
                    Component.onCompleted: SettingsService.whenReady(media_table.restoreShowsPlace)
                    Timer {
                        id: showsPlaceTimer
                        interval: 350
                        onTriggered: media_table.saveShowsPlace()
                    }
                    property int renamingIndex: -1
                    property string deletingCategoryName: ""
                    property int deletingCategoryIndex: -1
                    property int menuIndex: -1
                    function openCatMenu(i, x, y) {
                        // A right-click while the Settings modal is up belongs to the
                        // MODAL, not to the list underneath it (see ModalStack).
                        if (ModalStack.settingsOpen)
                            return
                        closeShowMenu()
                        menuIndex = i
                        catContextMenu.openAt(media_table, x, y, boundsItem())
                    }
                    function closeCatMenu() { menuIndex = -1 }
                    function activateCatMenu(label) {
                        const i = menuIndex
                        catContextMenu.visible = false
                        if (i < 0)
                            return
                        if (label === qsTr("Rename"))
                            renamingIndex = i
                        else if (label === qsTr("Delete"))
                            removeCategory(i)
                    }

                    // Show row context menu (right-click) — Rename/Duplicate,
                    // move to a library category, or Delete.
                    property int showMenuIndex: -1
                    property var showMenuTarget: null
                    readonly property string showMoveLabel: qsTr("Move to")
                    property var showMoveRow: null
                    function openShowMenu(i, x, y) {
                        // A right-click while the Settings modal is up belongs to the
                        // MODAL, not to the list underneath it (see ModalStack).
                        if (ModalStack.settingsOpen)
                            return
                        closeCatMenu()
                        showMoveMenu.closeFlyout()
                        const shows = table_body.shownShows
                        showMenuIndex = i
                        showMenuTarget = (shows && i >= 0 && i < shows.length) ? shows[i] : null
                        showContextMenu.model = [
                            { label: qsTr("Rename") },
                            { label: qsTr("Duplicate") },
                            { divider: true },
                            { label: showMoveLabel, trailing: "\u203a" },
                            { divider: true },
                            { label: qsTr("Delete"), danger: true }
                        ]
                        showContextMenu.openAt(media_table, x, y, boundsItem())
                    }
                    function closeShowMenu() {
                        showMenuIndex = -1
                        showMenuTarget = null
                    }
                    function activateShowMenu(label) {
                        const show = showMenuTarget
                        if (label === showMoveLabel) {
                            if (!showMoveMenu.visible)
                                openShowMoveMenu(showMoveRow)
                            return
                        }
                        showContextMenu.visible = false
                        if (!show)
                            return
                        if (label === qsTr("Rename")) {
                            showRenameDialog.targetPath = show.path
                            showRenameDialog.open(show.name)
                        } else if (label === qsTr("Duplicate")) {
                            if (ShowService.duplicateShow(show.path))
                                ShowService.refreshLibrary()
                        } else if (label === qsTr("Delete")) {
                            showDeleteConfirm.targetPath = show.path
                            showDeleteConfirm.title = qsTr("Delete show")
                            showDeleteConfirm.message = qsTr("Delete “%1”? This can be recovered from the library's .deleted bin.").arg(show.name)
                            showDeleteConfirm.open()
                        }
                    }
                    function openShowMoveMenu(rowItem) {
                        const show = showMenuTarget
                        if (!rowItem || !show) return
                        showMoveRow = rowItem
                        const entry = (name, label) => ({
                            label: label, payload: name,
                            trailing: show.category === name ? "\u2713" : ""
                        })
                        showMoveMenu.model = [entry("", qsTr("Unlabeled"))]
                            .concat(showCategories.map((c) => entry(c.name, c.name)))
                        showMoveMenu.openBeside(rowItem, boundsItem())
                    }
                    // Clamp bounds for openAt: the window root when there is
                    // one (always, in the app), else this pane.
                    function boundsItem() { return Window.window ? Window.window.contentItem : media_table }

                    // Engine is the source of truth — no UI-side roster.
                    readonly property var showCategories: {
                        const cats = ShowService.libraryCategories
                        // Re-evaluate when the engine republishes the library.
                        libRev
                        const out = []
                        for (let i = 0; i < cats.length; ++i) {
                            const n = cats[i]
                            out.push({ name: n, count: ShowService.libraryShowsIn(n).length })
                        }
                        return out
                    }
                    property int libRev: 0
                    Connections {
                        target: ShowService
                        function onLibraryChanged() {
                            media_table.libRev++
                            // The roster can arrive (or be republished) after this pane
                            // was built — the remembered category is applied then.
                            Qt.callLater(media_table.restoreShowsPlace)
                        }
                    }

                    // ---- Category CRUD — straight through to the engine.
                    // The engine validates (no duplicates, no junk names) and
                    // republishes; failures toast from ShowService itself.
                    function addCategory(name) {
                        const trimmed = name.trim()
                        if (trimmed === "") return
                        if (ShowService.createLibraryCategory(trimmed)) {
                            const idx = showCategories.findIndex(c => c.name === trimmed)
                            if (idx >= 0) currentCategory = idx + 1
                        }
                    }
                    function renameCategory(i, name) {
                        const trimmed = name.trim()
                        const list = showCategories
                        if (trimmed === "" || i >= list.length || list[i].name === trimmed) {
                            renamingIndex = -1                 // empty = cancel
                            return
                        }
                        const wasSelected = currentCategory === i + 1
                        ShowService.renameLibraryCategory(list[i].name, trimmed)
                        renamingIndex = -1
                        if (wasSelected) {
                            const idx = showCategories.findIndex(c => c.name === trimmed)
                            if (idx >= 0)
                                currentCategory = idx + 1
                        }
                    }
                    function removeCategory(i) {
                        const list = showCategories
                        if (i < 0 || i >= list.length)
                            return
                        deletingCategoryName = list[i].name
                        deletingCategoryIndex = i
                        deleteCategoryConfirm.message = qsTr("Remove ‘%1’? Its %2 show(s) will be kept in Unlabeled.")
                            .arg(list[i].name).arg(list[i].count)
                        deleteCategoryConfirm.open()
                    }

                    LibrarySidebar {
                        id: categories_sidebar

                        visible: media_tab_bar.currentPane === "shows"
                        x: 0
                        y: 0

                        height: parent.height

                        Column {
                            x: 8
                            y: 8
                            width: parent.width - 16
                            spacing: 2

                            SidebarRow {
                                objectName: "selfTestShowsAll"
                                width: parent.width
                                height: 38
                                icon: "layoutDashboard"
                                label: qsTr("All")
                                count: String(ShowService.libraryShows.length)
                                selected: media_table.currentCategory === 0
                                onClicked: media_table.currentCategory = 0
                            }
                            SidebarRow {
                                objectName: "selfTestShowsUnlabeled"
                                width: parent.width
                                height: 38
                                icon: "layers"
                                label: qsTr("Unlabeled")
                                count: String(ShowService.libraryShowsIn("").length)
                                selected: media_table.currentCategory === -1
                                onClicked: media_table.currentCategory = -1
                            }

                            // ---- Categories section header + add ------
                            Item {
                                width: parent.width; height: 26
                                Text {
                                    x: 12; anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("CATEGORIES")
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily; font.pixelSize: 12; font.bold: true
                                }
                            }
                        }

                        // ---- Scrollable category rows --------------------
                        Flickable {
                            id: catScroll

                            x: 8
                            y: 120
                            width: parent.width - 16
                            height: Math.max(0, parent.height - y - 48)

                            clip: true
                            contentWidth: width
                            contentHeight: catRows.height
                            boundsBehavior: Flickable.StopAtBounds

                            onMovementStarted: media_table.closeCatMenu()

                            Column {
                                id: catRows
                                width: parent.width
                                spacing: 2

                                Repeater {
                                    model: media_table.showCategories
                                    delegate: Rectangle {
                                        required property var modelData
                                        required property int index
                                        readonly property bool selected: media_table.currentCategory === index + 1

                                        width: parent.width
                                        height: 38
                                        radius: 6
                                        color: selected ? Theme.activeBg
                                             : (catHover.hovered ? Theme.hoverBg : Theme.rowBg)
                                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                                        Rectangle {
                                            visible: parent.selected
                                            x: 0; y: 5; width: 3; height: 20
                                            color: Theme.accent; radius: 1.5
                                        }
                                        IconGlyph {
                                            x: 12
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 14; height: 14
                                            name: "folder"
                                            color: selected ? Theme.accent : Theme.textSecondary
                                        }
                                        Text {
                                            id: catNameText
                                            x: 34
                                            width: parent.width - 78
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: media_table.renamingIndex !== index
                                            text: modelData.name
                                            color: selected ? Theme.textPrimary : Theme.navLabelMuted
                                            elide: Text.ElideRight
                                            font.family: Theme.fontFamily; font.pixelSize: 15
                                        }
                                        // Inline rename editor — opened from the
                                        // right-click menu's Rename entry.
                                        TextInput {
                                            id: catNameEdit
                                            x: 34
                                            width: parent.width - 78
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: media_table.renamingIndex === index
                                            enabled: visible
                                            text: visible ? modelData.name : ""
                                            color: Theme.textPrimary
                                            font.family: Theme.fontFamily; font.pixelSize: 15
                                            clip: true
                                            onAccepted: media_table.renameCategory(index, text)
                                            onActiveFocusChanged: if (!activeFocus && visible)
                                                media_table.renameCategory(index, text)
                                            Keys.onEscapePressed: media_table.renamingIndex = -1
                                            onVisibleChanged: if (visible) {
                                                forceActiveFocus()
                                                selectAll()
                                            }
                                        }
                                        Text {
                                            anchors.right: parent.right
                                            anchors.rightMargin: 12
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: media_table.renamingIndex !== index
                                            text: String(modelData.count)
                                            color: Theme.textMuted
                                            font.family: Theme.fontFamily; font.pixelSize: 14
                                        }
                                        PositionHoverArea {
                                            id: catHover
                                            anchors.fill: parent
                                        }
                                        MouseArea {
                                            id: catRowMouse
                                            anchors.fill: parent
                                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            onClicked: (mouse) => {
                                                if (mouse.button === Qt.RightButton) {
                                                    // Context menu at the cursor —
                                                    // Rename / Delete (user call).
                                                    const p = catRowMouse.mapToItem(
                                                        media_table, mouse.x, mouse.y)
                                                    media_table.openCatMenu(index, p.x + 4, p.y - 4)
                                            } else {
                                                media_table.currentCategory = index + 1
                                            }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        AppScrollBar {
                            flickable: catScroll

                            x: parent.width - 12
                            y: 120
                            height: Math.max(0, parent.height - y - 48)
                        }

                        SidebarAddButton {
                            objectName: "selfTestShowsNewCategory"
                            x: 8
                            y: parent.height - 40
                            width: parent.width - 16
                            text: qsTr("New category")
                            onClicked: showCategoryDialog.open("")
                        }
                    }

                    // ---- Category context menu (right-click) ---- one
                    DropdownPanel {
                        id: catContextMenu

                        visible: false
                        z: 60
                        width: 148
                        model: [
                            { label: qsTr("Rename") },
                            { label: qsTr("Delete"), danger: true }
                        ]
                        onItemActivated: (label) => media_table.activateCatMenu(label)
                    }
                    MenuCatcher { menu: catContextMenu }
                    Connections {
                        target: catContextMenu
                        function onVisibleChanged() {
                            if (!catContextMenu.visible)
                                media_table.closeCatMenu()
                        }
                    }

                    // ---- Show row context menu (right-click) ---- Rename/
                    DropdownPanel {
                        id: showContextMenu

                        visible: false
                        z: 60
                        width: 160
                        fitContentWidth: true
                        maxHeight: 320
                        onItemHovered: (label, hovering, rowItem) => {
                            if (label === media_table.showMoveLabel) {
                                if (hovering) {
                                    showMoveMenuGrace.stop()
                                    media_table.openShowMoveMenu(rowItem)
                                } else {
                                    showMoveMenuGrace.restart()
                                }
                            } else if (hovering) {
                                showMoveMenu.closeFlyout()
                            }
                        }
                        onItemActivated: (label) => media_table.activateShowMenu(label)
                    }
                    MenuCatcher { menu: showContextMenu }
                    Timer {
                        id: showMoveMenuGrace
                        interval: 260
                        onTriggered: if (!AppCursor.hovered(showMoveMenu)
                                       && !AppCursor.hovered(media_table.showMoveRow))
                                         showMoveMenu.closeFlyout()
                    }
                    DropdownPanel {
                        id: showMoveMenu
                        objectName: "selfTestShowMoveMenu"
                        visible: showContextMenu.visible
                        z: 60
                        fitContentWidth: true
                        maxHeight: 320
                        onItemPicked: (index, payload) => {
                            const show = media_table.showMenuTarget
                            showMoveMenu.closeFlyout()
                            showContextMenu.visible = false
                            if (show)
                                ShowService.moveShowToCategory(show.path, payload === undefined ? "" : payload)
                        }
                    }
                    MenuCatcher { menu: showMoveMenu }
                    Connections {
                        target: showContextMenu
                        function onVisibleChanged() {
                            if (!showContextMenu.visible) {
                                showMoveMenu.closeFlyout()
                                media_table.closeShowMenu()
                            }
                        }
                    }

                    NameDialog {
                        id: showRenameDialog
                        anchors.fill: parent
                        z: 30000
                        property string targetPath: ""
                        title: qsTr("Rename show")
                        confirmLabel: qsTr("Rename")
                        onAccepted: (text) => {
                            if (showRenameDialog.targetPath && text.trim() !== "") {
                                ShowService.renameShow(showRenameDialog.targetPath, text.trim())
                                ShowService.refreshLibrary()
                            }
                        }
                    }

                    NameDialog {
                        id: showCategoryDialog
                        objectName: "selfTestCategoryNameDialog"
                        anchors.fill: parent
                        z: 30000
                        title: qsTr("New category")
                        placeholder: qsTr("Category name")
                        confirmLabel: qsTr("Create")
                        onAccepted: (text) => media_table.addCategory(text)
                    }

                    ConfirmDialog {
                        id: deleteCategoryConfirm
                        objectName: "selfTestDeleteShowCategoryConfirm"
                        anchors.fill: parent
                        z: 30000
                        title: qsTr("Remove category")
                        confirmLabel: qsTr("Remove")
                        confirmVariant: "danger"
                        onConfirmed: {
                            close()
                            const name = media_table.deletingCategoryName
                            const index = media_table.deletingCategoryIndex
                            if (name !== "" && ShowService.removeLibraryCategory(name)) {
                                if (media_table.currentCategory === index + 1)
                                    media_table.currentCategory = 0
                                else if (media_table.currentCategory > index + 1)
                                    media_table.currentCategory--
                                if (media_table.renamingIndex === index)
                                    media_table.renamingIndex = -1
                                else if (media_table.renamingIndex > index)
                                    media_table.renamingIndex--
                            }
                            media_table.deletingCategoryName = ""
                            media_table.deletingCategoryIndex = -1
                        }
                        onDismissed: {
                            close()
                            media_table.deletingCategoryName = ""
                            media_table.deletingCategoryIndex = -1
                        }
                    }

                    ConfirmDialog {
                        id: showDeleteConfirm
                        anchors.fill: parent
                        z: 30000
                        property string targetPath: ""
                        confirmLabel: qsTr("Delete")
                        confirmVariant: "danger"
                        onConfirmed: {
                            showDeleteConfirm.close()
                            if (showDeleteConfirm.targetPath) {
                                ShowService.deleteShow(showDeleteConfirm.targetPath)
                                ShowService.refreshLibrary()
                            }
                        }
                        onDismissed: showDeleteConfirm.close()
                    }

                    Rectangle {
                        id: table_body
                        objectName: "selfTestShowsRows"

                        property int showsRowBuilds: 0
                        property int showsRowDrops: 0
                        readonly property var probeEvals: ({ rows: 0 })

                        visible: media_tab_bar.currentPane === "shows"
                        x: categories_sidebar.width + 12
                        y: 8

                        height: parent.height - y
                        width: parent.width - categories_sidebar.width - 24 - shows_template_sidebar.width

                        clip: true
                        color: "transparent"

                        readonly property var shownShows: {
                            media_table.libRev   // re-read on engine republish
                            const cat = media_table.currentCategory
                            const inCategory = cat === 0 ? ShowService.libraryShows
                                : (cat === -1 ? ShowService.libraryShowsIn("")
                                   : ShowService.libraryShowsIn(media_table.showCategories[cat - 1].name))
                            // The tab bar's Shows search: the ENGINE's library search (name or category), best first.
                            const query = media_tab_bar.searches.shows !== undefined ? media_tab_bar.searches.shows.trim() : ""
                            if (query === "")
                                return inCategory
                            const wanted = ShowService.searchLibrary(query)
                            const inScope = {}
                            inCategory.forEach((s) => { inScope[s.path] = true })
                            return wanted.filter((s) => inScope[s.path] === true)
                        }

                        property int showCursor: -1
                        function moveShowCursor(direction) {
                            const rows = table_body.shownShows
                            if (!rows || rows.length === 0)
                                return
                            let at = table_body.showCursor
                            if (at < 0 || at >= rows.length) {
                                const cur = vGRPresenter_Main_Screen.centerItem
                                at = cur && cur.ref
                                   ? rows.findIndex((s) => s.path === cur.ref) : -1
                                if (at < 0)
                                    at = direction > 0 ? -1 : rows.length
                            }
                            const next = Math.max(0, Math.min(rows.length - 1, at + direction))
                            table_body.showCursor = next
                            const row = table_scroll.itemAtIndex(next)
                            const onScreen = row
                                && row.y >= table_scroll.contentY - 1
                                && row.y + row.height <= table_scroll.contentY + table_scroll.height + 1
                            if (!onScreen)
                                table_scroll.positionViewAtIndex(next, ListView.Center)
                        }
                        function openCursorShow() {
                            const rows = table_body.shownShows
                            const at = table_body.showCursor
                            if (!rows || at < 0 || at >= rows.length)
                                return
                            const show = rows[at]
                            vGRPresenter_Main_Screen.centerItem = {
                                type: "show", ref: show.path, name: show.name,
                                category: show.category ?? "", layout: ""
                            }
                        }

                        // ---- Shows rows — ENGINE-FED -------------------
                        ListView {
                            id: table_scroll
                            objectName: "selfTestShowsList"
                            anchors.fill: parent
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            reuseItems: true
                            model: table_body.shownShows
                            // The row at the top of the list is part of the remembered
                            // place, and the model landing is when it can be put back.
                            onMovementEnded: media_table.touchShowsPlace()
                            onCountChanged: {
                                table_body.showCursor = -1   // a different list: no cursor
                                media_table.restoreShowsPlace()
                            }
                            delegate: Rectangle {
                                required property var modelData
                                required property int index

                                // LAYOUT PROBE (VGR_LAYOUT_TEST).
                                Component.onCompleted: table_body.showsRowBuilds++
                                Component.onDestruction: table_body.showsRowDrops++

                                height: 32
                                width: ListView.view.width - 8

                                radius: 4
                                readonly property bool selectorSelected: index === table_body.showCursor
                                color: selectorSelected ? Theme.activeBg : ListRows.stripeOverlay(index)
                                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                                HoverGlow {
                                    anchors.fill: parent
                                    radius: parent.radius
                                    hovered: rowHover.hovered && !newShowDockHover.hovered
                                    glowColor: Theme.hoverBg
                                }
                                PositionHoverArea {
                                    id: rowHover
                                    anchors.fill: parent
                                    showCursor: false
                                }

                                IconGlyph {
                                    x: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "shows"
                                    color: rowHover.hovered || parent.selectorSelected
                                           ? Theme.accent : Theme.textMuted
                                    width: 15; height: 15
                                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                                }

                                Text {
                                    // Indented past the mark; the width gives back the same 8px it used to start at.
                                    x: 31
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 263 - (showCategoryLabel.visible ? showCategoryLabel.width + 12 : 0)
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily; font.pixelSize: 14
                                    text: { table_body.probeEvals.rows++; return modelData.name }   // (scroll-cost probe: the row's name binding)
                                    textFormat: Text.PlainText
                                }
                                // WHICH CATEGORY THE SHOW IS IN, in "All" only — inside a category (or
                                // Unlabeled) every row would repeat the sidebar's own selection.
                                Text {
                                    id: showCategoryLabel
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16 + 201 + 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 150
                                    visible: media_table.currentCategory === 0
                                             && String(modelData.category ?? "") !== ""
                                    color: Theme.textMuted
                                    elide: Text.ElideRight
                                    font.family: Theme.fontFamily; font.pixelSize: 13
                                    horizontalAlignment: Text.AlignRight
                                    text: String(modelData.category ?? "")
                                    textFormat: Text.PlainText
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 201
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily; font.pixelSize: 13
                                    horizontalAlignment: Text.AlignRight
                                    text: {
                                        SettingsService.localeRevision
                                        const d = new Date(modelData.modifiedMs)
                                        Qt.formatDate(d, "dddd d, MMMM yyyy")
                                    }
                                    textFormat: Text.PlainText
                                }
                                // Click: the show on the centre page. Double-click: into the open project.
                                // Right-click: Rename/Duplicate/Delete. Drag: into a project.
                                DragSource {
                                    id: rowMouse
                                    anchors.fill: parent
                                    payload: ({ kind: "show_drawer", items: [{ ref: modelData.path, name: modelData.name }] })
                                    label: modelData.name
                                    onActivated: vGRPresenter_Main_Screen.centerItem = { type: "show", ref: modelData.path, name: modelData.name, category: modelData.category ?? "", layout: "" }
                                    onOpened: {
                                        const showRef = String(modelData.path)
                                        const showName = String(modelData.name)
                                        ProjectService.dropOnProject("show_drawer", [{ ref: showRef, name: showName }])
                                        vGRPresenter_Main_Screen.centerItem = {
                                            type: "show", ref: showRef,
                                            name: showName, category: modelData.category ?? "", layout: ""
                                        }
                                        Qt.callLater(() => showCenter.stageFirstSlide())
                                    }
                                    onContextRequested: (x, y) => {
                                        const p = rowMouse.mapToItem(media_table, x, y)
                                        media_table.openShowMenu(index, p.x, p.y)
                                    }
                                }
                            }
                        }   // ListView table_scroll
                    }
                    AppScrollBar {
                        flickable: table_scroll
                        visible: table_body.visible
                        y: table_body.y + 2
                        height: table_body.height - 60   // 2 top + 58 to clear new_show_dock_btn
                        x: table_body.x + table_body.width + 4   // into the gutter, beside the sidebar
                    }
                    Rectangle {
                        id: new_show_dock_btn

                        visible: media_tab_bar.currentPane === "shows"
                        anchors.right: parent.right
                        // Clears the template column as well as its own margin (the
                        // container now spans the whole dock — see media_table).
                        anchors.rightMargin: 22 + shows_template_sidebar.width
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 16

                        height: 36
                        width: new_show_1.width + 32
                        radius: 18
                        color: newShowDockHover.hovered ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                        // the soft shadow (drawn first, so it sits under the pill)
                        Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 3; width: parent.width + 6; height: parent.height + 4; radius: 20; color: "#26000000" }
                        Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 2; height: parent.height + 2; radius: 19; color: "#33000000" }

                        PositionHoverArea {
                            id: newShowDockHover
                            anchors.fill: parent
                            onClicked: vGRPresenter_Main_Screen.newShowRequested()
                        }

                        Row {
                            id: new_show_1

                            anchors.centerIn: parent
                            spacing: 8

                            IconGlyph {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: 1
                                name: "newShow"
                                color: Theme.controlInk
                                strokeWidth: 2
                                width: 16; height: 16
                            }

                            Text {
                                id: new_show_1_label

                                anchors.verticalCenter: parent.verticalCenter

                                color: Theme.controlInk
                                font.family: Theme.fontFamily
                                font.pixelSize: 14
                                font.weight: Font.Bold
                                text: qsTr("New show")
                                textFormat: Text.PlainText
                            }
                        }
                    }   // new_show_dock_btn
                }   // media_table
                // ---- The clock, in the preview column ----------------------
                Rectangle {
                    visible: media_tab_bar.currentPane === "media"
                          || media_tab_bar.currentPane === "overlays"
                          || media_tab_bar.currentPane === "templates"
                          || media_tab_bar.currentPane === "soon"
                          || media_tab_bar.currentPane === "functions"
                    x: parent.width - width
                    y: 39
                    width: vGRPresenter_Main_Screen.templateColumnWidth
                    height: parent.height - 39
                    color: Theme.panelBg
                }
                ClockPanel {
                    id: dock_clock
                    visible: media_tab_bar.currentPane === "media"
                          || media_tab_bar.currentPane === "overlays"
                          || media_tab_bar.currentPane === "templates"
                          || media_tab_bar.currentPane === "soon"
                          || media_tab_bar.currentPane === "functions"
                    x: parent.width - width
                    y: 39
                    width: vGRPresenter_Main_Screen.templateColumnWidth
                    height: parent.height - 39
                    // The column is ~400 wide; the type follows it.
                    mainSize: Math.min(60, Math.round(width * 0.15))
                    running: visible
                }

                // ---- Shows template sidebar (Shows tab) ---- the right column
                Rectangle {
                    id: shows_template_sidebar

                    visible: media_tab_bar.currentPane === "shows"
                    x: parent.width - width
                    y: 39
                    width: vGRPresenter_Main_Screen.templateColumnWidth
                    height: parent.height - 39

                    color: Theme.panelBg

                    property int templateRevision: 0
                    Connections {
                        target: TemplateLibraryService
                        function onChanged() { shows_template_sidebar.templateRevision++ }
                    }

                    // The selected category's name ("" for All — the DEFAULT row).
                    readonly property string categoryName: media_table.currentCategory <= 0
                                                           ? "" : media_table.showCategories[media_table.currentCategory - 1].name
                    readonly property string categoryPicksKey: "session.showsCategoryTemplates"
                    readonly property var categoryPicks: {
                        const raw = SettingsService.values[categoryPicksKey]
                        if (raw === undefined || raw === null || String(raw) === "") return ({})
                        try {
                            const parsed = JSON.parse(String(raw))
                            return (parsed !== null && typeof parsed === "object") ? parsed : ({})
                        } catch (e) {
                            return ({})
                        }
                    }
                    readonly property string defaultKey: "shows.defaultTemplate"
                    readonly property string defaultTemplateId: String(SettingsService.values[defaultKey] ?? "")
                    readonly property string ownTemplateId: categoryName === ""
                                                            ? "" : String(categoryPicks[categoryName] ?? "")
                    readonly property bool inherited: categoryName !== "" && ownTemplateId === "" && defaultTemplateId !== ""
                    function setOwnTemplate(id) {
                        if (categoryName === "") {
                            SettingsService.setValue(defaultKey, id)
                            return
                        }
                        const next = {}
                        for (const k in categoryPicks)
                            if (k !== categoryName && String(categoryPicks[k]) !== "") next[k] = categoryPicks[k]
                        if (id !== "") next[categoryName] = id
                        SettingsService.setValue(categoryPicksKey, JSON.stringify(next))
                    }
                    readonly property string templateId: categoryName === "" ? defaultTemplateId
                                                         : (ownTemplateId !== "" ? ownTemplateId : defaultTemplateId)
                    readonly property var templateDesign: {
                        shows_template_sidebar.templateRevision
                        if (templateId === "") return null
                        const list = TemplateLibraryService.designs()
                        for (let i = 0; i < list.length; ++i)
                            if (String(list[i].id) === templateId) return list[i]
                        return null
                    }
                    readonly property string templateName: templateDesign ? String(templateDesign.name ?? "") : ""
                    property bool optionsOpen: false

                    function openTemplatePicker() {
                        const categoryNames = {}
                        for (const c of TemplateLibraryService.categories) categoryNames[c.id] = c.name
                        templatePicker.templates = TemplateLibraryService.designs().map((t) => ({
                            key: t.id, name: t.name, color: t.color,
                            category: t.category,
                            categoryName: t.category ? (categoryNames[t.category] ?? t.category) : qsTr("Unlabeled")
                        }))
                        templatePicker.selectedKey = shows_template_sidebar.templateId
                        templatePicker.open = true
                    }

                    readonly property var previewDesign: {
                        shows_template_sidebar.templateRevision
                        if (templateId === "") return null
                        try { return TemplateLibraryService.design(templateId) } catch (e) { return null }
                    }
                    readonly property bool hasPreviewBlocks: previewDesign !== null
                                                             && previewDesign.blocks !== undefined
                                                             && previewDesign.blocks.length > 0

                    Flickable {
                        id: previewFlick
                        anchors.top: parent.top; anchors.bottom: parent.bottom
                        anchors.left: parent.left; anchors.right: parent.right
                        contentWidth: width
                        contentHeight: previewContent.height
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds

                        Column {
                            id: previewContent
                            width: previewFlick.width

                            // ---- the template's slide preview: fixed size, driven by width alone
                            // (ReferencePane's previewTile: never squeezed, the column scrolls instead) ----
                            Item {
                                width: parent.width
                                height: previewTile.height + 28

                                Rectangle {
                                    id: previewTile
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 20
                                    width: parent.width - 32
                                    height: StageGrid.heightForWidth(width)   // the stage's own ratio, purely from width
                                    color: "#000000"
                                    clip: true

                                    DesignPreview {
                                        anchors.fill: parent
                                        visible: shows_template_sidebar.hasPreviewBlocks
                                        blocks: shows_template_sidebar.hasPreviewBlocks ? shows_template_sidebar.previewDesign.blocks : []
                                        background: shows_template_sidebar.hasPreviewBlocks
                                                    && shows_template_sidebar.previewDesign.background !== undefined
                                                    ? shows_template_sidebar.previewDesign.background : "#000000"
                                    }
                                    Text {
                                        visible: !shows_template_sidebar.hasPreviewBlocks
                                        anchors.centerIn: parent
                                        text: shows_template_sidebar.categoryName === ""
                                              ? qsTr("No default template")
                                              : qsTr("No template for %1").arg(shows_template_sidebar.categoryName)
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily; font.pixelSize: 14
                                    }
                                }
                            }

                            // ---- the template controls — ReferencePane's exact row ----
                            Column {
                                visible: !shows_template_sidebar.optionsOpen
                                x: 16; width: parent.width - 32
                                spacing: 10

                                Rectangle {
                                    width: parent.width; height: 56; radius: 6
                                    color: Theme.windowBg; border.color: Theme.border   // Theme.border
                                    clip: true

                                    Column {
                                        x: 12; anchors.verticalCenter: parent.verticalCenter
                                        spacing: 2
                                        Text { text: qsTr("Template"); color: Theme.accent; font.family: Theme.fontFamily; font.pixelSize: 13 }
                                        Text {
                                            width: 150
                                            text: shows_template_sidebar.templateId === ""
                                                  ? qsTr("Default — no template")
                                                  : (shows_template_sidebar.templateName !== "" ? shows_template_sidebar.templateName : qsTr("Missing template"))
                                            color: shows_template_sidebar.templateId === "" ? Theme.textMuted : Theme.textPrimary
                                            elide: Text.ElideRight
                                            font.family: Theme.fontFamily; font.pixelSize: 16; font.weight: Font.DemiBold
                                        }
                                    }

                                    Row {
                                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                        spacing: 0

                                        Item {
                                            visible: shows_template_sidebar.categoryName === ""
                                                     ? shows_template_sidebar.templateId !== ""
                                                     : shows_template_sidebar.ownTemplateId !== ""
                                            width: 36; height: 56
                                            IconGlyph { anchors.centerIn: parent; name: "close"; color: clearHover.hovered ? Theme.textPrimary : Theme.textSecondary; width: 10; height: 10 }
                                            HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                                            TapHandler { onTapped: shows_template_sidebar.setOwnTemplate("") }
                                        }
                                        // choose another template (the shared picker popup)
                                        Item {
                                            width: 36; height: 56
                                            IconGlyph { anchors.centerIn: parent; name: "layoutTemplate"; color: pickHover.hovered ? Theme.textPrimary : Theme.textSecondary; width: 14; height: 14 }
                                            HoverHandler { id: pickHover; cursorShape: Qt.PointingHandCursor }
                                            TapHandler { onTapped: shows_template_sidebar.openTemplatePicker() }
                                        }
                                        // edit it on the Edit screen
                                        Rectangle {
                                            width: 46; height: 56; color: editHover.hovered ? Theme.hoverBg : Theme.windowBg
                                            IconGlyph { anchors.centerIn: parent; name: "penTool"; color: Theme.textPrimary; width: 14; height: 14 }
                                            HoverHandler { id: editHover; cursorShape: Qt.PointingHandCursor }
                                            TapHandler {
                                                onTapped: if (shows_template_sidebar.templateId !== "")
                                                              vGRPresenter_Main_Screen.designEditRequested("template", shows_template_sidebar.templateId)
                                            }
                                        }
                                    }
                                }

                                // The fallback-rule note — ReferencePane's pattern of
                                // a caption under the row (there: the old-templates note).
                                Text {
                                    width: parent.width
                                    text: shows_template_sidebar.categoryName === ""
                                          ? qsTr("The default — every category without its own pick uses this one.")
                                          : (shows_template_sidebar.inherited
                                             ? qsTr("Inherited from All — choose one to give \"%1\" its own.").arg(shows_template_sidebar.categoryName)
                                             : qsTr("\"%1\"'s own template.").arg(shows_template_sidebar.categoryName))
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily; font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }

                            // ---- the slide options (the round tune button turns this on) —
                            component ShowsOptionToggle: Item {
                                id: opt
                                property string settingKey: ""
                                width: parent.width; height: 40
                                readonly property var def: SettingsService.definitions[opt.settingKey]
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.borderSubtle }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: opt.def ? opt.def.label : ""
                                    color: Theme.textPrimary   // Theme.textPrimary
                                    font.family: Theme.fontFamily; font.pixelSize: 14
                                }
                                SettingsToggle {
                                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                    checked: SettingsService.values[opt.settingKey] === true
                                    onToggled: SettingsService.setValue(opt.settingKey, !(SettingsService.values[opt.settingKey] === true))
                                }
                            }
                            component ShowsOptionNumber: Item {
                                id: num
                                property string settingKey: ""
                                property int step: 1
                                width: parent.width; height: 40
                                readonly property var def: SettingsService.definitions[num.settingKey]
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.borderSubtle }
                                function nudge(delta) {
                                    const next = Math.max(num.def.min, Math.min(num.def.max, Number(SettingsService.values[num.settingKey]) + delta))
                                    if (next !== SettingsService.values[num.settingKey])
                                        SettingsService.setValue(num.settingKey, next)
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: num.def ? num.def.label : ""
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily; font.pixelSize: 14
                                }
                                Row {
                                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                    spacing: 6
                                    Rectangle {
                                        width: 24; height: 24; radius: 6; color: minusHover.hovered ? Theme.hoverBg : Theme.rowBg; border.color: Theme.border
                                        Text { anchors.centerIn: parent; text: "\u2212"; color: Theme.textPrimary; font.pixelSize: 15 }
                                        HoverHandler { id: minusHover; cursorShape: Qt.PointingHandCursor }
                                        TapHandler { onTapped: num.nudge(-num.step) }
                                    }
                                    Text {
                                        width: 40; horizontalAlignment: Text.AlignHCenter; anchors.verticalCenter: parent.verticalCenter
                                        text: SettingsService.values[num.settingKey] ?? ""
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily; font.pixelSize: 14
                                    }
                                    Rectangle {
                                        width: 24; height: 24; radius: 6; color: plusHover.hovered ? Theme.hoverBg : Theme.rowBg; border.color: Theme.border
                                        Text { anchors.centerIn: parent; text: "+"; color: Theme.textPrimary; font.pixelSize: 15 }
                                        HoverHandler { id: plusHover; cursorShape: Qt.PointingHandCursor }
                                        TapHandler { onTapped: num.nudge(num.step) }
                                    }
                                }
                            }

                            Column {
                                visible: shows_template_sidebar.optionsOpen
                                x: 16; width: parent.width - 32
                                spacing: 0

                                Text {
                                    x: 6; height: 26; verticalAlignment: Text.AlignVCenter
                                    text: qsTr("SLIDE OPTIONS")
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.DemiBold; font.letterSpacing: 1.2
                                }
                                Rectangle {
                                    width: parent.width; height: showsOptionsColumn.height + 8
                                    radius: 8; color: Theme.windowBg; border.color: Theme.border   // Theme.border

                                    Column {
                                        id: showsOptionsColumn
                                        x: 14; y: 4; width: parent.width - 28
                                        spacing: 0

                                        ShowsOptionToggle { settingKey: "shows.verseNumbers" }
                                        ShowsOptionToggle { settingKey: "shows.splitLongVerses" }
                                        Column {
                                            visible: SettingsService.values["shows.splitLongVerses"] === true
                                            width: parent.width
                                            ShowsOptionNumber { settingKey: "shows.wordsPerSlide"; step: 5 }
                                        }
                                    }
                                }
                            }

                            // ---- the footer: the round tune button (ReferencePane's, minus
                            // Convert-to-show — the shows sidebar has no picked content to
                            // convert; its template applies to shows created in the category) ----
                            Item {
                                x: 16; width: parent.width - 32; height: 48 + 12

                                Item {
                                    id: optionsButton
                                    anchors.right: parent.right; y: 0
                                    width: 48; height: 48
                                    Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 3; width: parent.width + 6; height: width; radius: width / 2; color: "#26000000" }
                                    Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 2; height: width; radius: width / 2; color: "#33000000" }
                                    Rectangle {
                                        anchors.fill: parent; radius: width / 2
                                        gradient: Gradient {
                                            GradientStop { position: 0.0; color: shows_template_sidebar.optionsOpen ? (optionsHover.hovered ? Theme.accentLight : Theme.accent) : (optionsHover.hovered ? Theme.hoverBg : Theme.rowBg) }
                                            GradientStop { position: 1.0; color: shows_template_sidebar.optionsOpen ? Qt.darker(Theme.accent, 1.2) : Theme.rowBg }
                                        }
                                        border.width: 1
                                        border.color: shows_template_sidebar.optionsOpen ? Theme.dangerLight : Theme.borderSubtle
                                        IconGlyph { anchors.centerIn: parent; name: "sliders"; color: shows_template_sidebar.optionsOpen ? Theme.controlInk : Theme.textPrimary; fit: true; width: 22; height: 22 }
                                    }
                                    HoverHandler { id: optionsHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: shows_template_sidebar.optionsOpen = !shows_template_sidebar.optionsOpen }
                                }
                            }
                        }
                    }

                    // The shared scrollbar — ReferencePane's previewScrollBar over
                    // the same-shaped Flickable.
                    AppScrollBar {
                        id: optionsScrollBar
                        flickable: previewFlick
                        anchors.top: parent.top; anchors.bottom: parent.bottom; anchors.right: parent.right
                        anchors.topMargin: 4; anchors.bottomMargin: 4; anchors.rightMargin: 3
                    }

                    TemplatePickerModal {
                        id: templatePicker
                        z: 30000
                        contentType: "shows"
                        contentTypeLabel: qsTr("All")
                        onApplied: (tpl) => {
                            templatePicker.open = false
                            shows_template_sidebar.setOwnTemplate(tpl.key)
                        }
                        onCancelled: templatePicker.open = false
                    }
                }

                SplitHandle {
                    visible: media_tab_bar.currentPane === "shows"
                          || media_tab_bar.currentPane === "media"
                          || media_tab_bar.currentPane === "overlays"
                          || media_tab_bar.currentPane === "templates"
                          || media_tab_bar.currentPane === "soon"
                          || media_tab_bar.currentPane === "functions"
                    value: parent.width - shows_template_sidebar.width
                    minValue: parent.width - Math.min(560, Math.max(220, parent.width - 360))
                    maxValue: parent.width - 220
                    y: 39
                    height: parent.height - 39
                    onDragged: (v) => vGRPresenter_Main_Screen.templateColumnOverride = parent.width - v
                    onResetRequested: vGRPresenter_Main_Screen.templateColumnOverride = -1
                }
            }
        Rectangle {
            id: right_column

            x: parent.width - vGRPresenter_Main_Screen.outputsColumnWidth

            height: workspace_body.topHeight
            width: vGRPresenter_Main_Screen.outputsColumnWidth

            border.color: Theme.border
            border.width: 1
            color: Theme.workspacePaneBg    // ---- Monitor wall — the shared MonitorWall component
            MonitorWall {
                id: monitorWall
                // Self-test handle (VGR_SELFTEST grabs this item to PNG for
                // pixel-sampling the checkerboard/style-bg rendering).
                objectName: "selfTestMonitorWall"
                // The tab being looked at decides which Scroll / Slide choice the wall shows (see MonitorWall.activePane).
                activePane: media_tab_bar.currentPane

                x: 0
                y: 0
                width: parent.width
            }

            Rectangle {
                id: line_2

                y: monitorWall.y + monitorWall.height + (monitorWall.pageCount > 1 ? 22 : 11)
                width: parent.width
                height: 1
                color: Theme.border   // Theme.border
            }
        }

        SplitHandle {
            // Harness hook: the split-drag scenario grabs this strip and drags it, to
            // prove a sidebar width is actually REMEMBERED (session.projectsColumnWidth).
            objectName: "selfTestProjectSplit"
            value: left_column.width
            minValue: 160
            maxValue: vGRPresenter_Main_Screen.projectsColumnMaxWidth
            height: workspace_body.topHeight
            onDragged: (v) => vGRPresenter_Main_Screen.projectsColumnOverride = v
            onResetRequested: vGRPresenter_Main_Screen.projectsColumnOverride = -1
        }

        // ---- Divider between the top region and the library dock: drag to resize, double-click
        // for the default (the same SplitHandle every sidebar uses).
        SplitHandle {
            vertical: false
            value: workspace_body.topHeight
            minValue: workspace_body.minTop
            maxValue: workspace_body.maxTop
            onDragged: (v) => { workspace_body.topOverride = v; saveColumnWidthsTimer.restart() }
            onResetRequested: { workspace_body.topOverride = -1; saveColumnWidthsTimer.restart() }
        }

        SplitHandle {
            value: workspace_body.width - right_column.width
            minValue: left_column.width + 400
            maxValue: workspace_body.width - 240
            height: workspace_body.topHeight
            onDragged: (v) => vGRPresenter_Main_Screen.outputsColumnOverride = workspace_body.width - v
            onResetRequested: vGRPresenter_Main_Screen.outputsColumnOverride = -1
        }
    }
}