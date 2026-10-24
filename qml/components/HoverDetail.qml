import QtQuick
import VGRPresenterUI

// ============================================================================
// HoverDetail.qml — the app's reusable "more about this" hover.
//
// WHAT IT IS: a WRAPPER. Put the thing you want to explain inside it — an icon,
// a status dot, a badge, one row of a list — give it a `title` and a `detail`,
// and hovering that thing publishes a tip through the app's one tooltip path
// (TipService → the window-level ToolTipLayer in Main.qml). The owner needs to
// know nothing about tips: not where the bubble goes, not that it has to stay
// inside the window, not that this build never delivers a hover-exit event.
//
//     HoverDetail {
//         title: "Main Output"
//         detail: qsTr("%1 frames · %2 ago").arg(frames).arg(age)
//         OutputRuntimeDot { ... }       // whatever the pointer is over
//     }
//
// EXTRACTED, because the alternative is what this replaced: every control with
// something to say wrote the same four things — a PositionHoverArea, show() on
// entered, hide() on exited, forget() on destruction — and the surface that
// forgot the last line left its bubble standing after a delegate recycled
// (exactly the bug ChannelPill's comments record). One file owns that now, and a
// new surface inherits it by wrapping instead of copying it.
//
// TWO RULES IT ENFORCES FOR ITS CONSUMERS:
//   * the tip follows POINTER POSITION, not hover events (PositionHoverArea):
//     this build delivers hover-ENTER but never hover-EXIT, so a tip armed from
//     a MouseArea's containsMouse stays up forever;
//   * the click sink is OFF by default, so a wrapped control keeps its own
//     clicks — a tip is paint, never an interceptor.
Item {
    id: root

    // The wrapped thing. Any children: `HoverDetail { IconGlyph { ... } }`.
    default property alias content: contentItem.data

    // What the tip says. `title` is the subject ("Main Output", "NDI 3"), and
    // `detail` the facts about it ("live · 1204 frames · 0.3 s ago"). Either may
    // be empty; with both empty no tip is raised at all, so a control can bind
    // them to state and simply go quiet when it has nothing to add.
    property string title: ""
    property string detail: ""
    readonly property string tipText: root.title !== "" && root.detail !== ""
                                      ? root.title + " · " + root.detail
                                      : (root.title !== "" ? root.title : root.detail)
    // A surface that has a tip but must not offer it right now (hovering while
    // a dialog is simply not the moment) turns this off.
    property bool tipEnabled: true
    // Off by default: once a tip is up, moving to a NEIGHBOURING control (the
    // toolbar's row of pills) swaps its text in place with no re-delay and no
    // blank gap — see ToolTipLayer's update(). A tall scrolling list of rows
    // (Scripture/The Table's verse rows) is a different kind of hover: each
    // row is read deliberately, not swept past, so its tip should earn its
    // own delay every time instead of popping up the instant the pointer
    // crosses into the next row. Those surfaces set this true.
    property bool alwaysDelay: false

    // The wrapped control usually has its own cursor claim; leave that alone.
    // A dot or a bare label that wants the explaining hand sets this.
    property int cursorShape: Qt.ArrowCursor
    // Take the click as well as the hover? Off by default: the content keeps
    // its own MouseArea (see the header).
    property bool takeClicks: false
    // How far OUTSIDE the content the hover still counts. A 6-7 px status dot is
    // a poor hover target, and the thing being explained is small precisely
    // because the picture is the point — so the target is larger than the dot.
    property int hoverPadding: 4

    implicitWidth: contentItem.implicitWidth
    implicitHeight: contentItem.implicitHeight

    Item {
        id: contentItem
        anchors.fill: parent
    }

    PositionHoverArea {
        id: tipArea
        anchors.fill: parent
        anchors.margins: -root.hoverPadding
        showCursor: root.cursorShape !== Qt.ArrowCursor
        cursorShape: root.cursorShape
        clickEnabled: root.takeClicks
        onEntered: if (root.tipEnabled && root.tipText !== "") TipService.show(root.tipText, root, root.alwaysDelay)
        onExited: TipService.hide(root)
    }

    // THE FACTS CAN MOVE WHILE THE POINTER DOESN'T: a frame count, a state that
    // flipped to stalled, a row that was switched off under a still pointer. The
    // tip is re-published in place for the same owner, so it updates instead of
    // going stale (or blinking out through the show delay).
    onTitleChanged: refreshTip()
    onDetailChanged: refreshTip()
    onTipEnabledChanged: refreshTip()
    function refreshTip() {
        if (!tipArea.hovered)
            return
        if (root.tipEnabled && root.tipText !== "")
            TipService.show(root.tipText, root, root.alwaysDelay)
        else
            TipService.hide(root)
    }

    // A recycled delegate unloads mid-hover: a tip must not outlive its control.
    Component.onDestruction: TipService.forget(root)
}
