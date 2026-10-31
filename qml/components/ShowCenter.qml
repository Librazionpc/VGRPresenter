import QtQuick
import VGRPresenterUI
import "../shortcuts"

// The centre page of the Show screen (FreeShow's Show.svelte): what the item you clicked - in the Projects panel or the Shows table -
// looks like. A show is its slide grid (drawn by the same DesignPreview as everywhere else, read from the show's file WITHOUT opening
// it for editing); media shows the picture or the file; audio, overlays, scripture and sections show what they are. "Edit" opens the show
// on the Edit screen. Nothing clicked yet: the splash behind this stays.
//
// `item` is a project item ({ type, ref, name, layout, meta }) or a bare one ({ type: "show", ref: path, name }).
Item {
    id: root

    property var item: null
    property bool slideHotkeysEnabled: false
    signal editRequested(string path)
    signal closeRequested()

    readonly property string type: item ? (item.type ?? "") : ""
    property int reload: 0            // bumped when the show's file changed under us (the next timer), so it is read again
    // The peeked show is a PUBLISHED read (see ShowService): peekShow asks for a
    // file the centre page already knows and hands back what is known, so this
    // binding carries `peekRevision` as its dependency — that is what re-evaluates
    // it when a read lands, or when this app writes that show's file.
    readonly property var loaded: { root.reload; ShowService.peekRevision; return root.type === "show" ? ShowService.peekShow(item.ref) : null }
    readonly property string imageFit: {
        const fileFit = root.type === "image" && root.item
            ? MediaLibraryService.itemFit(String(root.item.ref)) : ""
        return fileFit !== "" ? fileFit : LiveOutputService.defaultMediaFit
    }
    readonly property var show: loaded && loaded.ok ? loaded.show : null
    readonly property var slides: show ? show.slides : []
    property int selectedSlide: -1
    onItemChanged: { selectedSlide = -1; root.refreshWallStepper() }

    // Reuse the configurable Left/Right slide hotkeys for the centre preview's
    // slide selector. Do not steal those keys while the user is typing in a field.
    readonly property bool typingInField: {
        const w = Window.window
        const f = w ? w.activeFocusItem : null
        return !!f && f.text !== undefined && f.cursorPosition !== undefined
    }
    Shortcut {
        action: "previousSlide"
        enabled: root.visible && root.slideHotkeysEnabled && root.type === "show"
                 && root.slides.length > 1 && !root.typingInField
        onActivated: root.stepSlide(-1)
    }
    Shortcut {
        action: "nextSlide"
        enabled: root.visible && root.slideHotkeysEnabled && root.type === "show"
                 && root.slides.length > 1 && !root.typingInField
        onActivated: root.stepSlide(1)
    }

    // The view (FreeShow's slidesOptions), kept by the engine's settings: how many slides across, and grid / list / lyrics.
    readonly property int columns: Math.max(2, Math.min(10, Number(SettingsService.values["session.slideColumns"] ?? 4)))
    readonly property string viewMode: String(SettingsService.values["session.slideView"] ?? "grid")
    readonly property real cellWidth: Math.floor((width - 10) / columns)     // (FreeShow: the grid has 5px padding, a cell is 100 / columns %)
    // FreeShow's "dark" theme
    readonly property color cLighter: Theme.border
    readonly property color cDarker: Theme.rowBg
    readonly property color cDarkest: Theme.panelBg
    readonly property color cText: Theme.textPrimary
    // Selection and focus accents follow the user's Appearance setting.
    readonly property color cSecondary: Theme.accent
    readonly property string mono: FontService.monoFamily
    property bool headerMenuOpen: false
    // the colour of a slide's group (verse, chorus...): the bar under its picture
    function groupColor(slide) {
        const map = { break: "#f5255e", bridge: "#f52598", chorus: "#f525d2", intro: "#d525f5", outro: "#a525f5", pre_chorus: "#8825f5", tag: "#7525f5", verse: "#5825f5" }
        const norm = (t) => String(t ?? "").toLowerCase().replace(/[\s-]+/g, "_").replace(/_?\d+$/, "")
        return map[norm(slide.tag)] ?? map[norm(slide.title)] ?? ""
    }
    property bool barOpen: false      // the floating bar's chevron: shows the zoom
    property bool zoomOpen: false     // the zoom's + / - popup

    // ESCAPE, the ladder this component owns (Main's cancel binding calls it):
    // while the “…” menu is up, put it away — a menu is a menu on every surface
    // of this app; otherwise act as that menu's own Close row, which is how the
    // page is dismissed from the keyboard. It fires the SAME signal the click
    // sends, so the two can never disagree about what closing means. Returns
    // false when there is nothing to close, so the caller can tell the key did
    // no work here.
    function escapeBack() {
        if (root.headerMenuOpen) {
            root.headerMenuOpen = false
            return true
        }
        if (root.item !== null) {
            // The one line that says the KEY did this rather than a click —
            // "the page closed" alone cannot tell the two apart afterwards.
            EngineBridge.log("info", "Navigation",
                             "Esc closed the centre page (" + root.type + " “" + (root.item.name ?? "") + "”)")
            root.closeRequested()
            return true
        }
        return false
    }

    function setColumns(n) {
        const next = Math.max(2, Math.min(10, n))
        if (next !== root.columns)
            SettingsService.setValue("session.slideColumns", next)
    }
    // grid -> list -> lyrics -> grid, like FreeShow's view button
    function nextView() {
        const order = { grid: "list", list: "lyrics", lyrics: "grid" }
        SettingsService.setValue("session.slideView", order[root.viewMode] ?? "grid")
    }
    // ---- the next timer (FreeShow's clock): every slide moves on by itself after so many seconds ----
    property bool timerOpen: false
    property int timerSeconds: 10
    readonly property real timerTotal: root.slides.reduce((t, s) => t + (s.nextTimer ?? 0), 0)
    // every slide already has exactly the seconds in the box: the button then offers to take it off again
    readonly property bool timerApplied: root.slides.length > 0 && root.timerSeconds > 0 && root.slides.every((s) => Math.round(s.nextTimer ?? 0) === root.timerSeconds)
    function formatTime(t) {
        const n = Math.round(t)
        if (n <= 59) return n + "s"
        const m = Math.floor(n / 60), r = n % 60
        return r === 0 ? m + "m" : m + "m " + r + "s"
    }
    // The token of the next-timer write in flight (0 = none): it rewrites the
    // show's file on the engine worker, so the reload below hangs off its verdict.
    property int _timerToken: 0
    function applyTimer() {
        if (!root.item)
            return
        const seconds = root.timerApplied ? 0 : root.timerSeconds
        root._timerToken = ShowService.setNextTimer(root.item.ref, seconds)
    }
    Connections {
        target: ShowService
        function onDocumentJobFinished(token, kind, ok) {
            if (kind !== "timer" || token !== root._timerToken)
                return
            root._timerToken = 0
            if (!ok)
                return
            root.reload++          // the file changed: the grid reads the show again
            root.timerOpen = false
        }
    }
    // A slide's words, for the list and lyrics views.
    function slideText(slide) {
        return (slide.blocks ?? []).filter((b) => (b.kind ?? "text") === "text" && (b.text ?? "") !== "").map((b) => b.text).join("\n")
    }

    // A double-click on a slide STAGES it into the Main Output tile — it
    // used to go live immediately (goLiveWithSlides), which flipped the top
    // GO LIVE/STOP button (and, with an Output bound to a real screen, put
    // content there too) with no explicit go-live action from the user.
    // stageSlides only updates the tile; the top GO LIVE button is what
    // actually commits it. This pane stays a read-only PREVIEW of the
    // show's file (peekShow), not the editor, either way — staging must not
    // open the show for editing or touch the engine's shared "open
    // document" (that would risk discarding whatever the Edit screen has
    // unsaved).
    function takeSlideLive(index, slide, styledPreview) {
        root.selectedSlide = index
        // MEASURED, for the same reason the reference tabs' pick is: the tile only
        // updates when this function returns, so the elapsed time here IS the wait
        // the operator sees. Printed only when it is slow enough to notice.
        const pickT0 = Date.now()
        const wordsPerScreen = Number(SettingsService.value("shows.wordsPerSlide"))
        LiveOutputService.readingWordBudget = wordsPerScreen > 0 ? wordsPerScreen : 40
        // ONE SLIDE FIRST, THE WHOLE SHOW ONLY WHERE IT IS NEEDED.
        //
        // This used to hand stageSlideSet the WHOLE show with `index` saying which
        // slide is being read — for the neighbour slide Alt+Down / the wall's ‹ ›
        // step into. But the tile (and the air) only update when that call returns,
        // and the call first measures the pick's whole READING RUN: every slide's
        // words laid out as one passage at the active style. A show is far bigger
        // than a Table section, so the same double-click sat visibly longer here
        // ("the same for show, it takes a little too long to show on the monitor
        // preview"). The picked slide goes out NOW; the set costs nothing on this
        // click.
        //
        // What still wants the set is a SCROLL passage, which fills the screen from
        // the slides AFTER the pick — so under Scroll the full set follows on a
        // zero-delay timer, once the tile has been updated and painted. In Slide
        // mode nothing needs it: the wall's ‹ › stages the neighbouring slide itself
        // (see stepSlide), one slide at a time.
        const name = root.item ? root.item.name : ""
        const picked = slide ? slide
                             : ((index >= 0 && index < root.slides.length) ? root.slides[index] : null)
        if (picked)
            LiveOutputService.stageSlideSet(name, [picked], 0)
        showSetFill.restart()
        const pickMs = Date.now() - pickT0
        if (pickMs >= 80)
            console.log("[PICKCOST] show slide " + index + " staged in " + pickMs + " ms"
                        + (LiveOutputService.scrollMode ? " (scroll)" : " (slide)"))
    }
    // The show's own set, filled AFTER the click instead of inside it (see
    // takeSlideLive). Read lazily on the timer, not captured: a second click
    // restarts it with its own index, so only the newest pick is ever staged.
    function fillShowSet() {
        if (!root.item || !LiveOutputService.scrollMode)
            return
        const at = root.selectedSlide
        if (at < 0 || at >= root.slides.length)
            return
        LiveOutputService.stageSlideSet(root.item.name, root.slides, at)
    }
    Timer {
        id: showSetFill
        interval: 0
        repeat: false
        onTriggered: root.fillShowSet()
    }

    // ---- The wall's ‹ › while a SHOW slide is on the tile -------------------
    // A staged show slide belongs to no reference tab, so the wall's arrows
    // reached nobody: they were lit and dead ("no arrow on the wall is ever
    // enabled while doing nothing"). A show IS steppable — its neighbouring
    // slide, staged the same way a double-click stages this one — which is
    // exactly the passage step a reference tab does for its own content. This
    // pane therefore answers the wall's step request, and announces itself as
    // the tile's stepper while its show is what the tile is showing.
    function stepSlide(direction) {
        const slides = root.slides
        if (!root.item || slides.length === 0)
            return false
        const at = (root.selectedSlide >= 0 && root.selectedSlide < slides.length)
                 ? root.selectedSlide : 0
        const next = at + direction
        if (next < 0 || next >= slides.length)
            return false               // the show's first/last slide: nothing that way
        takeSlideLive(next, slides[next], null)
        return true
    }

    // Is THIS show's slide what the tile is showing? (staged, or on air — the
    // same two-state ownership test the reference panes make against their own
    // reference text.)
    function ownsWallTile() {
        if (!root.item)
            return false
        return LiveOutputService.stagedIsOurs(root.item.name)
            || (LiveOutputService.live && LiveOutputService.onAirTitle === root.item.name)
    }

    // Announce (or withdraw) this pane as the tile's stepper. A ONE-slide show
    // has nothing to step, so it withdraws too and the wall's arrows stay dim
    // rather than lit-and-inert.
    function refreshWallStepper() {
        LiveOutputService.setWallStepper("show", root.ownsWallTile() && root.slides.length > 1)
    }
    onSlidesChanged: root.refreshWallStepper()
    onSelectedSlideChanged: root.refreshWallStepper()
    Component.onCompleted: root.refreshWallStepper()
    Connections {
        target: LiveOutputService
        function onLiveChanged() { root.refreshWallStepper() }
        function onStagedChanged() { root.refreshWallStepper() }
        function onOnAirChanged() { root.refreshWallStepper() }
        // The wall's ‹ › reached either end of the on-air set: if this show's
        // slide is the one on the tile, step the SHOW (stage the neighbour).
        function onPassageStepRequested(direction) {
            if (root.ownsWallTile())
                root.stepSlide(direction)
        }
    }

    // The show on this page can be the one open in the editor: read it again when it changes, and when this page comes back into view.
    Connections {
        target: ShowService
        function onShowChanged() { if (root.visible && root.type === "show") root.reload++ }
    }
    onVisibleChanged: if (visible && root.type === "show") root.reload++

    Rectangle { anchors.fill: parent; color: root.cDarkest }

    // ---- the header (FreeShow's ShowHeader): 30px over the slides, the name on the left, the dots on the right --------------------
    Item {
        id: bar
        visible: root.item !== null
        z: 10
        x: 0; y: 0; width: parent.width; height: 30
        clip: true
        Rectangle { y: -10; width: parent.width; height: 40; radius: 10; color: "#ef0b0b14" }
        Text {
            x: 13; anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 60
            text: root.item ? root.item.name : ""
            color: root.cText; elide: Text.ElideRight
            font.family: root.mono; font.pixelSize: 15; font.weight: Font.DemiBold
        }
        Item {
            x: parent.width - width; width: 32; height: parent.height
            Rectangle { anchors.fill: parent; color: headerMoreHover.hovered ? Theme.hoverBg : "transparent" }
            IconGlyph { anchors.centerIn: parent; name: "moreVertical"; color: root.cText; opacity: root.headerMenuOpen ? 1 : 0.8; width: 4; height: 14 }
            HoverHandler { id: headerMoreHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.headerMenuOpen = !root.headerMenuOpen }
        }
    }
    // the dots' menu (FreeShow's .showDropdown)
    MouseArea { anchors.fill: parent; z: 19; enabled: root.headerMenuOpen; onPressed: (mouse) => { root.headerMenuOpen = false; mouse.accepted = true } }
    Rectangle {
        visible: root.headerMenuOpen
        z: 20
        x: root.width - width - 5; y: 31
        width: 200; height: headerMenuColumn.height + 2
        radius: 6; color: root.cDarkest; border.color: root.cLighter
        Column {
            id: headerMenuColumn
            x: 1; y: 1; width: parent.width - 2
            Repeater {
                model: root.type === "show" ? [qsTr("Edit show"), "-", qsTr("Close")] : [qsTr("Close")]
                delegate: Item {
                    id: menuEntry
                    required property var modelData
                    width: headerMenuColumn.width
                    height: modelData === "-" ? 1 : 34
                    Rectangle { visible: menuEntry.modelData === "-"; width: parent.width; height: 1; color: root.cLighter }
                    Rectangle { visible: menuEntry.modelData !== "-"; anchors.fill: parent; color: menuEntryHover.hovered ? Theme.hoverBg : "transparent" }
                    // WHITE, and MEDIUM: these two rows are ACTIONS (edit the
                    // show / close it). Pure #ffffff at Font.Normal on the mono
                    // face still read grey — a 15 px regular stroke never covers
                    // its pixels completely (measured: 223,223,226 at its
                    // brightest), so the weight, not the colour, is what makes a
                    // label look white at this size.
                    Text { visible: menuEntry.modelData !== "-"; x: 12; anchors.verticalCenter: parent.verticalCenter; text: menuEntry.modelData; color: Theme.controlInk; font.family: root.mono; font.pixelSize: 15; font.weight: Font.Medium }
                    HoverHandler { id: menuEntryHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        enabled: menuEntry.modelData !== "-"
                        onTapped: {
                            root.headerMenuOpen = false
                            if (menuEntry.modelData === qsTr("Edit show")) root.editRequested(root.item.ref)
                            else root.closeRequested()
                        }
                    }
                }
            }
        }
    }

    // ---- a show: its slides (grid, list or lyrics) ----
    Flickable {
        id: grid
        visible: root.type === "show" && root.show !== null
        x: 0; y: 0
        width: parent.width; height: parent.height
        topMargin: bar.height
        clip: true
        contentHeight: (root.viewMode === "grid" ? gridFlow.height : (root.viewMode === "list" ? listCol.height : lyricsCol.height)) + 76   // room for the bar
        boundsBehavior: Flickable.StopAtBounds

        // A 100-slide show overflowed wheel-only — a real draggable bar.
        // Same positioning convention ReferencePane's verse/paragraph list
        // uses: clear the floating bar (below) instead of running the
        // track straight through it.
        AppScrollBar {
            flickable: grid
            anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 2; anchors.bottomMargin: 56   // clear floatingBar
            anchors.right: parent.right; anchors.rightMargin: 4
        }

        // Ctrl + wheel zooms, like FreeShow.
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: (event) => root.setColumns(root.columns + (event.angleDelta.y < 0 ? 1 : -1))
        }

        Flow {
            id: gridFlow
            visible: root.viewMode === "grid"
            x: 5; y: 5
            width: parent.width - 10
            Repeater {
                model: root.viewMode === "grid" ? root.slides : []
                delegate: Item {
                    id: cell
                    required property var modelData
                    required property int index
                    readonly property bool selected: root.selectedSlide === index
                    readonly property color group: root.groupColor(modelData) !== "" ? root.groupColor(modelData) : root.cDarkest
                    // The active output style's template (Settings > Styles'
                    // "Template for Shows" pick), applied the SAME way
                    // onAirSlide/stagedSlide already do — so the thumbnail
                    // shows what the slide will actually look like on air,
                    // not just its own raw blocks. root.reload keys this
                    // to StylesScreen edits too (see the Connections below).
                    readonly property var styled: { void root.reload; return LiveOutputService.previewWithActiveStyle(cell.modelData) }
                    width: root.cellWidth
                    height: body.height + 4              // (FreeShow: a cell has 2px of padding)

                    Rectangle {
                        id: body
                        x: 2; y: 2; width: parent.width - 4
                        height: thumb.height + labelBar.height
                        color: root.cDarkest

                        // the picture: 16:9
                        Rectangle {
                            id: thumb
                            width: parent.width; height: StageGrid.heightForWidth(width)
                            color: "#000000"; clip: true
                            DesignPreview {
                                width: parent.width
                                showBindPlaceholders: false
                                blocks: cell.styled.blocks ?? []
                                background: cell.styled.background ?? "transparent"
                                // THE COMPOSITION'S ANSWER, not a re-derived
                                // "is this background clear": whether this frame
                                // is transparent is composed once
                                // (SlideComposition::frameTransparent) and read by
                                // every surface that draws the convention — the
                                // monitor wall, the output window and these
                                // thumbnails alike.
                                showCheckerboard: cell.styled.frameTransparent === true
                            }
                            Rectangle { anchors.fill: parent; color: Theme.hoverBg; visible: cellHover.hovered }      // the hover veil
                            // the slide's next timer
                            Rectangle {
                                visible: (cell.modelData.nextTimer ?? 0) > 0
                                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 4
                                width: timerLabel.width + 8; height: 16; radius: 3; color: "#cc0b0b14"
                                Text { id: timerLabel; anchors.centerIn: parent; text: root.formatTime(cell.modelData.nextTimer ?? 0); color: Theme.dangerLight; font.family: root.mono; font.pixelSize: 12 }
                            }
                        }
                        // the label: the number, the group's name, a line in the group's colour under it
                        Rectangle {
                            id: labelBar
                            y: thumb.height; width: parent.width; height: 25
                            color: root.cDarkest
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: cell.group }
                            Text { x: 5; y: 5; text: cell.index + 1; opacity: 0.85; color: root.cText; font.family: root.mono; font.pixelSize: 13 }
                            Text {
                                x: 22; y: 4; width: parent.width - 44
                                horizontalAlignment: Text.AlignHCenter
                                text: (cell.modelData.title ?? "") !== "" ? cell.modelData.title : "—"
                                color: root.cText; elide: Text.ElideRight
                                font.family: root.mono; font.pixelSize: 13; font.weight: Font.Bold
                            }
                        }
                        // the chosen slide: FreeShow's 2px outline in the accent, inside the cell
                        Rectangle { anchors.fill: parent; color: "transparent"; border.width: cell.selected ? 2 : 0; border.color: root.cSecondary }
                    }
                    HoverHandler { id: cellHover; cursorShape: Qt.PointingHandCursor }
                    // A slide already chosen in the preview goes live on one click; a slide
                    // that is not chosen yet is only chosen, and its double-click takes it
                    // live. Right-click opens Edit.
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: {
                            if (root.selectedSlide === cell.index)
                                root.takeSlideLive(cell.index, cell.modelData, cell.styled)
                            else
                                root.selectedSlide = cell.index
                        }
                        onDoubleTapped: root.takeSlideLive(cell.index, cell.modelData, cell.styled)
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: root.editRequested(root.item.ref)
                    }
                }
            }
        }

        // one row a slide: a small picture, its group, its words
        Column {
            id: listCol
            visible: root.viewMode === "list"
            width: parent.width
            spacing: 6
            Repeater {
                model: root.viewMode === "list" ? root.slides : []
                delegate: Rectangle {
                    id: listRow
                    required property var modelData
                    required property int index
                    readonly property bool selected: root.selectedSlide === index
                    // Same active-style-template composition as the grid
                    // cell's own `styled` — see its comment.
                    readonly property var styled: { void root.reload; return LiveOutputService.previewWithActiveStyle(listRow.modelData) }
                    // Grows to fit the WHOLE lyric text — was fixed at 104px
                    // with a 3-line ellipsis, which cut real content off
                    // (a 4+ line verse just lost its last line behind "…").
                    // No cap now: the row is exactly as tall as its text
                    // needs, never less than tall enough for the thumbnail.
                    width: listCol.width
                    height: Math.max(80, lyricText.y + lyricText.implicitHeight + 10)
                    radius: 6
                    color: selected ? Theme.activeBg : (listHover.hovered ? Theme.surface : Theme.panelBg)
                    // Purple selection — the app-wide selection accent
                    // (see cSecondary above).
                    border.color: selected ? Theme.accent : Theme.borderSubtle
                    Rectangle {
                        x: 10; y: 10; width: 106; height: StageGrid.heightForWidth(width); color: "#000"; radius: 3; clip: true
                        DesignPreview {
                            width: parent.width
                            showBindPlaceholders: false
                            blocks: listRow.styled.blocks ?? []
                            background: listRow.styled.background ?? "transparent"
                            // Same one answer as the grid cell above.
                            showCheckerboard: listRow.styled.frameTransparent === true
                        }
                    }
                    Text { x: 130; y: 10; text: (listRow.index + 1) + "  " + (listRow.modelData.title ?? "") + ((listRow.modelData.nextTimer ?? 0) > 0 ? "  ·  " + root.formatTime(listRow.modelData.nextTimer) : ""); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold }
                    // EDITABLE — same live-autosave mechanism as the "lyrics"
                    // view (ShowService.rewriteShowFromText), just presented
                    // per row instead of one big text block: a row's own
                    // words are what's edited here, everyone else's are
                    // read back from root.slides unchanged and rejoined
                    // around it. `ready` skips the autosave the binding's
                    // OWN initial assignment would otherwise fire.
                    TextEdit {
                        id: lyricText
                        x: 130; y: 32; width: parent.width - 146
                        text: root.slideText(listRow.modelData)
                        color: Theme.textSecondary; wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        font.family: Theme.fontFamily; font.pixelSize: 14

                        property bool ready: false
                        Component.onCompleted: lyricText.ready = true
                        onTextChanged: if (lyricText.ready) rowSaveTimer.restart()
                        onActiveFocusChanged: if (!activeFocus && rowSaveTimer.running) {
                            rowSaveTimer.stop()
                            listRow.flushEdit()
                        }
                    }
                    Timer {
                        id: rowSaveTimer
                        interval: 700
                        onTriggered: listRow.flushEdit()
                    }
                    function flushEdit() {
                        if (!root.item) return
                        const full = root.slides.map((s, i) =>
                            i === listRow.index ? lyricText.text : root.slideText(s)).join("\n\n")
                        ShowService.rewriteShowFromText(root.item.ref, full)
                    }
                    HoverHandler { id: listHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onTapped: {
                            if (root.selectedSlide === listRow.index)
                                root.takeSlideLive(listRow.index, listRow.modelData, listRow.styled)
                            else
                                root.selectedSlide = listRow.index
                        }
                        onDoubleTapped: root.takeSlideLive(listRow.index, listRow.modelData, listRow.styled)
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: root.editRequested(root.item.ref)
                    }
                }
            }
        }

        // the words — EDITABLE (FreeShow's own Lyrics/Text-editor view,
        // TextEditor.svelte): one continuous text block for the whole show,
        // blank-line-separated the same way Quick Lyrics splits one — not a
        // read-only per-slide list. Edits autosave (700ms settle, this
        // app's usual debounce) via ShowService.rewriteShowFromText, which
        // re-splits the text into slides and saves straight to the show's
        // own file — isolated from the Edit screen's own editing session,
        // so it can't collide with unsaved work open there.
        Column {
            id: lyricsCol
            visible: root.viewMode === "lyrics"
            x: Math.max(0, (parent.width - width) / 2)
            width: Math.min(parent.width, 760)

            // Which show's text is currently loaded (captured at load time —
            // root.item may already have moved on by the time a pending
            // autosave needs to flush against the RIGHT file).
            property string editingPath: ""
            property bool loaded: false

            function flushPending() {
                if (autosaveTimer.running && lyricsCol.editingPath !== "") {
                    autosaveTimer.stop()
                    ShowService.rewriteShowFromText(lyricsCol.editingPath, lyricsEditor.text)
                }
            }
            function loadText() {
                lyricsCol.flushPending()
                lyricsEditor.text = root.slides.map((s) => root.slideText(s)).join("\n\n")
                lyricsCol.editingPath = root.item ? root.item.ref : ""
                lyricsCol.loaded = true
            }
            onVisibleChanged: if (visible) lyricsCol.loadText(); else lyricsCol.flushPending()
            Connections {
                target: root
                function onItemChanged() { if (lyricsCol.visible && root.type === "show") lyricsCol.loadText() }
            }

            TextEdit {
                id: lyricsEditor
                width: lyricsCol.width
                wrapMode: TextEdit.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontFamily; font.pixelSize: 17
                selectByMouse: true
                onTextChanged: if (lyricsCol.loaded) autosaveTimer.restart()
            }

            Timer {
                id: autosaveTimer
                interval: 700
                onTriggered: {
                    if (lyricsCol.editingPath !== "")
                        ShowService.rewriteShowFromText(lyricsCol.editingPath, lyricsEditor.text)
                }
            }
        }
    }

    // ---- the floating bar (FreeShow's FloatingInputs on the slide area): a chevron that shows the zoom, and the view button ----
    Rectangle {
        id: floatingBar
        visible: root.type === "show" && root.show !== null && root.slides.length > 0
        z: 5
        anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.rightMargin: 12; anchors.bottomMargin: 10
        height: 40; radius: 20
        width: barRow.width + 2
        color: "#d9191923"; border.color: root.cLighter

        Row {
            id: barRow
            x: 1; y: 1; height: parent.height - 2

            // Step the selected show slide onto the monitor tile without
            // requiring another click in the slide grid. These controls use
            // the same staging path as a slide double-click.
            Item {
                objectName: "selfTestShowPreviousSlide"
                visible: root.type === "show"
                width: visible ? 34 : 0; height: parent.height
                opacity: root.selectedSlide > 0 ? 1 : 0.38
                Rectangle {
                    anchors.centerIn: parent; width: 30; height: 30; radius: 15; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: prevSlideHover.hovered; glowColor: Theme.hoverBg }
                }
                IconGlyph { anchors.centerIn: parent; name: "previous"; color: Theme.controlInk; width: 14; height: 14; fit: true }
                HoverHandler { id: prevSlideHover; cursorShape: root.selectedSlide > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler { enabled: root.selectedSlide > 0; onTapped: root.stepSlide(-1) }
            }
            Item {
                objectName: "selfTestShowNextSlide"
                visible: root.type === "show"
                width: visible ? 34 : 0; height: parent.height
                opacity: root.selectedSlide >= 0 && root.selectedSlide < root.slides.length - 1 ? 1 : 0.38
                Rectangle {
                    anchors.centerIn: parent; width: 30; height: 30; radius: 15; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: nextSlideHover.hovered; glowColor: Theme.hoverBg }
                }
                IconGlyph { anchors.centerIn: parent; name: "next"; color: Theme.controlInk; width: 14; height: 14; fit: true }
                HoverHandler { id: nextSlideHover; cursorShape: root.selectedSlide >= 0 && root.selectedSlide < root.slides.length - 1 ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler { enabled: root.selectedSlide >= 0 && root.selectedSlide < root.slides.length - 1; onTapped: root.stepSlide(1) }
            }

            // chevron: opens the bar (the zoom hides behind it, like FreeShow's)
            Item {
                objectName: "selfTestBarChevron"
                width: 40; height: parent.height
                Rectangle {
                    anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: chevHover.hovered; glowColor: Theme.hoverBg }
                }
                // a chevron drawn as two bars, so it sits dead centre: > while the bar is open, < while it is shut
                Item {
                    anchors.centerIn: parent
                    width: 12; height: 12
                    opacity: root.barOpen ? 1 : 0.65
                    rotation: root.barOpen ? 0 : 180
                    Behavior on rotation { NumberAnimation { duration: 250; easing.type: Easing.OutCubic } }
                    Rectangle { x: 4.5 - width / 2; y: 3.4 - height / 2; width: 8; height: 2; radius: 1; color: Theme.accent; rotation: 45 }
                    Rectangle { x: 4.5 - width / 2; y: 8.6 - height / 2; width: 8; height: 2; radius: 1; color: Theme.accent; rotation: -45 }
                }
                HoverHandler { id: chevHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { root.barOpen = !root.barOpen; if (!root.barOpen) root.zoomOpen = false } }
            }

            Rectangle { width: 1; height: parent.height; color: root.cLighter }

            // the next timer: red when the slides have one
            Item {
                id: timerButton
                objectName: "selfTestBarClock"
                width: 40; height: parent.height
                Rectangle {
                    anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: timerHover.hovered || root.timerOpen; glowColor: Theme.hoverBg }
                }
                IconGlyph { anchors.centerIn: parent; name: "clock"; color: root.timerTotal > 0 ? Theme.accent : Theme.controlInk; fit: true; width: 16; height: 16 }
                HoverHandler { id: timerHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        if (!root.timerOpen) {
                            const first = root.slides.length > 0 ? Math.round(root.slides[0].nextTimer ?? 0) : 0
                            root.timerSeconds = first > 0 ? first : 10
                        }
                        root.timerOpen = !root.timerOpen
                        root.zoomOpen = false
                    }
                }

                // the popover over the button
                Rectangle {
                    visible: root.timerOpen
                    x: parent.width - width + 1; y: -height - 8
                    width: 252; height: timerPanel.height + 28
                    radius: 12; color: "#f018192a"; border.color: root.cLighter
                    // Swallows any click that lands on the popover's own
                    // padding/gaps (not one of its controls below) — those
                    // had nothing to consume them, so the click fell through
                    // to whatever slide sits underneath the popover's visual
                    // position and selected it instead.
                    MouseArea { anchors.fill: parent; onClicked: (mouse) => { mouse.accepted = true } }
                    Column {
                        id: timerPanel
                        x: 16; y: 14; width: parent.width - 32
                        spacing: 12

                        Text { text: qsTr("NEXT TIMER"); color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 12; font.weight: Font.Bold; font.letterSpacing: 1.2 }

                        // seconds: - [ 10 ] +
                        Rectangle {
                            width: parent.width; height: 40; radius: 8; color: Theme.windowBg; border.color: Theme.border
                            Item {
                                width: 40; height: parent.height
                                opacity: root.timerSeconds > 0 ? 1 : 0.35
                                Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: Theme.controlInk }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.timerSeconds = Math.max(0, root.timerSeconds - 1) }
                            }
                            Row {
                                anchors.centerIn: parent; spacing: 6
                                TextInput {
                                    id: timerInput
                                    width: 44; horizontalAlignment: TextInput.AlignRight
                                    color: Theme.controlInk; selectByMouse: true
                                    font.family: Theme.fontFamily; font.pixelSize: 18; font.weight: Font.DemiBold
                                    validator: IntValidator { bottom: 0; top: 3600 }
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    onTextEdited: root.timerSeconds = Number(text) || 0
                                    Component.onCompleted: text = String(root.timerSeconds)
                                    Connections {
                                        target: root
                                        function onTimerSecondsChanged() { if (timerInput.text !== String(root.timerSeconds)) timerInput.text = String(root.timerSeconds) }
                                    }
                                }
                                Text { anchors.baseline: timerInput.baseline; text: qsTr("seconds"); color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: 14 }
                            }
                            Item {
                                anchors.right: parent.right
                                width: 40; height: parent.height
                                opacity: root.timerSeconds < 3600 ? 1 : 0.35
                                Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: Theme.controlInk }
                                Rectangle { anchors.centerIn: parent; width: 2; height: 12; radius: 1; color: Theme.controlInk }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: root.timerSeconds = Math.min(3600, root.timerSeconds + 1) }
                            }
                        }

                        // apply it to every slide, or take it off again
                        Rectangle {
                            width: parent.width; height: 40; radius: 20
                            readonly property bool usable: root.timerApplied || root.timerSeconds > 0
                            opacity: usable ? 1 : 0.4
                            color: root.timerApplied ? (applyHover.hovered ? Theme.hoverBg : "transparent") : (applyHover.hovered ? Theme.accentLight : Theme.accent)
                            border.color: root.timerApplied ? Theme.border : Theme.accent
                            Row {
                                anchors.centerIn: parent; spacing: 8
                                Text { text: root.timerApplied ? qsTr("Reset") : qsTr("To all slides"); color: Theme.controlInk; font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.DemiBold }
                                Text { visible: !root.timerApplied && root.timerSeconds > 0; text: root.formatTime(root.timerSeconds * root.slides.length); color: Theme.dangerLight; font.family: Theme.fontFamily; font.pixelSize: 14 }
                            }
                            HoverHandler { id: applyHover; cursorShape: parent.usable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                            TapHandler { enabled: parent.usable; onTapped: root.applyTimer() }
                        }

                        Text {
                            visible: root.timerTotal > 0
                            width: parent.width; horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Duration: %1").arg(root.formatTime(root.timerTotal))
                            color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 13
                        }
                    }
                }
            }

            Rectangle { visible: root.barOpen; width: 1; height: parent.height; color: root.cLighter }

            // zoom: the percentage, + and -, in a popup over the button
            Item {
                id: zoomButton
                visible: root.barOpen
                width: 40; height: parent.height
                Rectangle {
                    anchors.centerIn: parent; width: 32; height: 32; radius: 16; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: zoomHover.hovered || root.zoomOpen; glowColor: Theme.hoverBg }
                }
                IconGlyph { anchors.centerIn: parent; name: "zoomIn"; color: root.zoomOpen ? Theme.controlInk : Theme.navLabelMuted; fit: true; width: 16; height: 16 }
                HoverHandler { id: zoomHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { root.zoomOpen = !root.zoomOpen; root.timerOpen = false } }

                Rectangle {
                    visible: root.zoomOpen
                    x: -1; y: -height - 2; width: parent.width + 2; height: 122
                    radius: 20; color: "#f018192a"; border.color: root.cLighter
                    // (rounded top only: the bottom sits flat on the button)
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 20; color: parent.color }
                    Rectangle { anchors.bottom: parent.bottom; anchors.bottomMargin: -1; x: parent.border.width; width: parent.width - 2; height: 2; color: parent.color }
                    // Same click-through fix as the timer popover: swallow
                    // clicks on the popover's own padding/gaps.
                    MouseArea { anchors.fill: parent; onClicked: (mouse) => { mouse.accepted = true } }
                    Column {
                        anchors.fill: parent; anchors.topMargin: 1
                        Item {
                            width: parent.width; height: 38
                            Text { anchors.centerIn: parent; text: Math.round(100 / root.columns) + "%"; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 13 }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.setColumns(4) }   // the usual size
                        }
                        Rectangle { width: parent.width; height: 1; color: root.cLighter }
                        // + makes the slides bigger (fewer across)
                        Item {
                            width: parent.width; height: 40
                            opacity: root.columns > 2 ? 1 : 0.35
                            Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: Theme.controlInk }
                            Rectangle { anchors.centerIn: parent; width: 2; height: 12; radius: 1; color: Theme.controlInk }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.setColumns(root.columns - 1) }
                        }
                        Item {
                            width: parent.width; height: 40
                            opacity: root.columns < 10 ? 1 : 0.35
                            Rectangle { anchors.centerIn: parent; width: 12; height: 2; radius: 1; color: Theme.controlInk }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.setColumns(root.columns + 1) }
                        }
                    }
                }
            }

            Rectangle { visible: root.barOpen; width: 1; height: parent.height; color: root.cLighter }

            // the view: grid -> list -> lyrics
            Item {
                width: 44; height: parent.height
                Rectangle {
                    anchors.centerIn: parent; width: 34; height: 34; radius: 17; color: "transparent"
                    HoverGlow { anchors.fill: parent; radius: parent.radius; hovered: viewHover.hovered; glowColor: Theme.hoverBg }
                }
                IconGlyph {
                    anchors.centerIn: parent
                    name: root.viewMode === "list" ? "listView" : (root.viewMode === "lyrics" ? "textLines" : "gridView")
                    color: root.viewMode === "grid" ? Theme.controlInk : Theme.navLabelMuted
                    fit: true; width: 18; height: 18
                }
                HoverHandler { id: viewHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.nextView() }
            }
        }
    }

    // Click-outside catcher for the timer/zoom popovers — below floatingBar's
    // own z (5) so the popovers (its descendants) stay clickable, above
    // everything else so a click anywhere else in the pane dismisses them
    // (they used to only close on Escape or a successful "To all slides").
    MouseArea {
        anchors.fill: parent
        z: 4
        enabled: root.timerOpen || root.zoomOpen
        onClicked: { root.timerOpen = false; root.zoomOpen = false }
    }

    Text {
        visible: root.type === "show" && root.show === null
        anchors.centerIn: parent
        text: root.loaded && root.loaded.error ? qsTr("This show could not be read: %1").arg(root.loaded.error) : ""
        color: Theme.dangerLight; font.family: Theme.fontFamily; font.pixelSize: 15
    }

    // ---- a picture ----
    Image {
        visible: root.type === "image"
        // Start below the 30px preview header so its dark surface never covers
        // the image; use the dock divider as the lower edge to avoid a gutter.
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 40
        anchors.bottomMargin: 0
        fillMode: root.imageFit === "cover" ? Image.PreserveAspectCrop
                 : root.imageFit === "fill" ? Image.Stretch : Image.PreserveAspectFit
        asynchronous: true
        source: root.type === "image" && root.item ? "file:///" + String(root.item.ref).replace(/\\/g, "/") : ""

        // A single click previews the picture here; a double-click sends it
        // to the monitor output, matching the media library's take gesture.
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onDoubleTapped: {
                if (root.item)
                    LiveOutputService.takeMedia(String(root.item.ref), String(root.item.name ?? ""))
            }
        }

        // Still images also need an obvious take-to-monitor control in the
        // centre preview, matching the play/take affordance on video previews.
        Rectangle {
            visible: root.item !== null
                     && !(LiveOutputService.mediaOnAir
                          && LiveOutputService.mediaPath === String(root.item.ref))
            anchors.centerIn: parent
            width: 64; height: 64; radius: 32
            color: "#b0000000"
            border.width: 2
            border.color: Theme.accent
            IconGlyph {
                anchors.centerIn: parent
                name: "play"
                color: Theme.controlInk
                fit: true
                width: 26; height: 26
            }
            PositionHoverArea {
                anchors.fill: parent
                onClicked: {
                    if (root.item)
                        LiveOutputService.takeMedia(String(root.item.ref), String(root.item.name ?? ""))
                }
            }
        }
    }

    // ---- an overlay ----
    Item {
        visible: root.type === "overlay"
        anchors.fill: parent; anchors.margins: 16; anchors.topMargin: 56
        readonly property var design: root.type === "overlay" ? OverlayLibraryService.design(root.item.ref) : ({})
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width, StageGrid.widthForHeight(parent.height)); height: StageGrid.heightForWidth(width)
            color: "#000"; clip: true
            DesignPreview {
                anchors.fill: parent
                blocks: parent.parent.design.blocks ?? []
                background: parent.parent.design.background ?? "transparent"
            }
        }
    }

    // ---- a passage: the slide the engine builds for it, with the reference under ----
    Item {
        visible: root.type === "scripture" && scriptureView.ready
        anchors.fill: parent; anchors.margins: 16; anchors.topMargin: 56
        QtObject {
            id: scriptureView
            readonly property var meta: root.type === "scripture" && root.item ? (root.item.meta ?? {}) : ({})
            readonly property bool ready: meta.bible !== undefined && meta.book !== undefined
            readonly property var design: ready ? ScriptureService.preview(meta.bible, meta.book, meta.chapter, meta.verses ?? []) : ({})
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width, StageGrid.widthForHeight(parent.height)); height: StageGrid.heightForWidth(width)
            color: "#000"; clip: true
            DesignPreview {
                anchors.fill: parent
                blocks: scriptureView.design.blocks ?? []
                background: scriptureView.design.background ?? "transparent"
            }
        }
    }

    // ---- a video: the live frame + the on-air transport -----------------
    // Video files preview here (the frame follows playback — play it below
    // and this moves), with the TAKE ON AIR control and, once on air, the
    // full transport (play/pause, seek scrubber, position, mute, off-air).
    // Reads LiveOutputService's media state inside bindings, so the bar
    // follows the take no matter where it started (grid pill or here).
    Item {
        id: mediaView
        visible: root.type === "media" || root.type === "video"
        anchors.fill: parent; anchors.margins: 16; anchors.topMargin: 56

        readonly property string filePath: root.item ? String(root.item.ref) : ""
        readonly property bool onAir: LiveOutputService.mediaOnAir
                                      && LiveOutputService.mediaPath === filePath
        readonly property bool isVideoFile: {
            const e = filePath.substring(filePath.lastIndexOf(".") + 1).toLowerCase()
            return ["mp4", "mov", "mkv", "avi", "webm", "m4v", "wmv"].indexOf(e) >= 0
        }
        // The preview follows the ON-AIR playback when this file IS on air
        // (image://mediaplay, rev-paced); otherwise a still from the engine's
        // thumbnail cache (the same middle frame the grid tile shows).
        readonly property url previewSource: onAir
            ? "image://mediaplay?v=" + LiveOutputService.mediaRev
            : "image://mediathumb/500/-1/" + encodeURIComponent(filePath)
        readonly property bool playing: LiveOutputService.mediaState === "playing"

        // THIS FILE'S OWN FILL ("" = the output's default). A choice about the
        // PICTURE, stored on the file by the library, so it is the same on every
        // output and comes back with the next take — see
        // LiveOutputService::setMediaFit. `fitPulse` re-reads it when a write
        // lands (the service writes it off the interface thread).
        property int fitPulse: 0
        Connections {
            target: MediaLibraryService
            function onFoldersChanged() { mediaView.fitPulse++ }
        }
        // While the file is on air, use the compositor's effective fit;
        // otherwise use this file's optional override.
        readonly property string storedFit: {
            void mediaView.fitPulse
            return MediaLibraryService.itemFit(mediaView.filePath)
        }
        readonly property string shownFit: mediaView.onAir ? LiveOutputService.mediaFit
            : (mediaView.storedFit !== "" ? mediaView.storedFit : LiveOutputService.defaultMediaFit)
        readonly property int previewFill: mediaView.shownFit === "cover" ? Image.PreserveAspectCrop
                                            : mediaView.shownFit === "fill" ? Image.Stretch
                                                                            : Image.PreserveAspectFit

        // The media preview follows the saved file override or Outputs default.
        Rectangle {
            id: videoBox
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: Math.min(parent.width, parent.height * 16 / 9)
            height: Math.round(width * 9 / 16)
            // Letterboxed media uses the preview surface, not a black box
            // that appears around the image when it is taken to output.
            color: root.cDarkest
            radius: 4
            clip: true

            Image {
                anchors.fill: parent
                source: mediaView.previewSource
                // The picture laid out by ITS OWN fill, so the preview answers
                // the same question the outputs do.
                fillMode: mediaView.previewFill
                cache: false
                asynchronous: true
            }

            // Not on air: the centre overlay IS the take button.
            Rectangle {
                visible: !mediaView.onAir
                anchors.centerIn: parent
                width: 64; height: 64; radius: 32
                color: "#b0000000"
                border.width: 2
                border.color: Theme.accent
                IconGlyph {
                    anchors.centerIn: parent
                    name: "play"
                    color: Theme.controlInk
                    fit: true
                    width: 26; height: 26
                }
                PositionHoverArea {
                    anchors.fill: parent
                    showCursor: false
                    onClicked: {
                        if (mediaView.isVideoFile)
                            LiveOutputService.takeMedia(mediaView.filePath, root.item.name)
                        else
                            root.editRequested(mediaView.filePath)
                    }
                }
            }
        }

        // ---- the transport (videos only; an on-air still shows just OFF AIR)
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: videoBox.bottom
            anchors.topMargin: 10
            width: Math.min(videoBox.width, 560)
            spacing: 8
            visible: mediaView.isVideoFile

            // Seek scrubber + time readout. mediaPosition pulses at the
            // player's rate while playing — the scrubber tracks it.
            Item {
                id: seekBar
                width: parent.width
                height: 22

                readonly property real frac: mediaView.onAir && LiveOutputService.mediaDuration > 0
                    ? Math.max(0, Math.min(1, LiveOutputService.mediaPosition / LiveOutputService.mediaDuration)) : 0

                // Track.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 4
                    radius: 2
                    color: Theme.toggleOffTrack
                }
                // Fill + thumb.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * seekBar.frac
                    height: 4
                    radius: 2
                    color: Theme.accent
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Math.max(0, parent.width * seekBar.frac - 6)
                    width: 12; height: 12; radius: 6
                    color: Theme.controlInk
                    visible: mediaView.onAir
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: mediaView.onAir
                    cursorShape: Qt.PointingHandCursor
                    onClicked: (mouse) => LiveOutputService.mediaSeek(mouse.x / width)
                }
                Text {
                    anchors.right: parent.right
                    anchors.top: parent.bottom
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 11
                    text: mediaView.onAir
                        ? LiveOutputService.formatMediaTime(LiveOutputService.mediaPosition)
                          + " / " + LiveOutputService.formatMediaTime(LiveOutputService.mediaDuration)
                        : ""
                }
            }

            // Buttons: play/pause, mute. Taking media on/off air has its own
            // explicit control (the big centre take-overlay above, and the
            // off-air state's own play glyph there) — this row USED TO carry
            // a second button that duplicated that exact takeMedia/clearMedia
            // call behind a "play" icon, so clicking what looked like a
            // transport play button silently took the media live instead of
            // just starting playback (user report: "the play button... goes
            // live and it's meant to pause and play"). Removed; the
            // play/pause toggle below (mediaTogglePlay) is the only button
            // here now, and it never touches on-air state.
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 10
                Rectangle {
                    visible: mediaView.onAir
                    width: 34; height: 34; radius: 17
                    color: Theme.chip
                    IconGlyph {
                        anchors.centerIn: parent
                        name: mediaView.playing ? "pause" : "play"
                        color: Theme.textPrimary
                        fit: true; width: 15; height: 15
                    }
                    PositionHoverArea {
                        anchors.fill: parent
                        showCursor: false
                        onClicked: LiveOutputService.mediaTogglePlay()
                    }
                }
                Rectangle {
                    visible: mediaView.onAir
                    width: 34; height: 34; radius: 17
                    color: Theme.chip
                    IconGlyph {
                        anchors.centerIn: parent
                        name: LiveOutputService.mediaMuted ? "close" : "volume2"
                        color: LiveOutputService.mediaMuted ? Theme.dangerLight : Theme.textPrimary
                        fit: true; width: 15; height: 15
                    }
                    PositionHoverArea {
                        anchors.fill: parent
                        showCursor: false
                        onClicked: LiveOutputService.mediaToggleMuted()
                    }
                }
            }
        }
    }

    // ---- everything else: what it is ----
    Column {
        visible: root.item !== null && root.type !== "show" && root.type !== "image" && root.type !== "overlay"
                 && root.type !== "media" && root.type !== "video"
                 && !(root.type === "scripture" && scriptureView.ready)
        anchors.centerIn: parent
        spacing: 10
        IconGlyph {
            anchors.horizontalCenter: parent.horizontalCenter
            name: root.type === "audio" ? "music" : root.type === "video" || root.type === "media" ? "play" : root.type === "scripture" ? "bible"
                : root.type === "camera" ? "camera" : root.type === "pdf" ? "fileText" : "layoutDashboard"
            color: Theme.textMuted; fit: true; width: 40; height: 40
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.type === "section"
            text: root.item ? root.item.name : ""
            color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 30; font.weight: Font.DemiBold; font.capitalization: Font.AllUppercase
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.type !== "section" && root.item !== null
            width: Math.min(root.width - 60, 520); horizontalAlignment: Text.AlignHCenter
            text: root.item ? String(root.item.ref) : ""
            color: Theme.textMuted; elide: Text.ElideMiddle
            font.family: Theme.fontFamily; font.pixelSize: 14
        }
    }
}
