import QtQuick
import VGRPresenterUI

Item {
    id: root

    property Flickable flickable
    property int trackWidth: 8
    property color trackColor: Theme.surface
    property color thumbColor: "#3a3f4d"
    property int minThumbLength: 24
    property bool horizontal: false
    property int gripMargin: 5
    // True (default): a track click JUMPS so the thumb centers on the click.
    // False (page mode, opt-in): click above/below pages by one viewport.
    property bool jumpToClick: true

    // ---- Auto-hide ---------------------------------------------------------
    property bool autoHide: true
    property real idleOpacity: 0.0
    property int revealMs: 1600
    property int revealCoalesceMs: 220
    property int fadeOutMs: 480
    property int fadeInMs: 90

    readonly property bool pointerOnBar: stripHover.hovered
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

    function reveal() {
        if (root.revealPending)
            return
        root.revealPending = true
        Qt.callLater(root.applyReveal)
    }
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

    readonly property real maxScroll: !flickable ? 1
        : horizontal ? Math.max(flickable.contentWidth - flickable.width, 1)
                     : Math.max(flickable.contentHeight - flickable.height, 1)
    readonly property real thumbLength: !flickable ? 0
        : horizontal ? Math.max(minThumbLength, root.width * flickable.width / Math.max(flickable.contentWidth, 1))
                     : Math.max(minThumbLength, root.height * flickable.height / Math.max(flickable.contentHeight, 1))

    Rectangle {
        anchors.fill: parent
        radius: root.trackWidth / 2
        color: thumbArea.drag.active ? Qt.lighter(root.trackColor, 1.6) : root.trackColor

        HoverGlow {
            anchors.fill: parent
            radius: parent.radius
            hovered: root.pointerOnBar
            glowColor: Qt.rgba(1, 1, 1, 0.12)
        }
    }

    PositionHoverArea {
        id: stripHover
        anchors.fill: parent
        anchors.margins: -root.gripMargin
        clickEnabled: false
        showCursor: false
        checkAncestors: false
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

    MouseArea {
        id: trackArea
        anchors.fill: parent
        anchors.margins: -root.gripMargin   // clicks just beside a thin track count too
        cursorShape: Qt.PointingHandCursor
        // A faded-out bar is not a click target: without this the invisible
        // grip margin ate clicks on the row under it (see the auto-hide note).
        enabled: root.revealed
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
        color: thumbArea.pressed ? Qt.lighter(root.thumbColor, 1.5) : root.thumbColor

        // Hover is the shared layer (the press state above stays the bar's
        // own, because a held thumb must not dim under the pointer).
        HoverGlow {
            anchors.fill: parent
            radius: parent.radius
            // Position truth, not `thumbArea.containsMouse` (that latches true
            // in this build — the glow would never go out).
            hovered: root.pointerOnBar
            glowColor: Qt.rgba(1, 1, 1, 0.12)
        }
        width: root.horizontal ? root.thumbLength : root.trackWidth
        height: root.horizontal ? root.trackWidth : root.thumbLength
        Binding {
            target: thumb
            property: root.horizontal ? "x" : "y"
            value: root.thumbPos()
            when: !thumbArea.drag.active
        }

        onXChanged: if (root.horizontal) thumb.syncFromDrag()
        onYChanged: if (!root.horizontal) thumb.syncFromDrag()
        function syncFromDrag() {
            if (!thumbArea.drag.active || !root.flickable) return
            const at = root.horizontal ? thumb.x : thumb.y
            const span = (root.horizontal ? root.width : root.height) - thumbLength
            root.setScrollPos(span > 0 ? (at / span) * root.maxScroll : 0)
        }

        MouseArea {
            id: thumbArea
            anchors.fill: parent
            anchors.margins: -root.gripMargin   // the fat invisible grip
            cursorShape: Qt.PointingHandCursor
            enabled: root.revealed
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