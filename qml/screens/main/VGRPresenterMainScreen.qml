import QtQuick
import QtQuick.Shapes
import "../../components"

Rectangle {
    id: vGRPresenter_Main_Screen

    height: 900
    width: 1440

    border.color: "#232530"
    border.width: 1
    clip: true
    color: "#0f1015"

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

            border.color: "#232530"
            border.width: 1
            color: "#12131a"

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
                    onItemOpened: (item) => { if (item.type === "show") vGRPresenter_Main_Screen.openShowRequested(item.ref) }
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

                border.color: "#232530"
                border.width: 1
                color: "#0f1015"

                Rectangle {
                    id: logo_group

                    // (Content sits centred in the slate: the offsets keep it in the middle as the slate grows.)
                    x: 40
                    y: 76 + (workspace_body.topHeight - 460) / 2

                    height: 80
                    width: parent.width - 80

                    color: "transparent"

                    Text {
                        id: vGRPresenter_1

                        x: (parent.width - width) / 2

                        height: 60
                        width: 333

                        color: "#6c5ce7"
                        font.family: "Outfit"
                        font.letterSpacing: 0.48
                        font.pixelSize: 55
                        font.weight: Font.Black
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("VGRPresenter")
                        textFormat: Text.PlainText
                        verticalAlignment: Text.AlignTop
                    }
                    Text {
                        id: v1_0_5_beta_2

                        x: (parent.width - width) / 2
                        y: 64

                        height: 16
                        width: 83

                        color: "#5c6475"
                        font.family: "Segoe UI"
                        font.pixelSize: 15
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("v1.0.5-beta.2")
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

                        color: "#8a94a6"
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

                        border.color: actionQuickSearchMouse.containsMouse ? "#3a3d4d" : "#232530"
                        border.width: 1
                        color: actionQuickSearchMouse.pressed ? "#1e1f28"
                             : (actionQuickSearchMouse.containsMouse ? "#1c1d26" : "#16171e")
                        radius: 8

                        MouseArea {
                            id: actionQuickSearchMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
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

                            Shape {
                                id: _vector_16

                                x: 1.75
                                y: 1.75

                                height: 10.50
                                width: 10.50

                                ShapePath {
                                    id: _vector_16_ShapePath0

                                    fillColor: "#00000000"
                                    strokeColor: "#ff4d3d"
                                    strokeWidth: 2

                                    PathSvg {
                                        id: _vector_16_ShapePath0_PathSvg0

                                        path: "M 10.500091552734375 10.500091552734375 L 7.96842472041161 7.96842472041161 M 9.333333615901045 4.666666807950523 C 9.333333615901045 7.2439955679252375 7.2439955679252375 9.333333615901045 4.666666807950523 9.333333615901045 C 2.0893377698207902 9.333333615901045 0 7.2439955679252375 0 4.666666807950523 C 0 2.0893377698207902 2.0893377698207902 0 4.666666807950523 0 C 7.2439955679252375 0 9.333333615901045 2.0893377698207902 9.333333615901045 4.666666807950523 Z"
                                    }
                                }
                            }
                        }
                        Text {
                            id: quick_search

                            x: 42
                            y: 10

                            height: 16
                            width: 159

                            color: "#e2e8f0"
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

                            Shape {
                                id: _vector_17

                                x: 4.50
                                y: 3

                                height: 6
                                width: 3

                                ShapePath {
                                    id: _vector_17_ShapePath0

                                    fillColor: "#00000000"
                                    strokeColor: "#5c6475"
                                    strokeWidth: 2

                                    PathSvg {
                                        id: _vector_17_ShapePath0_PathSvg0

                                        path: "M 0 6 L 3 3 L 0 0"
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        id: action_New_project

                        y: 44

                        height: 36
                        width: 240

                        border.color: actionNewProjectMouse.containsMouse ? "#3a3d4d" : "#232530"
                        border.width: 1
                        color: actionNewProjectMouse.pressed ? "#1e1f28"
                             : (actionNewProjectMouse.containsMouse ? "#1c1d26" : "#16171e")
                        radius: 8

                        MouseArea {
                            id: actionNewProjectMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
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

                            Shape {
                                id: _vector_18

                                x: 2.92
                                y: 2.92

                                height: 8.17
                                width: 8.17

                                ShapePath {
                                    id: _vector_18_ShapePath0

                                    fillColor: "#00000000"
                                    strokeColor: "#ff4d3d"
                                    strokeWidth: 2

                                    PathSvg {
                                        id: _vector_18_ShapePath0_PathSvg0

                                        path: "M 0 4.083800315856934 L 8.167600631713867 4.083800315856934 M 4.083800315856934 0 L 4.083800315856934 8.167600631713867"
                                    }
                                }
                            }
                        }
                        Text {
                            id: new_project

                            x: 42
                            y: 10

                            height: 16
                            width: 159

                            color: "#e2e8f0"
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

                            Shape {
                                id: _vector_19

                                x: 4.50
                                y: 3

                                height: 6
                                width: 3

                                ShapePath {
                                    id: _vector_19_ShapePath0

                                    fillColor: "#00000000"
                                    strokeColor: "#5c6475"
                                    strokeWidth: 2

                                    PathSvg {
                                        id: _vector_19_ShapePath0_PathSvg0

                                        path: "M 0 6 L 3 3 L 0 0"
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        id: action_New_show

                        y: 88

                        height: 36
                        width: 240

                        border.color: actionNewShowMouse.containsMouse ? "#3a3d4d" : "#232530"
                        border.width: 1
                        color: actionNewShowMouse.pressed ? "#1e1f28"
                             : (actionNewShowMouse.containsMouse ? "#1c1d26" : "#16171e")
                        radius: 8

                        MouseArea {
                            id: actionNewShowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: vGRPresenter_Main_Screen.newShowRequested()
                        }

                        Rectangle {
                            id: presentation_1

                            x: 20
                            y: 11

                            height: 14
                            width: 14

                            clip: true
                            color: "transparent"

                            Shape {
                                id: _vector_20

                                x: 1.17
                                y: 1.75

                                height: 10.50
                                width: 11.67

                                ShapePath {
                                    id: _vector_20_ShapePath0

                                    fillColor: "#00000000"
                                    strokeColor: "#6c5ce7"
                                    strokeWidth: 2

                                    PathSvg {
                                        id: _vector_20_ShapePath0_PathSvg0

                                        path: "M 0 0 L 11.667600631713867 0 M 11.084220600128175 0 L 11.084220600128175 6.416666666666667 C 11.084220600128175 6.726085916161537 10.961294218155647 7.02283191929261 10.74248426689028 7.241624355316163 C 10.523674315624913 7.460416791339716 10.226904556745712 7.583333333333334 9.917460536956789 7.583333333333334 L 1.7501400947570802 7.583333333333334 C 1.440696074968156 7.583333333333334 1.1439260379116805 7.460416791339716 0.9251160866463125 7.241624355316163 C 0.7063061353809446 7.02283191929261 0.5833800315856936 6.726085916161537 0.5833800315856934 6.416666666666667 L 0.5833800315856934 0 M 2.916900157928467 10.5 L 5.833800315856934 7.583333333333334 L 8.7507004737854 10.5"
                                    }
                                }
                            }
                        }
                        Text {
                            id: new_show

                            x: 42
                            y: 10

                            height: 16
                            width: 159

                            color: "#e2e8f0"
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

                            Shape {
                                id: _vector_21

                                x: 4.50
                                y: 3

                                height: 6
                                width: 3

                                ShapePath {
                                    id: _vector_21_ShapePath0

                                    fillColor: "#00000000"
                                    strokeColor: "#5c6475"
                                    strokeWidth: 2

                                    PathSvg {
                                        id: _vector_21_ShapePath0_PathSvg0

                                        path: "M 0 6 L 3 3 L 0 0"
                                    }
                                }
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

                color: "#12131a"

                Rectangle {
                    id: dock_tab_bar

                    height: 39
                    // Full width (the FreeShow sample): the tab pack hugs the
                    // left edge and Search sits at the window's right edge —
                    // the clock panel below the row no longer dictates the
                    // bar's extent.
                    width: parent.width

                    border.color: "#232530"
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
                        width: parent.width; height: parent.height
                        filter: media_tab_bar.searches.scripture !== undefined ? media_tab_bar.searches.scripture : ""
                        onTemplateEditRequested: (id) => vGRPresenter_Main_Screen.designEditRequested("template", id)
                        onConvertToShowRequested: (name, slides) => vGRPresenter_Main_Screen.scriptureShowRequested(name, slides)
                        // Live reference autocomplete: the pane builds the rows (its suggest()),
                        // the tab bar feeds them to the search box and routes picks back here.
                        Component.onCompleted: media_tab_bar.registerSuggester("scripture", scripturePane)
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
                        width: parent.width; height: parent.height
                        filter: media_tab_bar.searches.table !== undefined ? media_tab_bar.searches.table : ""
                        // The Table autocompletes the same way (its suggest() offers sermons).
                        Component.onCompleted: media_tab_bar.registerSuggester("table", tablePane)
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
                        onItemActivated: (item) => vGRPresenter_Main_Screen.centerItem = item
                        onItemOpened: (item) => ProjectService.dropOnProject("media", [{ ref: item.ref, name: item.name }])
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
                        onDesignOpened: (id, name) => ProjectService.dropOnProject("overlay", [{ ref: id, name: name }])
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

                    // ---- Categories sidebar (Shows tab) ---- the sample's
                    // left rail: "All" row, Categories section, category
                    // rows with counts, and the round + that creates new
                    // ones. ENGINE-FED: the roster comes from
                    // ShowService.libraryCategories (real sub-folders of the
                    // show library — persisted across restarts); every CRUD
                    // call goes straight to the engine, which validates and
                    // republishes. Counts are the engine's shows-per-category.
                    property int currentCategory: 0   // 0 = All, 1..n = category
                    // The row being renamed inline (-1 = none). Opened from
                    // the right-click context menu; Enter/focus-loss commits,
                    // Escape cancels.
                    property int renamingIndex: -1
                    // Right-click context menu state — which row opened it
                    // (-1 = closed). One shared menu instance lives at the
                    // pane level (below), positioned over the clicked row.
                    property int menuIndex: -1
                    function openCatMenu(i, x, y) {
                        menuIndex = i
                        // Clamp inside the pane so the menu never pokes out.
                        catContextMenu.x = Math.min(x, width - catContextMenu.width - 4)
                        catContextMenu.y = Math.min(y, height - catContextMenu.height - 4)
                    }
                    function closeCatMenu() { menuIndex = -1 }

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
                    function addCategory() {
                        if (ShowService.createLibraryCategory(qsTr("New category"))) {
                            currentCategory = showCategories.length
                            renamingIndex = showCategories.length - 1
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

                            // ---- All row (the library-wide filter) ------
                            Rectangle {
                                width: parent.width
                                height: 40
                                radius: 6
                                // Tint is cursor-driven only — selection is
                                // marked by the red edge + ring, never by a
                                // persistent background (user call).
                                color: allRowMouse.containsMouse ? "#16171e" : "transparent"
                                Rectangle {
                                    visible: media_table.currentCategory === 0
                                    x: 0; y: 9; width: 3; height: 22
                                    color: "#ff4d3d"; radius: 1.5
                                }
                                Rectangle {
                                    x: 14
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 14; height: 14; radius: 7
                                    color: "transparent"
                                    border.color: media_table.currentCategory === 0 ? "#ff4d3d" : "#5c6475"
                                    border.width: 2
                                }
                                Text {
                                    x: 44
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("All")
                                    color: media_table.currentCategory === 0 ? "#e2e8f0" : "#c7cdd8"
                                    font.family: "Segoe UI"; font.pixelSize: 16
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: ShowService.libraryShows.length
                                    color: "#5c6475"
                                    font.family: "Segoe UI"; font.pixelSize: 14
                                }
                                MouseArea {
                                    id: allRowMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: media_table.currentCategory = 0
                                }
                            }

                            // ---- Categories section header + add ------
                            Item {
                                width: parent.width; height: 30
                                Text {
                                    x: 8; y: 9
                                    text: qsTr("Categories")
                                    color: "#8a94a6"
                                    font.family: "Segoe UI"; font.pixelSize: 14; font.weight: Font.DemiBold
                                }
                                // The + lives at the header's RIGHT (user call) —
                                // hover/press states, honest toast while creation
                                // is engine-owned.
                                Rectangle {
                                    id: addCatBtn

                                    anchors.right: parent.right
                                    anchors.rightMargin: 4
                                    anchors.verticalCenter: parent.verticalCenter

                                    width: 22
                                    height: 22
                                    radius: 11

                                    color: addCatMouse.pressed ? "#5b4bd1"
                                         : (addCatMouse.containsMouse ? "#8d7cf3" : "#6c5ce7")
                                    Behavior on color { ColorAnimation { duration: 100 } }

                                    MouseArea {
                                        id: addCatMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: media_table.addCategory()
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        text: "+"
                                        color: "#ffffff"
                                        font.family: "Segoe UI"; font.pixelSize: 18; font.weight: Font.Medium
                                    }
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
                            y: 82
                            width: parent.width - 16
                            height: parent.height - y - 8

                            clip: true
                            contentWidth: width
                            contentHeight: catRows.height
                            boundsBehavior: Flickable.StopAtBounds

                            // Scroll-proof hover: containsMouse on the rows
                            // goes stale when the list scrolls under a
                            // stationary cursor (no mouse movement = no exit
                            // event, so a tint can ride away with a row and
                            // stick). The hovered row is computed from the
                            // cursor position instead, so every scroll
                            // re-evaluates it. Row pitch = 38 + 2 spacing.
                            readonly property int hoveredRow: {
                                if (!hoverTracker.containsMouse) return -1
                                const y = hoverTracker.mouseY + contentY
                                return Math.min(Math.max(Math.floor(y / 40), 0),
                                                media_table.showCategories.length - 1)
                            }
                            // NoButton so clicks and flick drags pass
                            // straight through to the rows/Flickable.
                            MouseArea {
                                id: hoverTracker
                                anchors.fill: parent
                                acceptedButtons: Qt.NoButton
                                hoverEnabled: true
                            }
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
                                        // Cursor-driven only — the selected
                                        // row keeps its red edge + ring, never
                                        // a persistent background (user call).
                                        color: catScroll.hoveredRow === index ? "#16171e" : "transparent"

                                        Rectangle {
                                            visible: parent.selected
                                            x: 0; y: 8; width: 3; height: 22
                                            color: "#ff4d3d"; radius: 1.5
                                        }
                                        Rectangle {
                                            x: 14
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 14; height: 14; radius: 7
                                            color: "transparent"
                                            border.color: parent.selected ? "#ff4d3d" : "#5c6475"
                                            border.width: 2
                                        }
                                        Text {
                                            id: catNameText
                                            x: 44
                                            width: parent.width - 130
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: media_table.renamingIndex !== index
                                            text: modelData.name
                                            color: selected ? "#e2e8f0" : "#c7cdd8"
                                            elide: Text.ElideRight
                                            font.family: "Segoe UI"; font.pixelSize: 15
                                        }
                                        // Inline rename editor — opened from the
                                        // right-click menu's Rename entry.
                                        TextInput {
                                            id: catNameEdit
                                            x: 44
                                            width: parent.width - 130
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: media_table.renamingIndex === index
                                            enabled: visible
                                            text: visible ? modelData.name : ""
                                            color: "#e2e8f0"
                                            font.family: "Segoe UI"; font.pixelSize: 15
                                            clip: true
                                            onAccepted: media_table.renameCategory(index, text)
                                            onActiveFocusChanged: if (!activeFocus && visible)
                                                media_table.renameCategory(index, text)
                                            Keys.onEscapePressed: media_table.renamingIndex = -1
                                        }
                                        Text {
                                            anchors.right: parent.right
                                            anchors.rightMargin: 12
                                            anchors.verticalCenter: parent.verticalCenter
                                            visible: media_table.renamingIndex !== index
                                            text: modelData.count > 0 ? modelData.count : ""
                                            color: "#5c6475"
                                            font.family: "Segoe UI"; font.pixelSize: 14
                                        }
                                        MouseArea {
                                            id: catRowMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
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

                            x: parent.width - 8
                            y: 82
                            height: parent.height - y - 8
                        }
                    }

                    // ---- Category context menu (right-click) ---- one
                    // shared Rename/Delete menu for any row; a click anywhere
                    // else closes it. Declared LAST in the pane so it paints
                    // above the sidebar and table.
                    Rectangle {
                        id: catContextMenu

                        visible: media_table.menuIndex !== -1
                        x: 0; y: 0
                        width: 148
                        height: 76
                        z: 60

                        radius: 8
                        color: "#1e1f28"
                        border.color: "#3a3d4d"
                        border.width: 1

                        MouseArea {
                            anchors.fill: parent
                            onClicked: (mouse) => {
                                // Consume clicks on the menu itself so they
                                // don't fall through to the catcher.
                                mouse.accepted = true
                            }
                        }

                        Column {
                            anchors.fill: parent
                            anchors.margins: 4

                            Item {
                                width: parent.width; height: 32
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    radius: 5
                                    color: menuRenameMouse.containsMouse ? "#2c2f3c" : "transparent"
                                }
                                Text {
                                    x: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("Rename")
                                    color: "#e2e8f0"
                                    font.family: "Segoe UI"; font.pixelSize: 15
                                }
                                MouseArea {
                                    id: menuRenameMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        media_table.renamingIndex = media_table.menuIndex
                                        media_table.closeCatMenu()
                                        catNameEdit.forceActiveFocus()
                                        catNameEdit.selectAll()
                                    }
                                }
                            }
                            Item {
                                width: parent.width; height: 32
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    radius: 5
                                    color: menuDeleteMouse.containsMouse ? "#2c2f3c" : "transparent"
                                }
                                Text {
                                    x: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("Delete")
                                    color: "#ff6b61"
                                    font.family: "Segoe UI"; font.pixelSize: 15
                                }
                                MouseArea {
                                    id: menuDeleteMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        media_table.removeCategory(media_table.menuIndex)
                                        media_table.closeCatMenu()
                                    }
                                }
                            }
                        }
                    }
                    // Click-outside catcher while the menu is open.
                    MouseArea {
                        anchors.fill: parent
                        enabled: media_table.menuIndex !== -1
                        z: 50
                        onClicked: media_table.closeCatMenu()
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

                            color: "#5c6475"
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

                            color: "#5c6475"
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

                        // Grows with the pane; the footer (bound below)
                        // stays pinned to the bottom.
                        height: parent.height - 98
                        width: parent.width - categories_sidebar.width - 24

                        clip: true
                        color: "transparent"

                        // ---- Shows rows — ENGINE-FED -------------------
                        // One Repeater over ShowService.libraryShows (real
                        // .vgr files, newest first); "All" lists every show,
                        // a selected category filters to its sub-folder. Rows
                        // are click-to-open: the path goes up through
                        // openShowRequested → Main.qml → the engine, with the
                        // unsaved-changes guard. Hover = cursor-driven lift;
                        // the stale-containsMouse lesson applied from day one
                        // via a pointer-position tracker like the sidebar's.
                        readonly property var shownShows: {
                            media_table.libRev   // re-read on engine republish
                            const cat = media_table.currentCategory
                            const inCategory = cat === 0 ? ShowService.libraryShows
                                : ShowService.libraryShowsIn(media_table.showCategories[cat - 1].name)
                            // The tab bar's Shows search: the ENGINE's library search (name or category), best first.
                            const query = media_tab_bar.searches.shows !== undefined ? media_tab_bar.searches.shows.trim() : ""
                            if (query === "")
                                return inCategory
                            const wanted = ShowService.searchLibrary(query)
                            const inScope = {}
                            inCategory.forEach((s) => { inScope[s.path] = true })
                            return wanted.filter((s) => inScope[s.path] === true)
                        }
                        Repeater {
                            model: table_body.shownShows
                            delegate: Rectangle {
                                required property var modelData
                                required property int index

                                y: index * 32
                                height: 31
                                width: parent.width - 24

                                radius: 4
                                color: showRowsHover.hoveredIdx === index ? "#16171e" : "transparent"

                                Text {
                                    x: 8
                                    y: 8
                                    height: 15
                                    width: parent.width - 240
                                    color: "#e2e8f0"
                                    elide: Text.ElideRight
                                    font.family: "Segoe UI"; font.pixelSize: 14
                                    text: modelData.name
                                    textFormat: Text.PlainText
                                    verticalAlignment: Text.AlignTop
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.rightMargin: 16
                                    y: 9
                                    height: 13
                                    width: 201
                                    color: "#5c6475"
                                    font.family: "Segoe UI"; font.pixelSize: 13
                                    horizontalAlignment: Text.AlignRight
                                    text: {
                                        const d = new Date(modelData.modifiedMs)
                                        Qt.formatDate(d, "dddd d, MMMM yyyy")
                                    }
                                    textFormat: Text.PlainText
                                    verticalAlignment: Text.AlignTop
                                }
                                // Click: the show on the centre page. Double-click: into the open project. Drag: into a project.
                                DragSource {
                                    id: rowMouse
                                    anchors.fill: parent
                                    payload: ({ kind: "show_drawer", items: [{ ref: modelData.path, name: modelData.name }] })
                                    label: modelData.name
                                    onActivated: vGRPresenter_Main_Screen.centerItem = { type: "show", ref: modelData.path, name: modelData.name, layout: "" }
                                    onOpened: ProjectService.dropOnProject("show_drawer", [{ ref: modelData.path, name: modelData.name }])
                                }
                            }
                        }
                        // Cursor-position hover tracker for the rows (same
                        // scroll-proof pattern as the category list).
                        MouseArea {
                            id: showRowsHover
                            anchors.fill: parent
                            acceptedButtons: Qt.NoButton
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            z: 1
                            readonly property int hoveredIdx: {
                                if (!containsMouse || table_body.shownShows.length === 0) return -1
                                const i = Math.floor(mouseY / 32)
                                return (i >= 0 && i < table_body.shownShows.length) ? i : -1
                            }
                        }

                    }
                    Rectangle {
                        id: dock_footer

                        visible: media_tab_bar.currentPane === "shows"
                        x: categories_sidebar.width + 12
                        y: parent.height - 61

                        height: 31
                        width: parent.width - categories_sidebar.width - 24

                        color: "transparent"

                    Rectangle {
                        id: new_show_dock_btn

                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        y: 4

                        height: 27
                        width: 104

                        border.color: "#ff4d3d"
                        border.width: 1
                        color: newShowDockMouse.pressed ? "#701f19"
                             : (newShowDockMouse.containsMouse ? "#a03a30" : "#85261f")
                        radius: 100
                        Behavior on color { ColorAnimation { duration: 100 } }

                        // The second dead "New show" chrome button, now
                        // live with the same reset action.
                        MouseArea {
                            id: newShowDockMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: vGRPresenter_Main_Screen.newShowRequested()
                        }

                            Text {
                                id: new_show_1

                                x: 16
                                y: 6

                                height: 15
                                width: 88

                                // Was black text on this dark maroon pill - unreadable (barely more than the pill's own shadow).
                                // White and bolder, matching FreeShow's own "+ New show" pill.
                                color: "#ffffff"
                                font.family: "Segoe UI"
                                font.pixelSize: 14
                                font.weight: Font.Bold
                                horizontalAlignment: Text.AlignLeft
                                text: qsTr("+ New show")
                                textFormat: Text.PlainText
                                verticalAlignment: Text.AlignTop
                            }
                        }
                    }
                }
                Rectangle {
                    id: clock_panel

                    // The band's right region: clock + New show CTA. Starts
                    // BELOW the tab row (the FreeShow sample: the row runs the
                    // full width above it; the clock lives in the bottom-right
                    // of the band).
                    x: parent.width - 400
                    y: 39
                    visible: media_tab_bar.currentPane !== "scripture" && media_tab_bar.currentPane !== "table"   // the Scripture-architecture preview takes this spot

                    height: parent.height - 39

                    width: 400

                    color: "#0f1015"

                    Rectangle {
                        id: digital_clock_group

                        // Dynamic centering — was fixed x/y in the old 439px
                        // panel, which drifted off-balance as soon as the
                        // panel's height became content-driven.
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: -48

                        height: 88
                        width: 352

                        color: "transparent"

                        Text {
                            id: element_5

                            x: 56.50

                            height: 68
                            width: 242

                            color: "#ff523b"
                            font.family: "Segoe UI"
                            font.letterSpacing: 1.12
                            font.pixelSize: 64
                            font.weight: Font.ExtraBold
                            horizontalAlignment: Text.AlignHCenter
                            text: showClockTicker.formatClock(showClockTicker.now, false, true)
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                        Text {
                            id: saturday_8_August_2026

                            x: 97.50
                            y: 72

                            height: 16
                            width: 158

                            color: "#8a94a6"
                            font.family: "Segoe UI"
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            horizontalAlignment: Text.AlignHCenter
                            text: Qt.formatDate(showClockTicker.now, "dddd d, MMMM yyyy")
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                        }
                    }
                    Rectangle {
                        id: clock_actions

                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: 55

                        height: 61
                        width: 352

                        color: "transparent"

                        Rectangle {
                            id: new_show_btn

                            objectName: "selfTestNewShowBtn"   // UI self-test hover target

                            x: 86

                            height: 36
                            width: 180

                            // Hover/press must READ as a reaction — the old
                            // #7d6df0 tint was a ~7% lighten, imperceptible.
                            color: newShowMouse.pressed ? "#5b4bd1"
                                 : (newShowMouse.containsMouse ? "#8d7cf3" : "#6c5ce7")
                            radius: 100
                            Behavior on color { ColorAnimation { duration: 100 } }

                            // LIVE: resets the deck to a fresh slide (see
                            // newShowRequested at the top of this file).
                            MouseArea {
                                id: newShowMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: vGRPresenter_Main_Screen.newShowRequested()
                            }

                            Rectangle {
                                id: plus_1

                                x: 46
                                y: 11

                                height: 14
                                width: 14

                                clip: true
                                color: "transparent"

                                Shape {
                                    id: _vector_37

                                    x: 2.92
                                    y: 2.92

                                    height: 8.17
                                    width: 8.17

                                    ShapePath {
                                        id: _vector_37_ShapePath0

                                        fillColor: "#00000000"
                                        strokeColor: "#ffffff"
                                        strokeWidth: 2

                                        PathSvg {
                                            id: _vector_37_ShapePath0_PathSvg0

                                            path: "M 0 4.083800315856934 L 8.167600631713867 4.083800315856934 M 4.083800315856934 0 L 4.083800315856934 8.167600631713867"
                                        }
                                    }
                                }
                            }
                            Text {
                                id: new_show_2

                                x: 68
                                y: 10

                                height: 16
                                width: 67

                                color: "#ffffff"
                                font.family: "Segoe UI"
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                horizontalAlignment: Text.AlignLeft
                                text: qsTr("New show")
                                textFormat: Text.PlainText
                                verticalAlignment: Text.AlignTop
                            }
                        }
                        Text {
                            id: autosaved_2_mins_ago

                            x: 118.50
                            y: 48

                            height: 13
                            width: 116

                            color: "#5c6475"
                            font.family: "Segoe UI"
                            font.pixelSize: 13
                            font.weight: Font.Normal
                            horizontalAlignment: Text.AlignHCenter
                            textFormat: Text.PlainText
                            verticalAlignment: Text.AlignTop
                            // KERNEL-DRIVEN: real project.saved relay events with a
                            // live "n ago" ticker, not the export's frozen string.
                            property real lastSaveTs: 0
                            function relabel() {
                                if (lastSaveTs <= 0) {
                                    text = qsTr("Not saved yet")
                                    return
                                }
                                const mins = Math.max(0, Math.floor((Date.now() - lastSaveTs) / 60000))
                                text = mins === 0 ? qsTr("Autosaved just now")
                                     : qsTr("Autosaved %1 min%2 ago").arg(mins).arg(mins === 1 ? "" : "s")
                            }
                            Timer {
                                interval: 30000
                                running: parent.lastSaveTs > 0
                                repeat: true
                                triggeredOnStart: true
                                onTriggered: parent.relabel()
                            }
                            Component.onCompleted: {
                                const recent = EngineBridge.recentEngineEvents(200)
                                for (let i = recent.length - 1; i >= 0; --i)
                                    if (recent[i].topic === "project.saved") {
                                        lastSaveTs = recent[i].ts
                                        break
                                    }
                                relabel()
                            }
                        }
                        Connections {
                            target: EngineBridge
                            function onEngineEvent(topic, payload) {
                                if (topic === "project.saved" && payload) {
                                    autosaved_2_mins_ago.lastSaveTs = Date.now()
                                    autosaved_2_mins_ago.relabel()
                                }
                            }
                        }
                    }
                }
            }
        Rectangle {
            id: right_column

            x: parent.width - 400

            height: workspace_body.topHeight
            width: 400

            border.color: "#232530"
            border.width: 1
            color: "#12131a"            // ---- Monitor wall — the shared MonitorWall component
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
                color: "#232530"   // Theme.border
            }

            // Restored: the live clock panel my earlier static-tile cleanup
            // removed along with the preview blocks (it sat directly below
            // them in the original column). The time/date are now driven by
            // the shared LiveClock ticker instead of the Figma-export's
            // frozen "11:50:47" strings.
            LiveClock {
                id: showClockTicker
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
