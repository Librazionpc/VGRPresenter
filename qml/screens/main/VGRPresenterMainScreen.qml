import QtQuick
import QtQuick.Shapes
import VGRPresenterUI
import "../../components"

Rectangle {
    id: vGRPresenter_Main_Screen

    Component.onCompleted: EngineBridge.log("info", "StartupTrace", "main screen component completed")
    onVisibleChanged: if (visible) EngineBridge.log("info", "StartupTrace", "main screen became visible")

    height: 900
    width: 1440

    border.color: Theme.border
    border.width: 1
    clip: true
    color: Theme.windowBg

    // Emitted by BOTH "New show" buttons (the clock panel's primary CTA
    // and the dock footer's red one) — Main.qml funnels it into
    // EditScreen.newShow(), the single place the reset lives. The two
    // buttons were dead Figma-export chrome: no MouseArea, no action —
    // rendered clickable and weren't.
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
    // What the centre page shows (FreeShow's activeShow): a project item, or a show clicked in the Shows table. null = the splash.
    property var centerItem: null
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
    // The app-wide search jumps straight to a sermon: switch to The Table and open
    // the hit AT its paragraph (goTo). The result's title is a reference label
    // ("1953 12 · ¶3") — pasted into the pane's citation search it matches nothing
    // (the citation line reads "47-0412 - Faith Is The Substance"), which is the
    // "quick search lands on an empty tab" bug.
    function openSermonAt(bookId, chapter, verse) {
        showInLibrary("table", "")          // the tab + a cleared query
        tablePane.clearCitationMatches()    // (a stale filter list must not cover the sermon)
        tablePane.goTo({ bookId: bookId, chapter: chapter, verseStart: verse, verseEnd: verse })
    }
    // The app-wide search jumps straight to a Bible verse: switch to the Scripture
    // tab and open the passage there (goTo selects the verse and scrolls to it).
    // The hit carries its bibleId (which translation) + bookId/chapter/verse; a
    // reference-pick result carries only the reference string (resolve it).
    function openVerseAt(r) {
        showInLibrary("scripture", "")      // the tab + a cleared query
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

        // Fills the window under the header (it was a fixed 1440 x 852 design). The side
        // columns keep their widths; the middle column and the dock take the rest, and the top
        // region (slate / monitors) is the same 54% of the height it was designed with.
        height: parent.height - 48
        width: parent.width
        // Where the top region ends and the library dock begins. Drag the splitter on that
        // edge to change it (topOverride, in px; -1 = the default 54%, restored by double-click);
        // it can't leave room for less than the top region's content or the dock's tab bar.
        property real topOverride: -1
        readonly property real minTop: 320
        readonly property real maxTop: Math.max(minTop, height - 220)
        readonly property real topHeight: Math.max(minTop, Math.min(maxTop,
            topOverride > 0 ? topOverride : Math.max(460, Math.round(height * 0.54))))

        color: "transparent"

        Rectangle {
            id: left_column

            height: parent.height
            width: 280

            border.color: Theme.border
            border.width: 1
            color: Theme.panelBg

            Rectangle {
                id: projects_section

                height: workspace_body.topHeight
                width: 280

                color: "transparent"


                // The real Projects panel: the engine's folders and projects, the open project's items, drag and drop.
                ProjectsPanel {
                    id: projects_list
                    objectName: "selfTestProjectsPanel"

                    x: 12
                    y: 10

                    height: workspace_body.topHeight - 22
                    width: 256

                    onItemActivated: (item) => vGRPresenter_Main_Screen.centerItem = item
                    // Double-click here used to jump straight to Edit — the
                    // same "everything else lands on the centre preview,
                    // only a right-click edits" rule this session settled on
                    // for the show library applies to the project sidebar
                    // too, so double-click is now a no-op beyond the single
                    // click's own centerItem (edit still reachable via the
                    // centre preview's own right-click / Edit tab).
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

            x: 280

            height: parent.height
            width: parent.width - 280 - 400

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

                        // The brand two-tone, matching the boot splash and the
                        // menu bar's lockup: VGR WHITE (heavy) + Presenter GREY —
                        // the whole wordmark was single purple before.
                        Text {
                            height: 60
                            color: Theme.textPrimary
                            font.family: "Outfit"
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
                            font.family: "Outfit"
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
                        font.family: "Segoe UI"
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
                    width: 520

                    color: "transparent"

                    Text {
                        id: for_everything_there_is_an_appointed_time_and_an

                        height: 40
                        width: 521

                        color: Theme.textSecondary
                        font.family: "Segoe UI"
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

                        border.color: actionQuickSearchHover.hovered ? Theme.borderSubtle : Theme.border
                        border.width: 1
                        color: actionQuickSearchMouse.pressed ? Theme.activeBg
                             : (actionQuickSearchHover.hovered ? Theme.hoverBg : Theme.rowBg)
                        radius: 8
                        Behavior on border.color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }
                        Behavior on color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }

                        PositionHoverArea {
                            id: actionQuickSearchHover
                            anchors.fill: parent
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

                            // The quick-search magnifier now comes from the shared
                            // IconGlyph bank (name "search") rather than an inline
                            // Shape, so the one source owns every icon.
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
                            font.family: "Segoe UI"
                            font.pixelSize: 15
                            font.weight: Font.Medium
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("Quick search")
                            textFormat: Text.PlainText
                            // Optical centering: the icon container is centered in the
                            // 36px row; a top-aligned 15px Segoe line box put the glyphs
                            // ~3px low (user call). VCenter centers the LINE BOX in the
                            // item, which recenters the glyphs on the icon.
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

                        border.color: actionNewProjectHover.hovered ? Theme.borderSubtle : Theme.border
                        border.width: 1
                        color: actionNewProjectMouse.pressed ? Theme.activeBg
                             : (actionNewProjectHover.hovered ? Theme.hoverBg : Theme.rowBg)
                        radius: 8
                        Behavior on border.color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }
                        Behavior on color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }

                        PositionHoverArea {
                            id: actionNewProjectHover
                            anchors.fill: parent
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
                            font.family: "Segoe UI"
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

                        border.color: actionNewShowHover.hovered ? Theme.borderSubtle : Theme.border
                        border.width: 1
                        color: actionNewShowMouse.pressed ? Theme.activeBg
                             : (actionNewShowHover.hovered ? Theme.hoverBg : Theme.rowBg)
                        radius: 8
                        Behavior on border.color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }
                        Behavior on color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }

                        PositionHoverArea {
                            id: actionNewShowHover
                            anchors.fill: parent
                        }

                        MouseArea {
                            id: actionNewShowMouse
                            anchors.fill: parent
                            onClicked: vGRPresenter_Main_Screen.newShowRequested()
                        }

                        // Same fixed x-offset convention as Quick search/New
                        // project above (icon x=20, label x=42, chevron
                        // x=208) — this row previously centered its own
                        // icon+label+chevron Row as a group instead, which
                        // put its icon/label at a DIFFERENT x than its two
                        // siblings (the button is wider than that group),
                        // reading as visibly misaligned against them.
                        Rectangle {
                            id: new_show_glyph

                            x: 20
                            y: 11

                            height: 14
                            width: 14

                            clip: true
                            color: "transparent"

                            // The app's own New-show mark (IconGlyph "newShow", from
                            // qml/assets/new show add.svg — the shows cards with a plus), not a
                            // bare "+": creating a show reads as a show action, where a plain
                            // plus could have meant anything. This one asset covers every
                            // button of the title "New show" — the "New project" row above keeps
                            // its own plus, since a project is not a show. The glyph centres
                            // itself in this 14px box.
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
                            font.family: "Segoe UI"
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
                    objectName: "selfTestShowCenter"
                    anchors.fill: parent
                    visible: vGRPresenter_Main_Screen.centerItem !== null
                    item: vGRPresenter_Main_Screen.centerItem
                    onEditRequested: (path) => vGRPresenter_Main_Screen.openShowRequested(path)
                    onCloseRequested: vGRPresenter_Main_Screen.centerItem = null
                }
            }
        }
            Rectangle {
                id: media_resource_dock

                // Full-width bottom band (x=0 → window right edge) — the FreeShow
                // samples: the library tabs + shows table start at the EXTREME
                // LEFT and run the whole width, with the clock living inside
                // the band at the right. Sits directly under the top region
                // (slate / monitors), bottom edge at 852.
                x: 0
                y: workspace_body.topHeight

                height: parent.height - workspace_body.topHeight
                width: parent.width

                color: Theme.panelBg

                Rectangle {
                    id: dock_tab_bar

                    height: 39
                    // Full width (the FreeShow sample): the tab pack hugs the
                    // left edge and Search sits at the window's right edge —
                    // the clock panel below the row no longer dictates the
                    // bar's extent.
                    width: parent.width

                    border.color: Theme.border
                    border.width: 1
                    color: "transparent"

                    // ---- Live tab bar ---- one shared component
                    // (qml/components/LibraryTabBar.qml): all nine tabs —
                    // including the new Media and THE TABLE (the sermon
                    // app, riding the Scripture-architecture browser via
                    // its JSON export) — share ONE delegate, so selected
                    // underline, hover and metrics are identical by
                    // construction.
                    LibraryTabBar {
                        id: media_tab_bar
                        x: 0
                        y: 0
                        width: parent.width
                        height: 39
                        // A picked suggestion row (a sermon from The Table's multi-match
                        // dropdown, a book from Scripture's) routes back to the pane that
                        // offered it — the pane jumps to the row's ref. (The signal existed
                        // but nothing consumed it: a dropdown pick did nothing.)
                        onSuggestionPicked: (paneKey, ref) => {
                            if (paneKey === "scripture")
                                scripturePane.applySuggestion(ref)
                            else if (paneKey === "table")
                                tablePane.applySuggestion(ref)
                        }
                        // Hovering a suggestion row previews its content in the pane's
                        // verses column until the pointer leaves (peekSuggestion(-1)).
                        onSuggestionHovered: (paneKey, index, hovering) => {
                            if (paneKey === "scripture")
                                scripturePane.peekSuggestion(hovering ? index : -1)
                            else if (paneKey === "table")
                                tablePane.peekSuggestion(hovering ? index : -1)
                        }
                    }
                }
                Rectangle {
                    id: media_table

                    objectName: "selfTestShowsTable"

                    y: 39

                    // Tracks the dock's taller body so every pane (shows
                    // table, browsers, media grid) fills to the bottom.
                    height: parent.height - 39
                    // The Scripture-architecture tabs (Scripture and The
                    // Table) take the clock's place: their preview /
                    // template column sits at the far right, the way
                    // FreeShow's does, instead of being squeezed in beside
                    // the clock. Every other pane leaves the clock its 400px.
                    width: (media_tab_bar.currentPane === "scripture" || media_tab_bar.currentPane === "table") ? parent.width : parent.width - 400

                    clip: true
                    color: "transparent"

                    // ---- Per-tab panes ---- Shows keeps the original table
                    // (gated below); Scripture and The Table share the
                    // Scripture-architecture browser (ReferencePane, shared by both tabs).
                    // BOTH PANES OWN NO DATA: the engine drives all library
                    // content — the bridge pushes each library's document
                    // into setLibrary() once wired; until then they show the
                    // "Waiting for engine…" shell. Media binds the Settings
                    // rosters (buses/video/audio) + playlists; everything
                    // else gets the consistent coming-soon pane until its
                    // content lands.
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
                        // The preview toolbar's ‹ › at the edge of the on-air
                        // set: this tab re-picks the neighbouring passage and
                        // replays it (its own stepAndPlay path) when ITS content
                        // is the one on air.
                        Connections {
                            target: LiveOutputService
                            function onPassageStepRequested(direction) {
                                if (scripturePane.visible && scripturePane.adapter.liveIsOurs(scripturePane.referenceText))
                                    scripturePane.stepAndPlay(direction)
                            }
                        }
                    }
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
                                if (tablePane.visible && tablePane.adapter.liveIsOurs(tablePane.referenceText))
                                    tablePane.stepAndPlay(direction)
                            }
                        }
                        // The same show-building path Scripture uses (Main.qml's
                        // handler is generic: name + slides -> a new show).
                        onConvertToShowRequested: (name, slides) => vGRPresenter_Main_Screen.scriptureShowRequested(name, slides)
                        // The template card's edit pencil opens it in the Edit screen.
                        onTemplateEditRequested: (id) => vGRPresenter_Main_Screen.designEditRequested("template", id)
                    }
                    MediaLibraryPane {
                        id: mediaPane
                        objectName: "selfTestMediaPane"
                        onVisibleChanged: if (visible) EngineBridge.log("info", "StartupTrace", "Media pane became visible")
                        Component.onCompleted: EngineBridge.log("info", "StartupTrace", "Media pane component completed")
                        onItemActivated: (item) => vGRPresenter_Main_Screen.centerItem = item
                        // Double-click: drop it on the project (unchanged)
                        // AND take it to the Main Output — the SAME toggle
                        // the tile's own hover TAKE pill fires, so
                        // double-click is a shortcut for it instead of a
                        // dead gesture (it used to only drop on the
                        // project, which is invisible with none open —
                        // "double click does nothing" from the Media grid).
                        onItemOpened: (item) => {
                            ProjectService.dropOnProject("media", [{ ref: item.ref, name: item.name }])
                            if (LiveOutputService.mediaOnAir && LiveOutputService.mediaPath === item.ref)
                                LiveOutputService.clearMedia()
                            else
                                LiveOutputService.takeMedia(item.ref, item.name)
                        }
                        visible: media_tab_bar.currentPane === "media"
                        filter: media_tab_bar.searches.media !== undefined ? media_tab_bar.searches.media : ""
                        width: parent.width; height: parent.height
                    }
                    DesignLibraryPane {
                        objectName: "selfTestOverlaysPane"
                        visible: media_tab_bar.currentPane === "overlays"
                        service: OverlayLibraryService
                        noun: "overlay"
                        onDesignActivated: (id, name) => vGRPresenter_Main_Screen.centerItem = { type: "overlay", ref: id, name: name }
                        // Double-click: drop it on the project (unchanged)
                        // AND toggle it on the Main Output — multiple
                        // overlays can be live at once, stacked in take
                        // order (LiveOutputService.activeOverlays); a
                        // second double-click on an already-live one takes
                        // it back off, same re-click-to-release convention
                        // media/input use.
                        onDesignOpened: (id, name) => {
                            ProjectService.dropOnProject("overlay", [{ ref: id, name: name }])
                            if (LiveOutputService.overlayIsOnAir(id))
                                LiveOutputService.clearOverlay(id)
                            else
                                LiveOutputService.takeOverlay(id, name)
                        }
                        onDesignOpenRequested: (id) => vGRPresenter_Main_Screen.designEditRequested("overlay", id)
                        filter: media_tab_bar.searches.overlays !== undefined ? media_tab_bar.searches.overlays : ""
                        width: parent.width; height: parent.height
                    }
                    DesignLibraryPane {
                        objectName: "selfTestTemplatesPane"
                        visible: media_tab_bar.currentPane === "templates"
                        service: TemplateLibraryService
                        noun: "template"
                        onDesignOpenRequested: (id) => vGRPresenter_Main_Screen.designEditRequested("template", id)
                        filter: media_tab_bar.searches.templates !== undefined ? media_tab_bar.searches.templates : ""
                        width: parent.width; height: parent.height
                    }
                    LibraryComingSoonPane {
                        objectName: "selfTestSoonPane"
                        visible: media_tab_bar.currentPane === "soon"
                        width: parent.width; height: parent.height
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
                        width: parent.width; height: parent.height
                    }

                    // ---- Categories sidebar (Shows tab) ---- the sample's
                    // left rail: "All" row, Categories section, category
                    // rows with counts, and the round + that creates new
                    // ones. ENGINE-FED: the roster comes from
                    // ShowService.libraryCategories (real sub-folders of the
                    // show library — persisted across restarts); every CRUD
                    // call goes straight to the engine, which validates and
                    // republishes. Counts are the engine's shows-per-category.
                    property int currentCategory: 0   // 0 = All, -1 = Unlabeled, 1..n = category
                    // The row being renamed inline (-1 = none). Opened from
                    // the right-click context menu; Enter/focus-loss commits,
                    // Escape cancels.
                    property int renamingIndex: -1
                    // Right-click context menu state — which row opened it
                    // (-1 = closed). One shared menu instance lives at the
                    // pane level (below), positioned over the clicked row.
                    property int menuIndex: -1
                    function openCatMenu(i, x, y) {
                        // Only one popup menu at a time — opening this one
                        // closes whatever else was open (right-clicking a
                        // second row used to leave both menus up at once).
                        closeShowMenu()
                        menuIndex = i
                        // DropdownPanel.openAt lifts the panel to the window
                        // root and clamps it to `bounds`, so it can neither be
                        // clipped by this pane nor land off-screen near an
                        // edge — the hand-rolled menu clamped against the
                        // pane's own rect by hand, and being parented inside
                        // the pane is also what kept its dismiss layer from
                        // covering the rest of the window.
                        catContextMenu.openAt(media_table, x, y, boundsItem())
                    }
                    function closeCatMenu() { menuIndex = -1 }
                    // Picked from the panel's model (by label), so the row it
                    // acts on is read FIRST — hiding the panel runs
                    // closeCatMenu() through the panel's own visibleChanged.
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
                    // The show the open menu acts on, CAPTURED WHEN IT OPENS.
                    // The menu used to re-read it at click time through the
                    // table's filtered list; capturing it means the action
                    // always lands on the row you actually right-clicked, and
                    // cannot be disturbed by the list rebuilding underneath.
                    property var showMenuTarget: null
                    readonly property string showMoveLabel: qsTr("Move to")
                    property var showMoveRow: null
                    function openShowMenu(i, x, y) {
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
                    // Bumped after every engine CRUD call so the binding above
                    // (an engine list the engine doesn't NOTIFY per-call beyond
                    // its own libraryChanged) re-reads fresh counts.
                    property int libRev: 0
                    Connections {
                        target: ShowService
                        function onLibraryChanged() { media_table.libRev++ }
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
                        // Categories come back case-insensitively SORTED, so a
                        // rename can reshuffle rows — remember whether this row
                        // was selected and re-point the selection by name after
                        // the engine republishes.
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
                        if (currentCategory === i + 1)
                            currentCategory = 0            // deleted selection → All
                        else if (currentCategory > i + 1)
                            currentCategory--
                        if (renamingIndex === i)
                            renamingIndex = -1
                        else if (renamingIndex > i)
                            renamingIndex--
                        ShowService.removeLibraryCategory(list[i].name)
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
                        // When the roster outgrows the sidebar the rows
                        // flick vertically; the shared AppScrollBar (same
                        // visual language as every other list in the app)
                        // sits at the rail's right edge below.
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

                            // (Hover moved onto the rows themselves via
                            // PositionHoverArea — the tracker-Math MouseArea
                            // this replaces latched its containsMouse like
                            // every hoverEnabled MouseArea in this build.)
                            // Scrolling with the right-click menu open closes it.
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
                                        // Same pill ramp as the All row:
                                        // subtle lift at rest, hover a step
                                        // up, selected one more + edge.
                                        color: selected ? Theme.activeBg
                                             : (catHover.hovered ? Theme.hoverBg : Theme.rowBg)
                                        Behavior on color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }

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
                                            font.family: "Segoe UI"; font.pixelSize: 15
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
                                            font.family: "Segoe UI"; font.pixelSize: 15
                                            clip: true
                                            onAccepted: media_table.renameCategory(index, text)
                                            onActiveFocusChanged: if (!activeFocus && visible)
                                                media_table.renameCategory(index, text)
                                            Keys.onEscapePressed: media_table.renamingIndex = -1
                                            // The menu's Rename entry only sets renamingIndex — this
                                            // id lives inside a Repeater delegate, out of scope for
                                            // that menu (a stray catNameEdit.forceActiveFocus() there
                                            // threw a silent ReferenceError and never focused it, so
                                            // the box opened with no caret/selection to type into).
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
                                            font.family: "Segoe UI"; font.pixelSize: 14
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

                        // Shared app scrollbar for the category list — a
                        // sibling of the Flickable so it doesn't scroll with
                        // the rows. Hides itself when the list doesn't
                        // overflow (its own `visible` binding).
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
                    // shared Rename/Delete menu for any row; a click anywhere
                    // else in the WINDOW closes it (see MenuCatcher below).
                    // The shared DropdownPanel every other menu in the app
                    // uses (ProjectsPanel, DesignLibraryPane, the settings
                    // screens): its rows derive hover from pointer POSITION
                    // (PositionHoverArea) instead of containsMouse, which
                    // latches in this build — that is the hover highlight the
                    // hand-rolled rows never had reliably.
                    DropdownPanel {
                        id: catContextMenu

                        // DropdownPanel drives `visible` imperatively (openAt
                        // shows it, the consumer hides it). It must stay a
                        // plain value, never a binding: MenuCatcher dismisses
                        // by assigning it.
                        visible: false
                        z: 60
                        width: 148
                        model: [
                            { label: qsTr("Rename") },
                            { label: qsTr("Delete"), danger: true }
                        ]
                        onItemActivated: (label) => media_table.activateCatMenu(label)
                    }
                    // The shared dismiss layer. The MouseArea it replaces was
                    // parented INSIDE this pane, so it only caught clicks that
                    // landed in the library — a click on the header, the
                    // projects column or the output column left the menu
                    // sitting there, which is the "it doesn't always close"
                    // report. MenuCatcher lifts itself beside the menu at the
                    // window root, covers the whole window, closes on PRESS
                    // (release is the flaky half of click delivery in this
                    // build) and takes EVERY button, not just the left one.
                    MenuCatcher { menu: catContextMenu }
                    // MenuCatcher dismisses by assigning `visible` rather than
                    // calling back in, so the table re-derives its own state
                    // from the panel: however the menu closed — an activation,
                    // a press anywhere else, a wheel — nothing is left "open".
                    Connections {
                        target: catContextMenu
                        function onVisibleChanged() {
                            if (!catContextMenu.visible)
                                media_table.closeCatMenu()
                        }
                    }

                    // ---- Show row context menu (right-click) ---- Rename/
                    // Duplicate/Delete for one show: the same shared panel +
                    // dismiss pair as the category menu above, which is where
                    // the note on why the hand-rolled Rectangle + in-pane
                    // catcher is gone lives. Nothing here needs to be declared
                    // last any more: openAt lifts the panel out of this pane
                    // to the window root, so no sibling can paint over it.
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
                        anchors.fill: parent
                        z: 30000
                        title: qsTr("New category")
                        placeholder: qsTr("Category name")
                        confirmLabel: qsTr("Create")
                        onAccepted: (text) => media_table.addCategory(text)
                    }

                    ConfirmDialog {
                        id: showDeleteConfirm
                        anchors.fill: parent
                        z: 30000
                        property string targetPath: ""
                        confirmLabel: qsTr("Delete")
                        confirmVariant: "danger"
                        onConfirmed: {
                            if (showDeleteConfirm.targetPath) {
                                ShowService.deleteShow(showDeleteConfirm.targetPath)
                                ShowService.refreshLibrary()
                            }
                        }
                    }

                    // The original shows table — visible only on the Shows
                    // tab; the other tabs' panes replace it above. Shifted
                    // right of the categories sidebar (its width + 12 margins).
                    Rectangle {
                        id: table_row_header

                        visible: media_tab_bar.currentPane === "shows"
                        x: categories_sidebar.width + 12
                        y: 12

                        height: 21
                        width: parent.width - categories_sidebar.width - 24

                        color: "transparent"

                        Text {
                            id: nAME

                            x: 8
                            y: 4

                            height: 13
                            width: 34

                            color: Theme.textMuted
                            font.family: "Segoe UI"
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignLeft
                            text: qsTr("NAME")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                        Text {
                            id: mODIFIED

                            anchors.right: parent.right
                            anchors.rightMargin: 16
                            y: 4

                            height: 13
                            width: 201

                            color: Theme.textMuted
                            font.family: "Segoe UI"
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("MODIFIED")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                            wrapMode: Text.Wrap
                        }
                    }
                    Rectangle {
                        id: table_body

                        visible: media_tab_bar.currentPane === "shows"
                        x: categories_sidebar.width + 12
                        y: 35

                        // No bottom reserve: the "+ New show" pill FLOATS
                        // over the table (same idiom as DesignLibraryPane's
                        // FAB) instead of the old fixed footer band, whose
                        // hardcoded clearance (98 here vs the button's own
                        // 61+31) left only ~2px of gap — the button visibly
                        // touching the last row.
                        height: parent.height - y
                        width: parent.width - categories_sidebar.width - 24

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

                        // The rows live in a Flickable: the library outgrew
                        // one pane's worth of rows long ago, so everything
                        // below the header scrolls, with the shared
                        // AppScrollBar on the right (same pattern as
                        // ShowCenter's slide grid).
                        Flickable {
                            id: table_scroll
                            anchors.fill: parent
                            clip: true
                            boundsBehavior: Flickable.StopAtBounds
                            contentHeight: table_body.shownShows.length * 32

                        // ---- Shows rows — ENGINE-FED -------------------
                        // One Repeater over ShowService.libraryShows (real
                        // .vgr files, newest first); "All" lists every show,
                        // a selected category filters to its sub-folder. Rows
                        // are click-to-open: the path goes up through
                        // openShowRequested → Main.qml → the engine, with the
                        // unsaved-changes guard. Hover = cursor-driven lift
                        // via a per-row PositionHoverArea (the tabs'
                        // pattern — tracker-Math MouseAreas latch their
                        // containsMouse in this build).
                        Repeater {
                            model: table_body.shownShows
                            delegate: Rectangle {
                                required property var modelData
                                required property int index

                                y: index * 32
                                height: 31
                                width: parent.width - 24

                                radius: 4
                                // The floating "New show" pill sits OVER the last rows.
                                // PositionHoverArea reads pure geometry (AppCursor.hovered),
                                // so it cannot see the pill on top of it — without the gate
                                // the row beneath lit up while the pointer was on the pill.
                                color: (rowHover.hovered && !newShowDockHover.hovered) ? Theme.activeBg : "transparent"
                                Behavior on color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }
                                PositionHoverArea {
                                    id: rowHover
                                    anchors.fill: parent
                                    showCursor: false
                                }

                                // The show mark (IconGlyph "shows", from qml/assets/
                                // shows icon.svg) in front of every show, same as the
                                // Projects panel's rows — the list reads as shows at a
                                // glance instead of as bare filenames.
                                IconGlyph {
                                    x: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "shows"
                                    color: rowHover.hovered ? Theme.textSecondary : Theme.textMuted
                                    width: 15; height: 15
                                    Behavior on color { ColorAnimation { duration: 250; easing.type: Easing.OutCubic } }
                                }

                                Text {
                                    // Indented past the mark; the width gives back the same 8px it used to start at.
                                    x: 31
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - 263
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight
                                    font.family: "Segoe UI"; font.pixelSize: 14
                                    text: modelData.name
                                    textFormat: Text.PlainText
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 201
                                    color: Theme.textMuted
                                    font.family: "Segoe UI"; font.pixelSize: 13
                                    horizontalAlignment: Text.AlignRight
                                    text: {
                                        // Subscribe to the LOCALE so the day/month names
                                        // follow Settings > General > Language (QLocale has
                                        // no signal of its own to re-evaluate this binding).
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
                                    onActivated: vGRPresenter_Main_Screen.centerItem = { type: "show", ref: modelData.path, name: modelData.name, layout: "" }
                                    onOpened: ProjectService.dropOnProject("show_drawer", [{ ref: modelData.path, name: modelData.name }])
                                    onContextRequested: (x, y) => {
                                        const p = rowMouse.mapToItem(media_table, x, y)
                                        media_table.openShowMenu(index, p.x, p.y)
                                    }
                                }
                            }
                        }
                        }   // Flickable table_scroll

                        // A real draggable bar for the now-scrolling table
                        // (auto-hides when every row already fits).
                        AppScrollBar {
                            flickable: table_scroll
                            anchors.top: parent.top; anchors.bottom: parent.bottom
                            anchors.right: parent.right; anchors.rightMargin: 3
                        }

                    }
                    // A true FAB (DesignLibraryPane's "New <design>" pill is
                    // the reference this now matches): no border, no footer
                    // band behind it — it floats OVER the table on its own
                    // stacked-rect soft shadow, with the table scrolling
                    // freely beneath it. Replaces the old dock_footer band,
                    // whose fixed 31px lane left the button flush against
                    // the last row with no visible gap.
                    Rectangle {
                        id: new_show_dock_btn

                        visible: media_tab_bar.currentPane === "shows"
                        anchors.right: parent.right
                        anchors.rightMargin: 22
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 16

                        height: 36
                        width: new_show_1.width + 32
                        radius: 18
                        color: newShowDockHover.hovered ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                        Behavior on color { ColorAnimation { duration: 100 } }

                        // the soft shadow (drawn first, so it sits under the pill)
                        Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 3; width: parent.width + 6; height: parent.height + 4; radius: 20; color: "#26000000" }
                        Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 2; height: parent.height + 2; radius: 19; color: "#33000000" }

                        // PositionHoverArea, the codebase's standard hover/click surface:
                        // a hoverEnabled MouseArea latches containsMouse in this build
                        // (hover-EXIT never delivers) and its cursorShape is unreliable.
                        // Its id is what the table rows consult so the row under the
                        // floating pill no longer highlights when the pill is hovered —
                        // the pill now acts as its own independent button/popup.
                        PositionHoverArea {
                            id: newShowDockHover
                            anchors.fill: parent
                            onClicked: vGRPresenter_Main_Screen.newShowRequested()
                        }

                        // CENTERED: the app's New-show mark (IconGlyph "newShow",
                        // from qml/assets/new show add.svg) beside the label, ONE
                        // Row — the pill's width is this Row's width + 32, so the
                        // label has to live INSIDE the Row: as a sibling it was not
                        // measured, the pill sized itself to the icon alone (48px),
                        // and the label overflowed the pill with the centered icon
                        // on top of it. Sized to the text's cap band like the plus
                        // on the "New project" row (this glyph's ink is inset from
                        // its 24 grid, so 16 here draws at roughly the old cross's
                        // optical size), and nudged 1px the way every box-centered
                        // glyph beside Segoe UI text is.
                        Row {
                            id: new_show_1

                            anchors.centerIn: parent
                            spacing: 8

                            IconGlyph {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.verticalCenterOffset: 1
                                name: "newShow"
                                color: "#ffffff"
                                strokeWidth: 2
                                width: 16; height: 16
                            }

                            Text {
                                id: new_show_1_label

                                anchors.verticalCenter: parent.verticalCenter

                                color: "#ffffff"
                                font.family: "Segoe UI"
                                font.pixelSize: 14
                                font.weight: Font.Bold
                                text: qsTr("New show")
                                textFormat: Text.PlainText
                            }
                        }
                    }   // new_show_dock_btn
                }   // media_table
                // ---- Shows template sidebar (Shows tab) ---- the right column
                // Scripture/The Table have, per CATEGORY: each category keeps its
                // own template pick (settings key shows.categoryTemplate.<name>),
                // shown and changed here with the same chooser popup Scripture
                // uses (TemplatePickerModal, self-reparented to the window
                // overlay). "All" selects no category, so the panel says so
                // instead of showing some other category's pick.
                Rectangle {
                    id: shows_template_sidebar

                    visible: media_tab_bar.currentPane === "shows"
                    x: parent.width - 400
                    y: 39
                    width: 400
                    height: parent.height - 39

                    color: Theme.windowBg

                    // The selected category's name ("" for All — the DEFAULT row).
                    readonly property string categoryName: media_table.currentCategory <= 0
                                                           ? "" : media_table.showCategories[media_table.currentCategory - 1].name
                    // Per-category storage — the same values-map pattern scripture's
                    // "scripture.template" setting uses.
                    readonly property string settingsKey: "shows.categoryTemplate." + categoryName
                    // THE FALLBACK RULE — the same one every template sidebar follows
                    // (Scripture/The Table's engine default already works this way):
                    // when no template is selected FOR a category, what is chosen for
                    // All is used for it. All's own pick (shows.defaultTemplate) is
                    // that fallback; a category overrides it with its own pick.
                    readonly property string defaultKey: "shows.defaultTemplate"
                    readonly property string defaultTemplateId: String(SettingsService.values[defaultKey] ?? "")
                    readonly property string ownTemplateId: categoryName === ""
                                                            ? "" : String(SettingsService.values[settingsKey] ?? "")
                    readonly property bool inherited: categoryName !== "" && ownTemplateId === "" && defaultTemplateId !== ""
                    readonly property string templateId: categoryName === "" ? defaultTemplateId
                                                         : (ownTemplateId !== "" ? ownTemplateId : defaultTemplateId)
                    // Resolved from the designs() CATALOG (its rows provably carry
                    // id/name/color — design(key) only guarantees the preview
                    // blocks). null = no template stored, or a stored key the
                    // catalog no longer has (shows as "Missing template", Reset
                    // still offered).
                    readonly property var templateDesign: {
                        if (templateId === "") return null
                        const list = TemplateLibraryService.designs()
                        for (let i = 0; i < list.length; ++i)
                            if (String(list[i].id) === templateId) return list[i]
                        return null
                    }
                    readonly property string templateName: templateDesign ? String(templateDesign.name ?? "") : ""
                    // The round tune button swaps the panel between the template
                    // view and the slide-options view — the same two-state panel
                    // ReferencePane's footer button drives.
                    property bool optionsOpen: false

                    // The whole catalog, mapped to the picker's {key,name,color,
                    // category,categoryName} shape — copied from ReferencePane's
                    // openTemplateMenu so both pickers list identical rows.
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

                    // THE SHARED TEMPLATE COMPONENT — Scripture/The Table's
                    // right column (ReferencePane's previewCol), rebuilt for
                    // shows: the SAME 16:9 preview tile over the SAME Template
                    // row (label in the red accent, name in DemiBold, and the
                    // three icon actions: clear ✕, pick ⧉, edit ✎). The
                    // preview renders the category's template the way
                    // ReferencePane's renders the picked verses' slide.
                    readonly property var previewDesign: {
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
                        // Reserve the scrollbar's strip so the toggles never sit
                        // under it (ReferencePane's exact pattern).
                        anchors.rightMargin: optionsScrollBar.visible ? 9 : 0
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
                                    height: Math.round(width * 428 / 754)   // the 16:9-ish design ratio, purely from width
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
                                        font.family: "Segoe UI"; font.pixelSize: 14
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
                                        Text { text: qsTr("Template"); color: Theme.accent; font.family: "Segoe UI"; font.pixelSize: 13 }
                                        Text {
                                            width: 150
                                            text: shows_template_sidebar.templateId === ""
                                                  ? qsTr("Default — no template")
                                                  : (shows_template_sidebar.templateName !== "" ? shows_template_sidebar.templateName : qsTr("Missing template"))
                                            color: shows_template_sidebar.templateId === "" ? Theme.textMuted : Theme.textPrimary
                                            elide: Text.ElideRight
                                            font.family: "Segoe UI"; font.pixelSize: 16; font.weight: Font.DemiBold
                                        }
                                    }

                                    Row {
                                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                        spacing: 0

                                        // back to no template (only when one is chosen) —
                                        // for a category this clears ITS pick (it then
                                        // inherits All's, per the fallback rule); for All
                                        // it clears the default itself.
                                        Item {
                                            visible: shows_template_sidebar.categoryName === ""
                                                     ? shows_template_sidebar.templateId !== ""
                                                     : shows_template_sidebar.ownTemplateId !== ""
                                            width: 36; height: 56
                                            IconGlyph { anchors.centerIn: parent; name: "close"; color: clearHover.hovered ? Theme.textPrimary : Theme.textSecondary; width: 10; height: 10 }
                                            HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                                            TapHandler { onTapped: SettingsService.setValue(
                                                shows_template_sidebar.categoryName === ""
                                                ? shows_template_sidebar.defaultKey
                                                : shows_template_sidebar.settingsKey, "") }
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
                                    font.family: "Segoe UI"; font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }

                            // ---- the slide options (the round tune button turns this on) —
                            // ReferencePane's OptionToggle/OptionNumber rows verbatim, over the
                            // shows.* keys ("Line numbers", "Divide long lines", "Max lines").
                            // The engine consumes the scripture/table keys today; the shows
                            // builder reads the same shape when it lands.
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
                                    font.family: "Segoe UI"; font.pixelSize: 14
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
                                    font.family: "Segoe UI"; font.pixelSize: 14
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
                                        font.family: "Segoe UI"; font.pixelSize: 14
                                    }
                                    Rectangle {
                                        width: 24; height: 24; radius: 6; color: plusHover.hovered ? Theme.hoverBg : Theme.rowBg; border.color: Theme.border
                                        Text { anchors.centerIn: parent; text: "+"; color: Theme.textPrimary; font.pixelSize: 15 }
                                        HoverHandler { id: plusHover; cursorShape: Qt.PointingHandCursor }
                                        TapHandler { onTapped: num.nudge(num.step) }
                                    }
                                }
                            }

                            // The boxed options — ReferencePane's exact chrome:
                            // the SLIDE OPTIONS caption, then ONE rounded box
                            // (Theme.windowBg, Theme.border, r=8) holding every row.
                            Column {
                                visible: shows_template_sidebar.optionsOpen
                                x: 16; width: parent.width - 32
                                spacing: 0

                                Text {
                                    x: 6; height: 26; verticalAlignment: Text.AlignVCenter
                                    text: qsTr("SLIDE OPTIONS")
                                    color: Theme.textMuted
                                    font.family: "Segoe UI"; font.pixelSize: 12; font.weight: Font.DemiBold; font.letterSpacing: 1.2
                                }
                                Rectangle {
                                    width: parent.width; height: showsOptionsColumn.height + 8
                                    radius: 8; color: Theme.windowBg; border.color: Theme.border   // Theme.border

                                    Column {
                                        id: showsOptionsColumn
                                        x: 14; y: 4; width: parent.width - 28
                                        spacing: 0

                                        ShowsOptionToggle { settingKey: "shows.verseNumbers" }
                                        ShowsOptionToggle { settingKey: "shows.versesOnIndividualLines" }
                                        ShowsOptionToggle { settingKey: "shows.splitLongVerses" }
                                        Column {
                                            visible: SettingsService.values["shows.splitLongVerses"] === true
                                            width: parent.width
                                            ShowsOptionToggle { settingKey: "shows.splitLongVersesSuffix" }
                                            ShowsOptionNumber { settingKey: "shows.longVersesChars"; step: 10 }
                                            ShowsOptionNumber { settingKey: "shows.longVersesTolerance"; step: 5 }
                                        }
                                        ShowsOptionToggle { settingKey: "shows.smartSplit" }
                                        ShowsOptionNumber { settingKey: "shows.versesPerSlide" }
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
                                        IconGlyph { anchors.centerIn: parent; name: "sliders"; color: shows_template_sidebar.optionsOpen ? "#ffffff" : Theme.textPrimary; fit: true; width: 22; height: 22 }
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

                    // The chooser — same popup Scripture's template row opens, fed
                    // the whole catalog. Applied picks persist to THIS category's
                    // settings key; Reset clears back to "no template".
                    TemplatePickerModal {
                        id: templatePicker
                        z: 30000
                        contentType: "shows"
                        contentTypeLabel: qsTr("All")
                        onApplied: (tpl) => {
                            templatePicker.open = false
                            SettingsService.setValue(shows_template_sidebar.settingsKey, tpl.key)
                        }
                        onCancelled: templatePicker.open = false
                    }
                }
            }
        Rectangle {
            id: right_column

            x: parent.width - 400

            height: workspace_body.topHeight
            width: 400

            border.color: Theme.border
            border.width: 1
            color: Theme.panelBg            // ---- Monitor wall — the shared MonitorWall component
            // (qml/components/MonitorWall.qml), driven by OutputListModel.
            // The old preview_Main_Output / preview_Stage_Screen /
            // preview_Nursery_Display / preview_Stream_Overlay blocks were
            // fixed-position copies that never reacted to anything; the wall
            // itself then lived inline here while the Edit screen's ITEMS tab
            // hosted the same tiles in a plain Grid — "the same monitors"
            // rendered differently per surface. The wall logic (hero page,
            // 2×2 paging, snap swipe, dots) now lives in ONE component both
            // hosts instantiate, so every surface shows the identical wall.
            // Main Output is protected upstream (OutputListModel::removeOutput
            // refuses it and its Delete affordance is hidden), so the wall
            // always keeps a primary. Sits flush at the column's top — the
            // old static "Congregation / Inner Thinks" header strip above it
            // was removed (user call); the wall owns the whole column now.
            MonitorWall {
                id: monitorWall
                // Self-test handle (VGR_SELFTEST grabs this item to PNG for
                // pixel-sampling the checkerboard/style-bg rendering).
                objectName: "selfTestMonitorWall"

                x: (parent.width - width) / 2
                y: 0
                width: 376
            }

            // Divider between the monitor wall and the clock panel (the
            // original line_2 asset, repositioned to track the wall's
            // bottom edge instead of a fixed y).
            Rectangle {
                id: line_2

                y: monitorWall.y + monitorWall.height + (monitorWall.pageCount > 1 ? 22 : 11)
                width: parent.width
                height: 1
                color: Theme.border   // Theme.border
            }

        }


        // ---- Divider between the top region and the library dock: drag to resize, double-click
        // for the default (the same SplitHandle every sidebar uses).
        SplitHandle {
            vertical: false
            value: workspace_body.topHeight
            minValue: workspace_body.minTop
            maxValue: workspace_body.maxTop
            onDragged: (v) => workspace_body.topOverride = v
            onResetRequested: workspace_body.topOverride = -1
        }
    }
}
