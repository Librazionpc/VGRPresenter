import QtQuick
import VGRPresenterUI

// Reusable modal card: dim scrim + centered card with title/subtitle/close-X
// header, a scrollable content slot, and a pinned Cancel/Save footer (see
// FooterBar.qml — the same footer ModalShell.qml uses, not a per-dialog
// hand-rolled button row). The shared shell for Screens's Add/Edit dialogs
// — and any future settings-section modal — so scrim styling, header
// layout, footer and centering can't drift between dialogs.
//
// Contract: caller sets title/subtitle, fills `content` (children land in
// the scrollable column — NOT the footer; Cancel/Save buttons belong to
// this card, not the caller, so they can never end up scrolled out of view
// or double-built per dialog), and reacts to cancelled()/accepted(). Height
// never binds circularly: the inner Flickable caps itself against the
// overlay first, then the card derives from header + flick + footer.
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property string title: ""
    property string subtitle: ""
    // Overridable per dialog — a content-dense form (e.g. ScreenForm's
    // placement mini-map sitting next to its Identify/Lock controls) needs
    // more room than a simple confirm-style dialog before things start
    // getting clipped or squeezed.
    property real cardWidth: 560
    // Corner radius of the card itself — 0 gives a sharp RECTANGULAR card
    // (the Media pane's video-preview modal) while every other dialog keeps
    // the rounded Theme.radiusLg default.
    property real cardRadius: Theme.radiusLg
    property bool showFooter: true
    property bool showSave: true
    property string saveText: "Save"
    property string cancelText: "Cancel"
    // Real, not caller-guessed — the footer's own implicit height, so the
    // content Flickable always reserves exactly the right amount of room
    // instead of a manually-passed number silently drifting from FooterBar's
    // actual size.
    readonly property real footerHeight: root.showFooter ? footerBar.implicitHeight : 0

    default property alias contentData: contentCol.data

    signal cancelled()
    // Fired by the footer's Save button.
    signal accepted()

    anchors.fill: parent
    visible: opacity > 0
    enabled: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 130 } }

    property bool shown: false
    function open() { shown = true }
    function close() { shown = false }

    // Brings an item from the content column into view. A selection that
    // REVEALS a new section (AudioEffectsPanel's selected-effect editor)
    // otherwise materializes below the fold of a tall dialog — which reads
    // as "nothing happened". Maps the item into content coordinates and
    // nudges contentY just far enough to expose it; no-op when already
    // visible. Pass an empty item/null to skip.
    NumberAnimation {
        id: revealAnim
        target: flick
        property: "contentY"
        duration: 160
        easing.type: Easing.OutCubic
    }
    function revealItem(item) {
        if (!item || !contentCol)
            return
        const top = item.mapToItem(contentCol, 0, 0).y
        const bottom = top + item.height
        let target = flick.contentY
        if (top < flick.contentY + 8)
            target = Math.max(0, top - 8)
        else if (bottom > flick.contentY + flick.height - 8)
            target = Math.max(0, Math.min(flick.contentHeight - flick.height,
                                          bottom - flick.height + 8))
        if (Math.abs(target - flick.contentY) > 1) {
            revealAnim.stop()
            revealAnim.from = flick.contentY
            revealAnim.to = target
            revealAnim.start()
        }
    }

    // Shared scrim — also consumes wheel events so scrolling a full (or
    // empty) dialog can't leak into the page's Flickable behind the modal.
    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        // Capped against the available area too — same safety margin as
        // the height clamp below, so a wide cardWidth can't push the card
        // past the window's own edges on a smaller settings window.
        width: Math.min(root.cardWidth, root.width - 80)
        height: Math.min(root.height - 80,
                         Theme.space6 * 2 + headerItem.height + Theme.space4
                         + flick.height + root.footerHeight)
        anchors.centerIn: parent
        radius: root.cardRadius
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        clip: true

        Item {
            id: headerItem
            x: Theme.space6
            y: Theme.space6
            width: parent.width - 2 * Theme.space6
            height: headerCol.implicitHeight

            Column {
                id: headerCol
                width: parent.width - closeBtn.width - Theme.space3
                spacing: Theme.space1

                Text {
                    text: root.title
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXl
                    font.weight: Font.DemiBold
                }

                Text {
                    visible: root.subtitle !== ""
                    width: parent.width
                    text: root.subtitle
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.WordWrap
                }
            }

            IconGlyph {
                id: closeBtn
                anchors.right: parent.right
                anchors.top: parent.top
                name: "close"
                color: Theme.textMuted
                implicitWidth: 16
                implicitHeight: 16

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.cancelled()
                }
            }
        }

        Flickable {
            id: flick
            x: Theme.space6
            y: headerItem.y + headerItem.height + Theme.space4
            // Symmetric with the left margin — the scrollbar lives INSIDE
            // this same right-hand Theme.space6 strip (see scrollBar below),
            // not as extra width subtracted on top of it. Reserving space
            // for the scrollbar as an addition on top of the margin is what
            // left a visible dead gap between the scrollbar and the card's
            // actual edge.
            width: parent.width - 2 * Theme.space6
            height: Math.min(contentCol.height,
                             root.height - 120 - root.footerHeight - y)
            contentWidth: width
            contentHeight: contentCol.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: contentCol
                width: parent.width
                spacing: Theme.space4
            }
        }

        // Auto-hides itself when there's nothing to scroll (see
        // AppScrollBar's own `visible` binding) — always present, never
        // reserving space unnecessarily on a short dialog.
        AppScrollBar {
            id: scrollBar
            // Centered within the card's own trailing Theme.space6 margin
            // (the same strip flick's right edge stops at), not clutched
            // right up against the content.
            x: parent.width - Theme.space6 + (Theme.space6 - width) / 2
            y: flick.y
            height: flick.height
            flickable: flick
        }

        // Pinned footer, sibling of (and below) the Flickable — never part
        // of the scrollable content, so Cancel/Save can't be scrolled out
        // of view or overlap whatever's above it. footerHeight (see its own
        // property) reserves exactly this much room for flick above.
        FooterBar {
            id: footerBar
            visible: root.showFooter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            showSave: root.showSave
            saveText: root.saveText
            cancelText: root.cancelText
            onCancelRequested: root.cancelled()
            onSaveRequested: root.accepted()
        }
    }
}
