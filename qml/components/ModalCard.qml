import QtQuick
import QtQuick.Controls
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
    // so clicks and hover never reach the controls behind the dialog. The
    // actual value is bumped to the top of ModalStack's shared counter the
    // moment this card OPENS (see open() below) — a flat 30000 on every
    // instance stacked simultaneously-open dialogs by declaration order
    // instead of by which one was opened last (see ModalStack.qml).
    z: ModalStack.base

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
    // ModalCard forms can contain edits that are saved continuously. Keep an
    // outside tap from silently cancelling them; dialogs that intentionally
    // support backdrop dismissal can opt in at their call site.
    property bool clickOutsideToDismiss: false
    // Resolved automatically from ModalStack's open-dialog stack (see its
    // own comment) — whatever is directly behind THIS card, whether that's
    // the Settings panel, another dialog this one opened on top of, or the
    // real page. No per-dialog wiring: push/pop below is the only plumbing,
    // and blurSourceFor() does the rest, so a dialog three files deep from
    // Main.qml gets the right answer without knowing where it lives.
    readonly property Item blurSource: ModalStack.blurSourceFor(card)
    // An optional single header action, drawn left of the close X (the action
    // editor's icon picker). Empty icon = no button, so every other dialog's
    // header is unchanged; `headerActionActive` gives it the accent wash the
    // same way a selected chip reads.
    property string headerActionIcon: ""
    property bool headerActionActive: false
    signal headerActionClicked()
    // A leading back arrow beside the title, for a dialog that dives into a
    // sub-view of itself (the action editor's trigger list). The title stays
    // put, so the operator can see which action they are still inside.
    property bool headerBackVisible: false
    signal headerBackClicked()
    property string saveText: "Save"
    property string cancelText: "Cancel"
    // Real, not caller-guessed — the footer's own implicit height, so the
    // content Flickable always reserves exactly the right amount of room
    // instead of a manually-passed number silently drifting from FooterBar's
    // actual size.
    // NaN-proof: a NaN here propagates into flick.height and collapses the whole
    // card to zero height with no QML error (the footer is created last, and a
    // read that lands mid-construction must never take the dialog down with it).
    readonly property real footerHeight: {
        if (!root.showFooter || !footerBar)
            return 0
        const h = footerBar.implicitHeight
        return isFinite(h) && h > 0 ? h : 0
    }

    default property alias contentData: contentCol.data

    signal cancelled()
    // Fired by the footer's Save button.
    signal accepted()

    // The dialog belongs to the WINDOW, not to the pane that declares it. A modal
    // declared inside a settings page (every Add / Edit dialog in Settings) must dim
    // the whole app, not just that page's content column: parented in place its scrim
    // covered one side of the settings card, so the dialog read as half-modal (user
    // report: "the black box doesn't cover the whole settings modal, only one side").
    // Reparented to the window overlay, this component has no scaled ancestor to
    // inherit a scale from, so its CARD applies the app scale itself (see AppScale.qml).
    parent: Overlay.overlay
    anchors.fill: parent
    visible: opacity > 0
    enabled: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 130 } }

    property bool shown: false
    function open() {
        shown = true
        root.z = ModalStack.nextZ()
        ModalStack.push(card)
    }
    function close() {
        shown = false
        ModalStack.pop(card)
    }

    // ---- Esc = cancel, Enter = accept (qml/settings/Hotkeys.qml) ---------
    // Every dialog built on this card answers the app's two keys while it is
    // open, with no per-dialog key code: Esc is this card's own cancel path
    // (the same one the X and the scrim call), Enter is the footer's Save —
    // and ONLY when the card has a Save, so a read-only card (showSave: false)
    // leaves Enter to whatever has focus.
    //
    // A scope, not a Keys handler, because the focus inside a form is usually
    // a text field and a field consumes Enter; the window's Shortcut sees the
    // key first and the Hotkeys stack routes it to the dialog on top.
    property int hotkeyScope: 0
    function syncHotkeyScope() {
        if (root.shown && root.hotkeyScope === 0) {
            root.hotkeyScope = Hotkeys.open({
                name: root.title !== "" ? root.title : "ModalCard",
                cancel: function () { root.cancelled() },
                // A card with no footer, or no Save in it, has nothing for
                // Enter to press (the quick-search card is the important one:
                // its Enter opens the selected result, so claiming the key for
                // an `accepted()` nobody listens to would break it).
                accept: (root.showSave && root.showFooter)
                        ? function () { root.accepted() } : null
            })
        } else if (!root.shown && root.hotkeyScope !== 0) {
            Hotkeys.close(root.hotkeyScope)
            root.hotkeyScope = 0
        }
    }
    onShownChanged: root.syncHotkeyScope()
    // A dialog that gains or loses its Save (or its footer) while open must
    // claim/release Enter with it.
    onShowSaveChanged: root.rebuildHotkeyScope()
    onShowFooterChanged: root.rebuildHotkeyScope()
    function rebuildHotkeyScope() {
        Hotkeys.close(root.hotkeyScope)
        root.hotkeyScope = 0
        root.syncHotkeyScope()
    }
    Component.onDestruction: { Hotkeys.close(root.hotkeyScope); ModalStack.pop(card) }

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
        blurSource: root.blurSource
        clickToDismiss: root.clickOutsideToDismiss
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        // Hook for the modal-fit scenario: the modal ROOT always fills the
        // window, so only the card can show that a dialog overflows. A dialog
        // that carries an objectName gets its card measurable as
        // "<objectName>_card" (the scenario measures the card against the
        // window at the smallest size the window allows).
        objectName: root.objectName === "" ? "" : root.objectName + "_card"
        // The app's one scale is applied to the CARD, never to the scrim above it:
        // the dim backdrop has to keep covering the whole window it is dimming, or
        // the dialog loses the black box that gives it its modal feel. AppScale
        // (AppScale.qml) also subtracts whatever scale an ancestor already applies
        // (a screen, the Settings card), so a dialog opened inside one of those is
        // never scaled twice.
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
        // Capped against the available area too — same safety margin as
        // the height clamp below, so a wide cardWidth can't push the card
        // past the window's own edges on a smaller settings window.
        // The room this card may use, in DESIGN units. The root's own size is in
        // WINDOW pixels — this dialog's root fills the window — while cardWidth,
        // the header and the footer are design values, so the root's size is
        // divided by the card's own scale before the two are compared. Without
        // that, a dialog over a scaled-down window capped itself at roughly half
        // the height it actually had.
        readonly property real roomWidth: root.width / (scale > 0 ? scale : 1) - 80
        readonly property real roomHeight: root.height / (scale > 0 ? scale : 1) - 80
        // The margin the Flickable keeps under the footer, on top of the 40px
        // margins roomHeight already removed — named so the height chain reads as
        // header + viewport + footer + gutter instead of a bare literal.
        readonly property real bottomGutter: 40
        width: Math.min(root.cardWidth, roomWidth)
        height: Math.min(roomHeight,
                         Theme.space6 * 2 + headerItem.height + Theme.space4
                         + flick.height + root.footerHeight)
        // A caller whose content swaps shape while open (as ManageActionsModal
        // does when an inline picker opens) gets a smooth resize instead of a snap, with no per-dialog
        // code: ONE fix here covers every ModalCard, which is the whole
        // point of sharing this shell (user call: "that is why i want a
        // single point of use"). Disabled for the dialog's OWN open/close
        // (shown flips, not root.shown) — the height is already correct
        // the instant `visible` turns true (opacity fades it in separately),
        // so animating height on open would have the card visibly grow from
        // whatever its height happened to be the PREVIOUS time it opened.
        Behavior on height {
            enabled: root.shown
            NumberAnimation { duration: 160; easing.type: Easing.OutQuad }
        }
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
                x: root.headerBackVisible ? 30 : 0
                width: parent.width - closeBtn.width - Theme.space3
                       - (root.headerActionIcon !== "" ? headerAction.width + Theme.space2 : 0)
                       - x
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
                id: headerBack
                visible: root.headerBackVisible
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.topMargin: 4
                name: "arrowLeft"
                color: Theme.textSecondary
                implicitWidth: 16
                implicitHeight: 16
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.headerBackClicked()
                }
            }

            Rectangle {
                id: headerAction
                visible: root.headerActionIcon !== ""
                anchors.right: closeBtn.left
                anchors.rightMargin: Theme.space3
                anchors.verticalCenter: closeBtn.verticalCenter
                width: 30; height: 30; radius: Theme.radiusMd
                color: root.headerActionActive ? Qt.alpha(Theme.accent, 0.18)
                     : (headerActionHover.hovered ? Theme.hoverBg : "transparent")
                IconGlyph {
                    anchors.centerIn: parent
                    name: root.headerActionIcon
                    color: root.headerActionActive ? Theme.accentLight : Theme.textSecondary
                    width: 15; height: 15; fit: true
                }
                HoverHandler { id: headerActionHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.headerActionClicked() }
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
            // The viewport's own room: roomHeight (the window's height in design
            // units, already minus the card's outer 40px top and bottom margins)
            // less the bottom gutter, the footer and the header offset above this
            // Flickable. Floored at 0 and never NaN — the card's height derives
            // from this, so one bad number here is a BLANK dialog, not a short one.
            //
            // card.roomHeight, not a bare roomHeight: this binding hangs off the
            // Flickable, and QML only resolves an unqualified name against the
            // object the binding sits on plus the component's root — never a
            // SIBLING's custom properties. Unqualified it threw
            // "ReferenceError: roomHeight is not defined", the whole expression
            // went NaN, the Flickable collapsed to height 0 and every dialog's
            // content vanished ("add source has cleared it").
            readonly property real room: {
                const h = card.roomHeight - card.bottomGutter - root.footerHeight - y
                return isFinite(h) ? Math.max(0, h) : 0
            }
            height: Math.min(contentCol.height, room)
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
            // Names the footer's buttons "<objectName>_save"/"_cancel" for a
            // dialog this card gives a name (see FooterBar.hookName).
            hookName: root.objectName
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
