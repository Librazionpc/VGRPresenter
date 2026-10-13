import QtQuick

// A thin vertical scrollbar for a Flickable/ListView. QtQuick.Controls'
// ScrollBar doesn't match this app's track/thumb visual language, so this
// is a small from-scratch replacement — hand it a `flickable` and it does
// the rest: computes its own thumb length/position from contentHeight,
// supports drag-to-scroll, and clicking the track jumps/pages. Fully
// interactive: the thumb grabs anywhere ON it (with a fat invisible grip
// margin so a thin bar is still easy to grab), and a track click JUMPS the
// content so the thumb centers on the click (a page step left the click
// target non-sticky — the classic "I clicked where I wanted to go" expect-
// ation; shift-click keeps the old page-up/page-down behavior).
// Reusable anywhere a scrollable list needs one instead of each screen
// hand-rolling its own track/thumb math.
//
// SLEEK WHEN IDLE: nothing happening leaves the picture entirely — the bar
// fades out and comes back the moment the user scrolls, hovers its strip, or
// grabs the thumb. It is an OVERLAY, so what it fades is OPACITY, never
// `visible`: a caller must be able to size its layout from this bar without the
// two chasing each other through Qt's polish pass (see the hang-class note on
// `visible` below), and opacity is not part of layout. While it is faded out
// its interactive areas are disabled too, so an invisible bar can never swallow
// a click meant for the row underneath it.
Item {
    id: root

    property Flickable flickable
    property int trackWidth: 8
    property color trackColor: "#161823"
    property color thumbColor: "#3a3f4d"
    property int minThumbLength: 24
    // Horizontal mode: laid out along the BOTTOM, thumb maps contentX
    // (RoutingMatrixModal's sideways matrix). Everything else — fat grip,
    // hover, jump-to-click, drag — behaves identically.
    property bool horizontal: false
    // Fat invisible hover/drag zone around the thumb: an 8px bar with a 5px
    // margin is an 18px-wide grab target — mouse-reachable without pixel
    // hunting. The VISIBLE thumb stays trackWidth; only the hit area grows.
    property int gripMargin: 5
    // True (default): a track click JUMPS so the thumb centers on the click.
    // False (page mode, opt-in): click above/below pages by one viewport.
    property bool jumpToClick: true

    // ---- Auto-hide ---------------------------------------------------------
    // autoHide false pins the bar on screen (a caller that wants a permanent
    // position indicator, e.g. next to a long document). idleOpacity is what
    // "hidden" fades to — 0 by default, and a caller in a busier surface can
    // leave a ghost (0.15) instead. `revealMs` is the tail: the bar stays up for
    // that long after the last scroll so a wheel burst does not blink it out
    // between notches. It is deliberately generous (1.6 s) and fadeOutMs is a
    // slow fade rather than a snap: at 900 / 320 the bar read as snatching
    // itself away the instant a scroll settled.
    //
    // `revealCoalesceMs` is the anti-churn window. A scroll delivers a content
    // change PER FRAME, so re-arming the tail on every one of them restarted a
    // QTimer dozens of times a second on the GUI thread — during the exact
    // moment the user is scrolling — which is what made scrolling feel laggy.
    // A repeat reveal inside the window is a no-op; the tail's deadline is only
    // pushed out once per window (see reveal()).
    property bool autoHide: true
    property real idleOpacity: 0.0
    property int revealMs: 1600
    property int revealCoalesceMs: 220
    property int fadeOutMs: 480
    property int fadeInMs: 90

    // "The user is using me right now". Every term is LATCHED by an event or by
    // an explicit call — none is read back from the MouseAreas below, whose
    // `enabled` comes from `revealed`, which comes from these:
    //   * `pointerOnBar` — the two HoverHandlers. NEVER `thumbArea.containsMouse`:
    //     that closes `revealed` -> `enabled` -> `containsMouse` -> ... , the
    //     "binding loop detected for property barBusy" report.
    //   * `holding` — a press on the bar or its thumb, latched on press and
    //     cleared on release/cancel. Reading `thumbArea.pressed`/`drag.active`
    //     here would both re-close that loop and flip once per frame while a
    //     drag runs. It also means a press that holds still keeps the bar (and
    //     the drag) alive past the reveal tail, which reading the tail alone
    //     never did.
    //   * `revealing` — the reveal tail, written only by reveal()/its Timer.
    //
    // `flickable.moving`/`dragging` is deliberately NOT a term any more. It made
    // visibility depend on a signal that belongs to the SCROLL, not to the bar:
    // a burst re-decides it between notches, and every change re-targets the
    // opacity animation under the pointer. The content changes the scroll itself
    // delivers keep the tail armed, so the bar stays up for the whole burst
    // without being asked to re-decide mid-flight.
    //
    // MEASURED (tools-side scroll bench, 2026-10): a 1.4 s synthetic fast-scroll
    // burst over 200 rows showed `revealed` flipping ONCE per variant with this
    // and without it, and no difference in frame cost — so this term was NOT
    // proven to be the lag. It is removed because it is the wrong shape (scroll
    // state in the bar's own visibility), not because it measured hot.
    readonly property bool pointerOnBar: trackHover.hovered || thumbHover.hovered
    property bool holding: false
    property bool revealing: false
    // When the tail was last (re)armed — what revealCoalesceMs measures against.
    property double lastRevealAt: 0
    // A deferred reveal is queued (see reveal()).
    property bool revealPending: false
    // Revealed = visible AND interactive. A faded-out bar hands its strip back
    // to whatever is underneath (see the MouseAreas' own `enabled`).
    readonly property bool revealed: !autoHide
        || (root.scrollable && (pointerOnBar || holding || revealing))
    // Hover is an event too — latch the tail on it rather than deriving from it.
    onPointerOnBarChanged: if (root.pointerOnBar) root.reveal()

    // DEFERRED, COALESCED, IDEMPOTENT. Content changes arrive from inside the
    // Flickable's own update pass, so writing `revealing` — a dependency of
    // `revealed`/`opacity`/`enabled` — from that handler re-entered the very
    // update that produced it. Qt.callLater runs this once AFTER that pass, no
    // matter how many calls arrived, and the coalesce window keeps the Timer
    // restart off the per-frame path.
    function reveal() {
        if (root.revealPending)
            return
        root.revealPending = true
        Qt.callLater(root.applyReveal)
    }
    // The actual state write. `revealing` is set on every run — that flag is
    // what holds the bar up — while the tail is re-armed only when it has run
    // out or once per revealCoalesceMs.
    function applyReveal() {
        root.revealPending = false
        if (!root.revealing)
            root.revealing = true
        const now = Date.now()
        if (revealTail.running && now - root.lastRevealAt < root.revealCoalesceMs)
            return
        root.lastRevealAt = now
        revealTail.restart()
    }
    Timer {
        id: revealTail
        interval: root.revealMs
        onTriggered: root.revealing = false
    }
    // A wheel notch (or any other content move) is a use: hold the bar up for
    // the tail's length. `moving` alone misses a single wheel step that lands
    // and settles within the same frame.
    Connections {
        target: root.flickable
        function onContentYChanged() { if (!root.horizontal) root.reveal() }
        function onContentXChanged() { if (root.horizontal) root.reveal() }
    }

    opacity: root.autoHide && !root.revealed ? root.idleOpacity : 1
    Behavior on opacity {
        NumberAnimation {
            duration: root.autoHide && !root.revealed ? root.fadeOutMs : root.fadeInMs
            easing.type: Easing.OutCubic
        }
    }

    // Length along the bar = width (vertical) or height (horizontal); the
    // caller sizes that dimension. The CROSS dimension is the track width.
    width: horizontal ? implicitLength : trackWidth
    height: horizontal ? trackWidth : implicitLength
    property int implicitLength: 8   // caller overrides along the bar axis
    // Is there anything to scroll at all? One expression, because `visible` and
    // the auto-hide reveal must never disagree about it.
    readonly property bool scrollable: !!flickable && (horizontal
        ? flickable.contentWidth > flickable.width + 1
        : flickable.contentHeight > flickable.height + 1)
    visible: root.scrollable

    // NOTE (hang class): a caller must NEVER size its layout from this bar's
    // `visible` — doing so lets the bar's own visibility move the content it is
    // measuring, and the two chase each other forever inside Qt's polish pass
    // (GUI thread spin, AppHangB1). The bar is an overlay: reserve its strip
    // unconditionally (see VGRPresenterMainScreen's shows sidebar, ReferencePane's
    // preview column and EditScreen's text panel).

    readonly property real maxScroll: !flickable ? 1
        : horizontal ? Math.max(flickable.contentWidth - flickable.width, 1)
                     : Math.max(flickable.contentHeight - flickable.height, 1)
    readonly property real thumbLength: !flickable ? 0
        : horizontal ? Math.max(minThumbLength, root.width * flickable.width / Math.max(flickable.contentWidth, 1))
                     : Math.max(minThumbLength, root.height * flickable.height / Math.max(flickable.contentHeight, 1))

    Rectangle {
        anchors.fill: parent
        radius: root.trackWidth / 2
        color: trackHover.hovered || thumbArea.drag.active ? Qt.lighter(root.trackColor, 1.6) : root.trackColor

        // `margin` extends the hover zone over the thumb's grip, which the
        // old `thumbArea.containsMouse` term used to cover.
        HoverHandler {
            id: trackHover
            margin: root.gripMargin
        }
    }

    function scrollPos() {
        if (!root.flickable) return 0
        return root.horizontal ? root.flickable.contentX : root.flickable.contentY
    }
    function setScrollPos(v) {
        if (!root.flickable) return
        if (root.horizontal) root.flickable.contentX = v
        else root.flickable.contentY = v
    }
    function page(direction) {
        if (!root.flickable) return
        const step = root.horizontal ? root.flickable.width : root.flickable.height
        root.setScrollPos(Math.max(0, Math.min(root.maxScroll, root.scrollPos() + direction * step)))
    }

    // Track interaction. NOTE the ordering: this MouseArea sits BEHIND the
    // thumb's own (declared later = on top), so a press that lands on the
    // thumb never reaches the track handler.
    MouseArea {
        id: trackArea
        anchors.fill: parent
        anchors.margins: -root.gripMargin   // clicks just beside a thin track count too
        cursorShape: Qt.PointingHandCursor
        // A faded-out bar is not a click target: without this the invisible
        // grip margin ate clicks on the row under it (see the auto-hide note).
        enabled: root.revealed
        // Press latches the hold (event-driven — see the note on `holding`):
        // a press that then holds still keeps the bar up and interactive however
        // long it lasts. Release re-arms the tail so the bar lingers briefly
        // instead of vanishing the instant the press ends.
        onPressed: root.holding = true
        onReleased: { root.holding = false; root.reveal() }
        onCanceled: root.holding = false
        onClicked: (mouse) => {
            if (!root.flickable) return
            const clickPos = root.horizontal ? mouse.x : mouse.y
            const at = clickPos - thumbLength / 2
            if (jumpToClick && !(mouse.modifiers & Qt.ShiftModifier)) {
                // JUMP: center the thumb on the click (clamped). Shift-click
                // keeps the classic page step for keyboard-style scrollers.
                const range = Math.max((root.horizontal ? root.width : root.height) - thumbLength, 1)
                root.setScrollPos(Math.max(0, Math.min(root.maxScroll, (at / range) * root.maxScroll)))
            } else {
                root.page(clickPos < thumbPos() ? -1 : 1)
            }
        }
    }

    function thumbPos() {
        const span = (root.horizontal ? root.width : root.height) - thumbLength
        return span > 0 ? span * (root.scrollPos() / root.maxScroll) : 0
    }

    Rectangle {
        id: thumb
        radius: root.trackWidth / 2
        // Hover feedback: brighten before you commit to the grab, the same
        // hover language every chip/button in this app uses.
        color: thumbArea.pressed ? Qt.lighter(root.thumbColor, 1.5)
             : thumbArea.containsMouse ? Qt.lighter(root.thumbColor, 1.25)
             : root.thumbColor
        // The thumb runs across the track; its length/position come from the
        // bar axis. Vertical: fixed trackWidth-wide, y driven. Horizontal:
        // fixed trackWidth-tall, x driven.
        width: root.horizontal ? root.thumbLength : root.trackWidth
        height: root.horizontal ? root.trackWidth : root.thumbLength
        Binding {
            target: thumb
            property: root.horizontal ? "x" : "y"
            value: root.thumbPos()
            when: !thumbArea.drag.active
        }

        // Drag state: the thumb's axis position is being set imperatively by
        // the drag — push it back into the Flickable's content position. Reads
        // the thumb's ACTUAL x/y (thumbPos() derives from the content position
        // being written here — it would never move).
        onXChanged: if (root.horizontal) thumb.syncFromDrag()
        onYChanged: if (!root.horizontal) thumb.syncFromDrag()
        function syncFromDrag() {
            if (!thumbArea.drag.active || !root.flickable) return
            const at = root.horizontal ? thumb.x : thumb.y
            const span = (root.horizontal ? root.width : root.height) - thumbLength
            root.setScrollPos(span > 0 ? (at / span) * root.maxScroll : 0)
        }

        // Hover-only twin of the thumb's MouseArea (see the pointerOnBar note).
        HoverHandler {
            id: thumbHover
            margin: root.gripMargin
        }

        MouseArea {
            id: thumbArea
            anchors.fill: parent
            anchors.margins: -root.gripMargin   // the fat invisible grip
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            enabled: root.revealed
            // Same latches as the track (see the note on `holding`). Latching
            // the press is also what keeps a DRAG alive when the pointer is
            // held still: `drag.active` is not readable from the visibility
            // binding, so before this a stationary thumb press outlasted the
            // reveal tail, `enabled` went false and the drag was torn down
            // mid-gesture.
            onPressed: root.holding = true
            onReleased: { root.holding = false; root.reveal() }
            onCanceled: root.holding = false
            drag.target: thumb
            drag.axis: root.horizontal ? Drag.XAxis : Drag.YAxis
            drag.minimumX: 0
            drag.maximumX: root.width - thumb.width
            drag.minimumY: 0
            drag.maximumY: root.height - thumb.height
        }
    }
}
