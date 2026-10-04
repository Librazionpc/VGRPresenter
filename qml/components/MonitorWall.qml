import QtQuick
import VGRPresenterUI

// The output monitor wall — ONE component, TWO hosts (Show screen right
// column, Edit screen ITEMS tab). FreeShow's preview chrome, adapted:
//
//   [• Main Output] [• Stage]        <- output tabs (click = that output
//   [  GO LIVE  ]                     becomes the on-air one)
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
    readonly property bool anythingOnAir: root.live || root.inputTaken || root.staged
        || LiveOutputService.mediaOnAir || LiveOutputService.activeOverlays.length > 0
        || EngineBridge.anyAudioMetering

    // External contract: hosts position content under the wall and drive
    // the page dots from these.
    readonly property alias pageCount: wall.pageCount
    readonly property alias currentPage: wall.currentPage
    function goTo(page) {
        wall.goTo(page)
    }

    height: tabs.height + wall.height + (toolbar.visible ? toolbar.height : 0)

    // ---- Output tabs (FreeShow's PreviewOutputs tab strip) -----------------
    // GO LIVE lives in the app header now (user call) — the wall keeps only
    // the per-output tabs.
    Item {
        id: tabs
        anchors.horizontalCenter: parent.horizontalCenter
        y: 0
        width: wall.pageW
        height: wall.rowsPerPage * 26
        clip: true

        Repeater {
            model: root._wallRev >= 0 ? OutputListModel.rowCount() : 0

            delegate: Rectangle {
                id: tab
                required property int index
                // getOutput() is an invokable snapshot, so explicitly depend
                // on the model's data revision to refresh enabled/active state.
                readonly property var out: {
                    root._wallRev
                    return OutputListModel.getOutput(index)
                }
                readonly property bool isCurrent: tab.out.active === true
                readonly property int slot: index % wall.perPage
                readonly property int page: Math.floor(index / wall.perPage)
                readonly property int columns: wall.columnsForPage(page)
                readonly property real tileWidth: wall.slotWidthForPage(page)

                x: page * wall.pageW + (slot % columns) * (tileWidth + wall.columnGap) - wall.contentX
                y: Math.floor(slot / columns) * 26
                width: tileWidth
                height: 26
                radius: 0
                color: isCurrent ? Theme.activeBg : (tabArea.containsMouse ? Theme.hoverBg : Theme.panelBg)
                border.color: isCurrent ? Theme.borderSubtle : Theme.border
                border.width: 1

                Row {
                    anchors.centerIn: parent
                    spacing: 6

                    // State dot: green = the selected active output, grey =
                    // inactive. Enabled/disabled controls whether the tab can
                    // be selected; it is not the on-air status.
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 7; height: 7; radius: 3.5
                        color: tab.isCurrent ? Theme.success : Theme.textMuted
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, tab.width - 30)
                        text: tab.out.name
                        color: tab.isCurrent ? Theme.textPrimary : Theme.textSecondary
                        elide: Text.ElideRight
                        font.family: "Segoe UI"
                        font.pixelSize: 11
                        font.bold: tab.isCurrent
                    }
                }

                // Disabled screens can't take air — the click is inert.
                MouseArea {
                    id: tabArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: tab.out.isEnabled ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: if (tab.out.isEnabled) {
                        OutputListModel.setActive(tab.index)
                        root.goTo(Math.floor(tab.index / wall.perPage))
                    }
                }
            }
        }
    }

    // ---- Paged tile wall -----------------------------------------------------
    Flickable {
        id: wall

        anchors.horizontalCenter: parent.horizontalCenter
        y: tabs.height

        readonly property int count: root._wallRev >= 0 ? OutputListModel.rowCount() : 0
        // Page capacity: a lone output gets a hero page of its own.
        readonly property int perPage: count === 1 ? 1 : 4
        readonly property int pageCount: Math.max(1, Math.ceil(count / perPage))
        readonly property real pageW: 376
        readonly property real columnGap: 0
        // Each page sizes itself to its own roster. A lone output fills the
        // page; two use two columns; three use three columns; four use 2×2.
        // This also lets a partial final page grow (for example output 5).
        function itemsOnPage(page) {
            return Math.max(0, Math.min(perPage, count - page * perPage))
        }
        function columnsForPage(page) {
            const items = itemsOnPage(page)
            if (items <= 1) return 1
            if (items === 3) return 3
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
        readonly property int rowsPerPage: rowsForPage(currentPage)

        width: 376
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
                Behavior on color { ColorAnimation { duration: 120 } }

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
        width: 376
        height: transportRow.height + 12
        radius: 8
        color: Theme.panelBg
        border.color: Theme.border
        border.width: 1

        readonly property bool atStart: LiveOutputService.onAirIndex <= 0
        readonly property bool atEnd: LiveOutputService.onAirTotal > 0 && LiveOutputService.onAirIndex >= LiveOutputService.onAirTotal - 1
        readonly property bool onAir: LiveOutputService.onAirTotal > 0
        // A single-slide pick (one verse) makes BOTH ends true — but the
        // ‹ › must stay live: at the ends they become PASSAGE steps (the
        // tab that owns the air re-picks the neighbouring passage), the
        // same way the preview pane's own pills advance it.
        readonly property bool multiSlide: LiveOutputService.onAirTotal > 1

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
                            ? (toolArea.containsMouse ? Qt.alpha(Theme.danger, 0.20) : Qt.alpha(Theme.danger, 0.15))
                            : (toolArea.containsMouse ? Theme.chip : Theme.inset)
                    return toolArea.containsMouse ? Theme.chip : Theme.inset
                }
                border.color: {
                    if (!toolBtn.enabled2) return Theme.chip
                    if (toolBtn.danger)
                        return toolBtn.active
                            ? (toolArea.containsMouse ? Qt.alpha(Theme.danger, 0.40) : Qt.alpha(Theme.danger, 0.20))
                            : (toolArea.containsMouse ? Qt.alpha(Theme.danger, 0.22) : Qt.alpha(Theme.danger, 0.14))
                    return toolArea.containsMouse ? Theme.borderSubtle : Theme.chip
                }
                border.width: 1
                Behavior on color { ColorAnimation { duration: 100 } }
            }
            IconGlyph {
                anchors.centerIn: parent
                name: toolBtn.icon
                color: !toolBtn.enabled2 ? Theme.textMuted
                     : (toolBtn.danger ? (toolBtn.active ? Theme.dangerLight : Theme.textMuted) : Theme.textPrimary)
                width: 15; height: 15; fit: true
                Behavior on color { ColorAnimation { duration: 100 } }
            }
            HoverHandler { id: toolArea; cursorShape: toolBtn.enabled2 ? Qt.PointingHandCursor : Qt.ArrowCursor }
            TapHandler { onTapped: if (toolBtn.enabled2) toolBtn.picked() }
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
                    enabled2: toolbar.onAir && !(toolbar.multiSlide && toolbar.atStart)
                    onPicked: LiveOutputService.stepPassage(-1)
                }
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "next"
                    enabled2: toolbar.onAir && !(toolbar.multiSlide && toolbar.atEnd)
                    onPicked: LiveOutputService.stepPassage(1)
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
                }
                // Lock: Ctrl+L in FreeShow — arms the "nothing can change the
                // output" state; the engine has no output lock yet, so the
                // button shows the unlocked state and is inert (glyph dimmed,
                // pill + hover intact).
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "unlocked"
                    enabled2: false
                }
                // Transition: FreeShow opens its transition popup; there is
                // no engine transition editor yet, so the glyph is a marker.
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "transition"
                    enabled2: false
                }
            }

            // Clear all (ClearButtons' red full-width clear.all).
            Item {
                width: parent.width; height: 28
                Rectangle {
                    anchors.fill: parent; radius: 8
                    color: clearAllArea.containsMouse ? Qt.alpha(Theme.accent, 0.18) : Qt.alpha(Theme.accent, 0.11)
                    border.color: clearAllArea.containsMouse ? Qt.alpha(Theme.accent, 0.40) : Qt.alpha(Theme.accent, 0.20)
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }
                }
                Row {
                    anchors.centerIn: parent
                    spacing: 6
                    IconGlyph { name: "clearIcon"; color: Theme.dangerLight; width: 13; height: 13; anchors.verticalCenter: parent.verticalCenter }
                    Text { text: qsTr("Clear all"); color: Theme.dangerLight; font.family: "Segoe UI"; font.pixelSize: 12; font.weight: Font.Medium; anchors.verticalCenter: parent.verticalCenter }
                }
                HoverHandler { id: clearAllArea; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        // A taken input is one of the things "Clear all"
                        // clears — releasing it here (without leaving the
                        // wall for the Media pane) is the discoverable path.
                        if (LiveOutputService.inputLabel !== "")
                            LiveOutputService.clearInput()
                        if (LiveOutputService.activeOverlays.length > 0)
                            LiveOutputService.clearAllOverlays()
                        // Never touches GO LIVE/STOP itself anymore — same
                        // rule as every per-layer icon in this row now:
                        // live_ is the ONE thing only the user's own GO
                        // LIVE/STOP press controls. Clears the on-air SLIDE
                        // content instead (same as the book icon), leaving
                        // the output live and showing the style's own
                        // background in its place.
                        if (root.hasRealOnAirContent)
                            LiveOutputService.clearOnAirSlide()
                        // A STAGED pick (double-clicked, not yet committed
                        // by GO LIVE) is independent of live_ — it kept
                        // showing in the tile after Clear All otherwise.
                        if (root.staged)
                            LiveOutputService.clearStaged()
                    }
                }
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
                width: parent.width
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
                    onPicked: {
                        if (LiveOutputService.mediaOnAir) LiveOutputService.clearMedia()
                        if (root.inputTaken) LiveOutputService.clearInput()
                    }
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
                }
                // Audio: a taken audio FILE (real program audio) OR a live
                // input/bus meter tap (the Media pane's audio/bus double-
                // click interim — see MediaLibraryPane.qml's onDoubleClicked
                // comment; it's what's actually moving the tile's L/R
                // meters in that case). anyInputMetering, not the broader
                // anyAudioMetering: that also covers the program-mix tap
                // every active/live tile runs automatically regardless of
                // whether there is any real audio, which would light this
                // button up any time the show is simply live.
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "audio"
                    danger: true
                    enabled2: (LiveOutputService.mediaOnAir && LiveOutputService.mediaIsAudio)
                              || EngineBridge.anyInputMetering
                    onPicked: {
                        if (LiveOutputService.mediaOnAir) LiveOutputService.clearMedia()
                        EngineBridge.stopAllInputMeters()
                    }
                }
                ToolButton {
                    width: parent.btnW; height: 30
                    icon: "timerFill"
                    danger: true
                    enabled2: false
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
        root.height = Qt.binding(() => tabs.height + wall.height + (toolbar.visible ? toolbar.height : 0))
}
