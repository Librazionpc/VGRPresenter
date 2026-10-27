import QtQuick
import VGRPresenterUI

// The output monitor wall — ONE component, TWO hosts (Show screen right
// column, Edit screen ITEMS tab). FreeShow's preview chrome, adapted:
//
//   [ live frame | live frame ]      <- REAL frames from the engine preview
//   [ < >  ▶  ⌫ Clear ]              <- transport: prev/next slide, clear
//
// The old tile's fake "LIVE 1" badge and play glyph are GONE — a tile either
// shows the actual distributed frame (its output is active) or the
// checkerboard empty state. Geometry contract kept: `height` is the WALL's
// height; hosts keep their +22/+11 spacing constants.
Item {
    id: root

    // Roster-change revision: rowCount() isn't notifyable from QML.
    property int _wallRev: 0
    Connections {
        target: OutputListModel
        function onRowsInserted() { root._wallRev++ }
        function onRowsRemoved() { root._wallRev++ }
        function onDataChanged() { root._wallRev++ }
    }

    // Frame cadence: the preview provider's cache-buster — bumped by the live
    // service's frameRev while live, by a slow idle timer otherwise (cheap; a
    // static image request is skipped entirely while the frame hash is null).
    readonly property int frameRev: LiveOutputService.frameRev
    readonly property bool live: LiveOutputService.live
    // A taken input engages the preview chrome too — the toolbar's clear
    // actions are the wall-side way to release it (the Media pane's click
    // is the take side).
    readonly property bool inputTaken: LiveOutputService.inputLabel !== ""
    // ANYTHING landing on the Main Output tile engages the toolbar, not
    // just GO LIVE/a taken input — media-on-air, a live overlay, or an
    // audio meter tap (the Media pane's audio/bus double-click) each put
    // something on the tile just as visibly, so Clear all / ‹ › / the
    // per-layer buttons must be reachable for those too, not stay hidden
    // until GO LIVE is separately pressed.
    // A STAGED pick (stageSlides() — a double-clicked slide, not yet
    // committed by GO LIVE) also lands visibly on the tile now, same as
    // everything else in this list — live report: the whole toolbar
    // (including the ▶ button that's now the explicit way to commit a
    // stage) vanished the moment something was staged instead of truly
    // live, so there was nothing left to click to go live WITH.
    readonly property bool staged: LiveOutputService.stagedSlide.valid === true
    // "Genuinely has content on air" — narrower than toolbar.onAir
    // (LiveOutputService.onAirTotal > 0), which counts the go-live-with-
    // nothing blank fallback as "1 slide on air" too: that fallback exists
    // so a real style background shows instead of plain black, not to mark
    // the slide-status icon below as if a real pick was on air (reported
    // live: "the icon is still selected... when there's no active show").
    readonly property bool hasRealOnAirContent: !!(LiveOutputService.onAirSlide.blocks
        && LiveOutputService.onAirSlide.blocks.length > 0)
    // Deliberately does NOT count metering taps: a meter TAP is an observation,
    // not content, and this app holds taps that have nothing to do with the
    // wall at all — an audio row being configured in Settings ·
    // Audio & Video meters its device so the strips move while you aim it, and
    // every source card in Media can hold one. Counting those put the toolbar on
    // screen (and lit the audio button) with nothing taken, nothing live and
    // nothing on air, and then "Clear all" could never clear it: there was no
    // wall-side state to clear, the tap belonged to another screen. What the
    // wall reports is what the WALL is doing.
    readonly property bool anythingOnAir: root.live || root.inputTaken || root.staged
        || LiveOutputService.mediaOnAir || LiveOutputService.activeOverlays.length > 0
        // ...AND while a Clear all is still undoable. This is the one entry
        // here that is not "something is on the wall" — but it has to be, or
        // Clear all would hide the very toolbar that holds its own undo the
        // instant it ran: the button clearing everything is what takes the
        // toolbar down, so "Show all" could never be reached.
        || LiveOutputService.canRestoreCleared

    // ONE icon box for every glyph on the preview chrome (transport pills,
    // Clear all / Show all / Blackout, the per-layer clears, the audio
    // transport). They used to be 15 in the tool pills and 13 in the row
    // buttons, which read as mismatched sizes in the same strip; a single
    // shared number is what keeps them optically equal, since `fit` scales
    // each Lucide path from its own 24-unit grid into this box.
    readonly property real toolIconSize: 14
    // ...and one stroke weight, or a shipped 2-unit stroke reads heavier at
    // one size than the other even with equal boxes.
    readonly property real toolIconStroke: 1.6

    // External contract: hosts position content under the wall and drive
    // the page dots from these.
    readonly property alias pageCount: wall.pageCount
    readonly property alias currentPage: wall.currentPage
    // Host-facing switch for the DESTRUCTIVE half of the chrome: Clear all /
    // Show all, Blackout, the per-layer clears and the audio transport. Those
    // belong to the Show screen, which is where the operator drives the
    // service; a surface that only needs to watch the outputs (the Edit panel,
    // where they sat between the previews and the tab strip) turns them off and
    // keeps just the transport row — ‹ › and the play/lock/transition actions,
    // which apply to what the previews are showing wherever you are. The
    // toolbar's own height is `transportRow.height + 12`, so dropping the rows
    // shrinks the wall and everything hung below it follows.
    property bool showClearControls: true
    function goTo(page) {
        wall.goTo(page)
    }

    height: wall.height + Math.max(wall.pageCount > 1 ? 12 : 0,
        toolbar.visible ? (wall.pageCount > 1 ? 22 : 8) + toolbar.height : 0)

    // ---- Paged tile wall -----------------------------------------------------
    Flickable {
        id: wall

        anchors.horizontalCenter: parent.horizontalCenter
        y: 0

        readonly property int count: root._wallRev >= 0 ? OutputListModel.rowCount() : 0
        // Page capacity: previews are grouped four to a page.
        readonly property int perPage: count === 1 ? 1 : 4
        readonly property int pageCount: Math.max(1, Math.ceil(count / perPage))
        readonly property real pageW: root.width
        readonly property real columnGap: 0
        // Each page uses a consistent two-column grid when it has multiple
        // previews. A lone output fills its page.
        function itemsOnPage(page) {
            return Math.max(0, Math.min(perPage, count - page * perPage))
        }
        function columnsForPage(page) {
            const items = itemsOnPage(page)
            if (items <= 1) return 1
            return 2
        }
        function slotWidthForPage(page) {
            const columns = columnsForPage(page)
            return (pageW - (columns - 1) * columnGap) / columns
        }
        function rowsForPage(page) {
            return Math.max(1, Math.ceil(itemsOnPage(page) / columnsForPage(page)))
        }
        function slotHeightForPage(page) {
            return 2 + (slotWidthForPage(page) - 2) * 9 / 16
        }
        function pageHeightForPage(page) {
            const rows = rowsForPage(page)
            return rows * slotHeightForPage(page)
        }
        readonly property int columns: columnsForPage(currentPage)

        width: root.width
        height: {
            let tallestPage = 0
            for (let page = 0; page < pageCount; ++page)
                tallestPage = Math.max(tallestPage, pageHeightForPage(page))
            return tallestPage
        }
        contentWidth: pageCount * pageW
        contentHeight: height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 2000
        interactive: pageCount > 1

        property int currentPage: 0
        // Page snap (Flickable has no snapMode): when a drag or a flick
        // ends, settle on the nearest page with a short ease. contentX
        // writes from these handlers are programmatic, not user drags, so
        // no fighting occurs.
        function snapToPage() {
            root.goTo(Math.round(contentX / pageW))
        }
        onDragEnded: snapToPage()
        onFlickEnded: snapToPage()

        NumberAnimation {
            id: snapAnim
            target: wall
            property: "contentX"
            duration: 180
            easing.type: Easing.OutCubic
        }

        function goTo(page) {
            currentPage = Math.max(0, Math.min(pageCount - 1, page))
            snapAnim.to = currentPage * pageW
            snapAnim.restart()
        }
        onPageCountChanged: root.goTo(Math.min(currentPage, pageCount - 1))

        Repeater {
            model: OutputListModel

            delegate: OutputMonitorTile {
                required property int index
                // Hook for the style-background self-test, which reads a tile's
                // own styleBg before and after a style gains a background
                // image (run_style_bg_test.sh) — proof the tile repaints, not
                // just that the model changed.
                objectName: "selfTestMonitorTile" + index
                readonly property int page: wall.count === 1 ? 0 : Math.floor(index / wall.perPage)
                readonly property int slot: wall.count === 1 ? 0 : index % wall.perPage
                readonly property int columns: wall.columnsForPage(page)
                readonly property real tileWidth: wall.slotWidthForPage(page)

                x: page * wall.pageW + (slot % columns) * (tileWidth + wall.columnGap)
                y: Math.floor(slot / columns) * wall.slotHeightForPage(page)
                width: tileWidth
            }
        }
    }

    // Page dots — one per page, current page lit; click to swipe. Drawn
    // BELOW wall.height (outside bounds; root doesn't clip).
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        y: wall.y + wall.height + 6
        visible: wall.pageCount > 1
        spacing: 6

        Repeater {
            model: wall.pageCount

            delegate: Rectangle {
                required property int index

                width: 6; height: 6; radius: 3
                color: index === wall.currentPage ? Theme.accent : Theme.border
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.goTo(parent.index)
                }
            }
        }
    }

    // ---- Preview chrome (FreeShow's ShowActions + ClearButtons, rebuilt) ----
    // Two sections under the preview, like FreeShow's Preview.svelte:
    //   [ < > ▶ 🔒 ◐ ]   ShowActions — previous/next slide, play (go live /
    //                     resume), the output LOCK, and the (informational)
    //                     transition glyph
    //   [ ✕ Clear all ]   ClearButtons' red full-width clear.all — everything
    //                     off air
    //   [🖼 📄 ⊙ 🎵 ⏱]   ClearButtons' group — clear background / slide /
    //                     overlays / audio / timers, always red-tinted
    //                     (FreeShow's own MaterialButtons pass `red`
    //                     unconditionally) but dimmed + inert until that
    //                     SPECIFIC layer has something on air, matching
    //                     ClearButtons.svelte's per-layer `disabled` binds
    //                     exactly, not a shared "anything at all is live" gate.
    // Background and audio now have a REAL independent clear (clearMedia())
    // since the engine grew media playback; slide is still the whole-output
    // stop() (no slide-only clear exists yet); overlays/timers have no
    // engine-exposed active state at all yet, so they stay honestly dim and
    // unclickable rather than faking a lit state.
    Rectangle {
        id: toolbar
        visible: root.anythingOnAir
        anchors.horizontalCenter: parent.horizontalCenter
        y: wall.y + wall.height + (wall.pageCount > 1 ? 22 : 8)
        width: root.width
        height: transportRow.height + 12
        radius: 8
        color: Theme.panelBg
        border.color: Theme.border
        border.width: 1

        readonly property bool atStart: LiveOutputService.onAirIndex <= 0
        readonly property bool atEnd: LiveOutputService.onAirTotal > 0 && LiveOutputService.onAirIndex >= LiveOutputService.onAirTotal - 1
        readonly property bool onAir: LiveOutputService.onAirTotal > 0
        // CAN A PRESS MOVE SOMETHING? That — not "is something on the tile" —
        // is what lights the ‹ ›, so no arrow is ever enabled while doing
        // nothing (a live blank and a staged slide nobody owns both used to
        // leave them lit and inert). Two ways to be steppable:
        //
        //  * the wall steps the live presentation ITSELF — its own slides —
        //    which needs a set with more than one slide (multiSlide); at either
        //    end of such a set the step becomes a PASSAGE step instead, which is
        //    the second case;
        //  * a TAB owns the tile's content and says it can step it
        //    (LiveOutputService.wallSteppable): a reference pane re-picking the
        //    neighbouring passage, the show pane stepping the show's own slides.
        //    Staging is the normal state before GO LIVE, so this is what keeps
        //    the arrows alive there — including for a single-verse pick.
        readonly property bool multiSlide: LiveOutputService.onAirTotal > 1
        readonly property bool canStep: toolbar.multiSlide || LiveOutputService.wallSteppable

        // A toolbar button — one glyph in a PILL (always-visible rounded
        // chip, like FreeShow's MaterialButton tiles) that brightens on
        // hover. FreeShow's own ClearButtons.svelte always tints its
        // per-layer buttons red (the `red` prop is unconditional) and
        // instead "comes alive" purely through `disabled` — vivid + clickable
        // while that layer has something to clear, dimmed + inert the
        // moment it's empty. Non-danger buttons (transport) keep the plain
        // enabled/disabled dimming they already had.
        component ToolButton: Item {
            id: toolBtn
            property string icon: ""
            // What hovering this glyph explains (components/HoverDetail.qml): the
            // title names the action, the detail says what it does here — a row
            // of bare icons is otherwise only decodable by trying them.
            property string tipTitle: ""
            property string tipDetail: ""
            property bool enabled2: true
            property bool danger: false
            // Lit (red) vs "clickable but currently off" — distinct from
            // enabled2 (clickability alone) so a toggle-style button (the
            // scripture clear/resume) can stay clickable while reading as
            // OFF between a clear and a resume. Every other button never
            // sets this, so it just mirrors enabled2 — no change for them.
            property bool active: enabled2
            signal picked()
            width: 36; height: 30
            Rectangle {
                anchors.fill: parent; radius: 8
                color: {
                    if (!toolBtn.enabled2) return Theme.inset
                    if (toolBtn.danger)
                        return toolBtn.active
                            ? (toolArea.hovered ? Qt.alpha(Theme.danger, 0.20) : Qt.alpha(Theme.danger, 0.15))
                            : (toolArea.hovered ? Theme.chip : Theme.inset)
                    return toolArea.hovered ? Theme.chip : Theme.inset
                }
                border.color: {
                    if (!toolBtn.enabled2) return Theme.chip
                    if (toolBtn.danger)
                        return toolBtn.active
                            ? (toolArea.hovered ? Qt.alpha(Theme.danger, 0.40) : Qt.alpha(Theme.danger, 0.20))
                            : (toolArea.hovered ? Qt.alpha(Theme.danger, 0.22) : Qt.alpha(Theme.danger, 0.14))
                    return toolArea.hovered ? Theme.borderSubtle : Theme.chip
                }
                border.width: 1
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            }
            IconGlyph {
                anchors.centerIn: parent
                name: toolBtn.icon
                color: !toolBtn.enabled2 ? Theme.textMuted
                     : (toolBtn.danger ? (toolBtn.active ? Theme.dangerLight : Theme.textMuted) : Theme.textPrimary)
                width: root.toolIconSize; height: root.toolIconSize; fit: true
                strokeWidth: root.toolIconStroke
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
            }
            HoverHandler { id: toolArea; cursorShape: toolBtn.enabled2 ? Qt.PointingHandCursor : Qt.ArrowCursor }
            TapHandler { onTapped: if (toolBtn.enabled2) toolBtn.picked() }
            // The shared hover detail, laid OVER the button: it takes no clicks
            // (HoverDetail's click sink is off), so the TapHandler above still
            // gets every press.
            HoverDetail {
                anchors.fill: parent
                title: toolBtn.tipTitle
                detail: toolBtn.tipDetail
            }
        }

        // A full-width row button — the Clear all / Show all PAIR below. Same
        // accent-tinted pill the old full-width Clear all was, plus the
        // disabled dimming the per-layer row's buttons use, so "Show all" with
        // nothing to bring back reads as unavailable rather than as a control
        // that silently does nothing.
        component RowButton: Item {
            id: rowBtn
            property string icon: ""
            property string caption: ""
            property string tipTitle: ""
            property string tipDetail: ""
            property color tint: Theme.dangerLight
            property bool enabled2: true
            signal picked()
            height: 28
            Rectangle {
                anchors.fill: parent; radius: 8
                color: !rowBtn.enabled2 ? Theme.inset : Qt.alpha(Theme.accent, 0.11)
                border.color: !rowBtn.enabled2 ? Theme.chip : Qt.alpha(Theme.accent, 0.20)
                border.width: 1

                // The pointer's light is the app's shared hover layer (the
                // disabled state above stays the button's own).
                HoverGlow {
                    anchors.fill: parent
                    radius: parent.radius
                    hovered: rowArea.hovered && rowBtn.enabled2
                    glowColor: Qt.alpha(Theme.accent, 0.07)
                    edge: true
                    edgeColor: Qt.alpha(Theme.accent, 0.20)
                }
            }
            Row {
                anchors.centerIn: parent
                spacing: 6
                IconGlyph {
                    name: rowBtn.icon
                    color: rowBtn.enabled2 ? rowBtn.tint : Theme.textMuted
                    width: root.toolIconSize; height: root.toolIconSize; fit: true
                    strokeWidth: root.toolIconStroke
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: rowBtn.caption
                    color: rowBtn.enabled2 ? rowBtn.tint : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            HoverHandler { id: rowArea; cursorShape: rowBtn.enabled2 ? Qt.PointingHandCursor : Qt.ArrowCursor }
            TapHandler { onTapped: if (rowBtn.enabled2) rowBtn.picked() }
            HoverDetail {
                anchors.fill: parent
                title: rowBtn.tipTitle !== "" ? rowBtn.tipTitle : rowBtn.caption
                detail: rowBtn.tipDetail
            }
        }

        Column {
            id: transportRow
            x: 10; y: 8
            width: parent.width - 20
            spacing: 6

            // ShowActions: [previous next play lock transition] — spread
            // across the full strip (FreeShow's buttons are flex-grow: 1, so
            // every action owns an equal slot instead of huddling mid-bar).
            // Explicit spacing + width accounting for it (rather than a bare
            // width/5 with no spacing) so the five pills sit as distinct,
            // evenly-gapped buttons instead of one edge-to-edge strip.
            Row {
                width: parent.width
                spacing: 6
                readonly property real btnW: (width - spacing * 4) / 5

                // ‹ › (FreeShow's OutputHelper.advanceOutputs): a step within
                // the on-air set, or — at either end of it — a PASSAGE step:
                // the tab whose content is on air re-picks the neighbouring
                // passage and replays it, so the arrows keep driving the
                // output preview even on a single-verse pick (like the
                // preview pane's own pills).
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "previous"
                    // Dead only mid-set with nowhere back (a multi-slide pick
                    // already on the first slide): a single-verse pick keeps
                    // the arrow (it passage-steps).
                    enabled2: toolbar.canStep && !(toolbar.multiSlide && toolbar.atStart)
                    onPicked: LiveOutputService.stepPassage(-1)
                    objectName: "selfTestWallPrev"
                    tipTitle: qsTr("Previous")
                    tipDetail: qsTr("Step back to the previous slide or passage")
                }
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "next"
                    enabled2: toolbar.canStep && !(toolbar.multiSlide && toolbar.atEnd)
                    onPicked: LiveOutputService.stepPassage(1)
                    objectName: "selfTestWallNext"
                    tipTitle: qsTr("Next")
                    tipDetail: qsTr("Step forward to the next slide or passage")
                }
                // Play: the explicit GO LIVE trigger while nothing is on
                // air yet (commits whatever's staged — see
                // LiveOutputService.goLive's staged-pick precedence — or
                // falls back to the open show); once live, its job changes
                // to restarting the on-air set from its first slide. Always
                // enabled: it IS the control for deciding when go-live
                // actually activates, not something only usable after it
                // already has.
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "play"
                    enabled2: true
                    onPicked: {
                        if (!LiveOutputService.live) { LiveOutputService.goLive(); return }
                        LiveOutputService.jumpTo(0)
                    }
                    tipTitle: qsTr("Play")
                    tipDetail: LiveOutputService.live
                               ? qsTr("Restart the on-air set from its first slide")
                               : qsTr("Go live with whatever is staged")
                }
                // Lock: Ctrl+L in FreeShow — arms the "nothing can change the
                // output" state; the engine has no output lock yet, so the
                // button shows the unlocked state and is inert (glyph dimmed,
                // pill + hover intact).
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "unlocked"
                    enabled2: false
                    tipTitle: qsTr("Lock output")
                    tipDetail: qsTr("Not available yet")
                }
                // Transition: FreeShow opens its transition popup; there is
                // no engine transition editor yet, so the glyph is a marker.
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "transition"
                    enabled2: false
                    tipTitle: qsTr("Transition")
                    tipDetail: qsTr("Not available yet")
                }
            }

            // Clear all + its undo (ClearButtons' red full-width clear.all,
            // split in two now).
            //
            // WHY AN UNDO EXISTS: Clear all is the wall's one destructive
            // control, and it used to be a dead end — whatever it took off the
            // monitor was gone ("the clear all button should have showall or
            // something so when it click ... it bring the things back"). The
            // whole action and its snapshot now live behind ONE service call,
            // so the two can't drift apart; the button no longer runs the four
            // clears itself.
            Row {
                width: parent.width; height: visible ? 28 : 0
                visible: root.showClearControls
                spacing: 6

                RowButton {
                    width: (parent.width - parent.spacing) / 2
                    icon: "clearIcon"
                    caption: qsTr("Clear all")
                    // The one destructive action on this bar — it keeps the
                    // red-ish tint it always had. Never touches GO LIVE/STOP:
                    // live_ is the ONE thing only the user's own GO LIVE/STOP
                    // press controls (see LiveOutputService::clearAll).
                    onPicked: LiveOutputService.clearAll()
                    tipDetail: qsTr("Clear everything on air — slide, media, overlays and audio")
                }
                RowButton {
                    width: (parent.width - parent.spacing) / 2
                    icon: "layers"
                    caption: qsTr("Show all")
                    // Lit only while a Clear all is undoable; consumed by the
                    // press, so the pair can't be pressed twice for one clear.
                    enabled2: LiveOutputService.canRestoreCleared
                    tint: Theme.textPrimary
                    onPicked: LiveOutputService.restoreCleared()
                    tipDetail: qsTr("Bring back what the last Clear all took off air")
                }
            }

            // BLACKOUT — the deliberate "nothing at all on screen" (see
            // LiveOutputService::blackout). It is separate from Clear all on
            // purpose: Clear all takes the CONTENT down and leaves the output's
            // own branded look up, while a blackout takes even that away, which
            // is a thing you do to an audience mid-service and must never
            // happen as a side effect of clearing a slide.
            //
            // LATCHING: while the carrier is on air the row says so and the
            // same press puts back exactly what was showing (the engine keeps
            // running; only the picture came down). Lit while blacked, dimmed
            // and inert off air — there is no screen to blank when nothing is
            // going out, and LIVE stays the user's own GO LIVE/STOP decision.
            RowButton {
                width: parent.width
                height: visible ? 28 : 0
                visible: root.showClearControls
                icon: "ban"
                caption: LiveOutputService.blackedOut ? qsTr("End blackout")
                                                      : qsTr("Blackout")
                tint: LiveOutputService.blackedOut ? Theme.textPrimary : Theme.dangerLight
                enabled2: root.live
                objectName: "selfTestWallBlackout"
                onPicked: {
                    if (LiveOutputService.blackedOut) LiveOutputService.endBlackout()
                    else LiveOutputService.blackout()
                }
                tipDetail: LiveOutputService.blackedOut
                           ? qsTr("Bring the picture back — the show keeps running")
                           : qsTr("Hide the picture without stopping the show")
            }

            // ClearButtons' per-layer group (image/slide/overlays/audio/timer)
            // — same equal-slot spreading as the transport row. Background
            // and audio now have a REAL per-layer clear (clearMedia()/
            // clearInput(), independent of the on-air slide) since the
            // engine grew media playback; slide still only has the
            // whole-output stop(). Overlays/timers have no engine-exposed
            // active state or per-layer clear yet — same honest "stays dim,
            // stays inert" treatment as the lock/transition transport
            // buttons above, not a fake lit state or a clear that's really
            // just Clear All in disguise.
            Row {
                width: parent.width; height: visible ? 30 : 0
                visible: root.showClearControls
                spacing: 6
                readonly property real btnW: (width - spacing * 4) / 5

                // Background/media: video, image, OR a taken camera/screen
                // input — whichever one the compositor's background layer
                // is currently showing.
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "image"
                    danger: true
                    enabled2: (LiveOutputService.mediaOnAir && !LiveOutputService.mediaIsAudio) || root.inputTaken
                    // Only the PICTURE's media here: an audio file is the audio
                    // button's to clear (it has its own transport above), so
                    // "Clear background" must not silently stop the music.
                    onPicked: {
                        if (LiveOutputService.mediaOnAir && !LiveOutputService.mediaIsAudio) LiveOutputService.clearMedia()
                        if (root.inputTaken) LiveOutputService.clearInput()
                    }
                    tipTitle: qsTr("Clear background")
                    tipDetail: qsTr("Clear the on-air media, or a taken camera/screen input")
                }
                // Slide: the on-air text/scripture/song content indicator —
                // same shape as the image/overlays buttons on either side of
                // it now (enabled2 driven purely by "is my own content here
                // right now", onPicked just clears it, no active override,
                // no resume/live special-casing). The earlier "let it also
                // resume the last go-live when nothing's on/staged" behaviour
                // is gone — live report: "the book icon is not anywhere
                // special, remove it from all special places and let it be
                // among the pack" (it also never touches GO LIVE/STOP,
                // exactly like every other button in this row never does).
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "scripture"
                    danger: true
                    enabled2: root.hasRealOnAirContent || root.staged
                    onPicked: {
                        if (root.staged) LiveOutputService.clearStaged()
                        if (root.hasRealOnAirContent) LiveOutputService.clearOnAirSlide()
                    }
                    tipTitle: qsTr("Clear slide")
                    tipDetail: qsTr("Clear the on-air text, scripture or song content")
                }
                // Overlays: the whole stacked layer off at once (no
                // per-overlay button in this 5-slot row — LiveOutputService
                // now genuinely tracks which overlays are live).
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "overlays"
                    danger: true
                    enabled2: LiveOutputService.activeOverlays.length > 0
                    onPicked: LiveOutputService.clearAllOverlays()
                    tipTitle: qsTr("Clear overlays")
                    tipDetail: qsTr("Take every active overlay off air")
                }
                // Audio: a taken audio FILE (real program audio) OR a live
                // input/bus meter tap (the Media pane's audio/bus double-
                // click interim — see MediaLibraryPane.qml's onDoubleClicked
                // comment; it's what's actually moving the tile's L/R
                // meters in that case). anyInputMetering, i.e. a tap somebody
                // actually asked for — never "a meter is running somewhere",
                // which would light this button up any time the show is simply
                // live (that broader reading used to include the program-mix
                // meter, now removed with the engine-audio path).
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "audio"
                    danger: true
                    // Audio CONTENT the wall is playing: a taken audio file, or a
                    // taken input. It used to also light on anyInputMetering —
                    // "a tap is running somewhere in the app" — which is how it
                    // came on while a settings row was merely being set up, and
                    // how it stayed on through Clear all (see anythingOnAir).
                    enabled2: (LiveOutputService.mediaOnAir && LiveOutputService.mediaIsAudio)
                              || root.inputTaken
                    onPicked: {
                        if (LiveOutputService.mediaOnAir) LiveOutputService.clearMedia()
                        // Releasing all input taps here also stopped meters
                        // owned by the Media pane, while its cards still
                        // appeared to be monitoring. Settings reads those
                        // same shared snapshots, so leave their taps alone;
                        // LiveOutputService releases only its own media take.
                    }
                    tipTitle: qsTr("Clear audio")
                    tipDetail: qsTr("Clear the on-air audio file or a taken input")
                }
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "timerFill"
                    danger: true
                    enabled2: false
                    tipTitle: qsTr("Clear timer")
                    tipDetail: qsTr("Not available yet")
                }
            }

            // ---- Audio transport -----------------------------------------
            // FreeShow's scrubber under the preview, kept honest: it appears
            // ONLY while the wall is playing an audio FILE (mediaOnAir &&
            // mediaIsAudio). A text slide, a video or a taken camera carries
            // no dead play/seek row — audio that was never picked has no
            // scrubber to show. height collapses to 0 when hidden so the
            // Column (and therefore the wall's own height) shrinks with it.
            Item {
                id: audioBar
                objectName: "selfTestWallAudioBar"
                width: parent.width
                height: visible ? 28 : 0
                visible: root.showClearControls && LiveOutputService.mediaOnAir && LiveOutputService.mediaIsAudio

                readonly property bool playing: LiveOutputService.mediaState === "playing"
                readonly property real progress: LiveOutputService.mediaDuration > 0
                    ? Math.max(0, Math.min(1, LiveOutputService.mediaPosition / LiveOutputService.mediaDuration))
                    : 0

                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: Theme.inset
                    border.color: Theme.chip
                    border.width: 1
                }

                // play / pause — the same engine call as the Media pane's own
                // play button (mediaTogglePlay), so the two can never disagree
                // about whether the file is running.
                Item {
                    id: audioToggle
                    x: 8; y: 0
                    width: 22; height: parent.height
                    IconGlyph {
                        anchors.centerIn: parent
                        name: audioBar.playing ? "pause" : "play"
                        color: Theme.dangerLight
                        width: root.toolIconSize; height: root.toolIconSize; fit: true
                        strokeWidth: root.toolIconStroke
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: LiveOutputService.mediaTogglePlay() }
                }

                Text {
                    id: audioElapsed
                    anchors.left: audioToggle.right
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: LiveOutputService.formatMediaTime(LiveOutputService.mediaPosition)
                    color: Theme.dangerLight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                Text {
                    id: audioTotal
                    anchors.right: audioMute.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: LiveOutputService.formatMediaTime(LiveOutputService.mediaDuration)
                    color: Theme.dangerLight
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }

                // Seek track: click or drag anywhere on it to jump. The knob
                // is the position, not an animation, so it reads the same as
                // the elapsed time beside it.
                Item {
                    id: seekTrack
                    anchors.left: audioElapsed.right
                    anchors.right: audioTotal.left
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    height: 16

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 4; radius: 2
                        color: Theme.chip
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * audioBar.progress
                        height: 4; radius: 2
                        color: Theme.danger
                    }
                    Rectangle {
                        x: Math.max(0, Math.min(parent.width - width, parent.width * audioBar.progress - width / 2))
                        anchors.verticalCenter: parent.verticalCenter
                        width: 9; height: 9; radius: 4.5
                        color: Theme.dangerLight
                    }

                    function seekTo(x) {
                        if (seekTrack.width <= 0) return
                        LiveOutputService.mediaSeek(Math.max(0, Math.min(1, x / seekTrack.width)))
                    }
                    TapHandler {
                        onTapped: function (point) { seekTrack.seekTo(point.position.x) }
                    }
                    DragHandler {
                        onCentroidChanged: if (active) seekTrack.seekTo(centroid.position.x)
                    }
                }

                // Mute — dimmed glyph means muted, the same read the Media
                // pane's speaker uses.
                Item {
                    id: audioMute
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    width: 22; height: parent.height
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "volume2"
                        color: LiveOutputService.mediaMuted ? Theme.textMuted : Theme.dangerLight
                        width: root.toolIconSize; height: root.toolIconSize; fit: true
                        strokeWidth: root.toolIconStroke
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: LiveOutputService.mediaToggleMuted() }
                }
            }
        }
    }

    // The wall's total height must account for the dots row's visual space
    // even when the toolbar is hidden (hosts hang content +22 below).
    //
    // Qt.binding(), NOT a bare assignment: `root.height = ...` here would
    // destroy the declarative height binding at the top of this file, and the
    // wall would then stay frozen at whatever height it had when visibility
    // last changed. Anything on air afterwards grew the toolbar INSIDE that
    // frozen height, so the toolbar (and its per-layer tiles) drew on top of
    // whatever the host had laid out below the wall — the Edit screen's SLIDE
    // label and Background row were the visible casualties. Re-asserting the
    // same expression as a binding keeps the recompute-on-show intent without
    // killing the live dependency on toolbar.visible.
    onVisibleChanged: if (visible)
        root.height = Qt.binding(() => wall.height + Math.max(wall.pageCount > 1 ? 12 : 0,
            toolbar.visible ? (wall.pageCount > 1 ? 22 : 8) + toolbar.height : 0))
}
