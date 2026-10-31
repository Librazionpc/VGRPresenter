import QtQuick
import VGRPresenterUI

// Shared roster shortcut editor used by ManageGroups and ManageActions.
// The owning model and row stay with the caller; this component only captures
// one letter/number or handles the standard clear/cancel keys.
//
// Three shapes, one capture path (so the key rules can never drift):
//   "chip"  the compact centered chip in a roster row (ManageGroups, the
//           action list);
//   "field" the action editor's ADD state: a tall inset box with the label
//           above it and a centered "— shortcut —" placeholder;
//   "boxed" the action editor's EDIT state: a floating label, the captured
//           key at the lower left, and the clear (×) / capture (↵) marks at
//           the right.
Rectangle {
    id: root
    property string shortcut: ""
    property bool capturing: false
    property string variant: "chip"      // "chip" | "field" | "boxed"
    // Drops the field's own chrome (background, border, radius) so it can be a
    // CELL of a joined row — Manage groups' FreeShow-style strip, where the cells
    // share one surface. The layout and the key rules stay identical, which is the
    // point of sharing this component instead of copying the capture handler.
    property bool frameless: false
    // Drawn INSIDE the box, above the value — only read by "boxed".
    property string label: ""
    signal captureRequested()
    signal shortcutEdited(string value)
    signal captureCancelled()

    readonly property bool boxed: variant === "boxed"
    readonly property bool tall: variant !== "chip"

    width: 90
    // 44 — the app's standard field height (matches the Name field right
    // above it in the editor; both are "boxed" fields and must stay the
    // same size or the two rows read as misaligned).
    height: root.tall ? 44 : 28
    radius: root.frameless ? 0 : (root.tall ? 7 : 6)
    color: root.frameless ? "transparent"
           : (root.capturing ? Qt.alpha(Theme.accent, 0.18)
                             : (root.tall ? Theme.inset : Theme.card))
    border.width: root.frameless ? 0 : 1
    border.color: root.capturing ? Theme.accent : Theme.border

    onCapturingChanged: if (root.capturing) keySink.forceActiveFocus()

    Text {
        id: labelText
        visible: root.boxed && root.label !== ""
        x: 13; y: 5
        text: root.label
        // Lit once the field carries a value (or is listening) — the same
        // "this one is filled" cue the Name field's label gives.
        color: root.capturing || root.shortcut !== "" ? Theme.accentLight : Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
    }

    Text {
        id: valueText
        anchors.left: parent.left
        anchors.leftMargin: root.boxed ? 13 : 0
        anchors.right: trailing.visible ? trailing.left : parent.right
        anchors.rightMargin: root.tall ? 6 : 0
        anchors.verticalCenter: root.boxed ? undefined : parent.verticalCenter
        anchors.bottom: root.boxed ? parent.bottom : undefined
        anchors.bottomMargin: root.boxed ? 7 : 0
        horizontalAlignment: root.boxed ? Text.AlignLeft : Text.AlignHCenter
        text: root.capturing ? qsTr("Press a key…")
                             : (root.shortcut !== "" ? root.shortcut
                                : (root.boxed ? "" : qsTr("— shortcut —")))
        color: root.shortcut !== "" || root.capturing ? Theme.textPrimary : Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: root.tall ? Theme.textSm : Theme.textXs
        font.weight: root.boxed ? Font.DemiBold : Font.Normal
        elide: Text.ElideRight
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.captureRequested()
    }

    // ---- the boxed field's own two marks --------------------------------
    // Declared AFTER the field's MouseArea so they sit above it: pressing ×
    // clears without starting a capture, pressing ↵ starts a capture.
    Row {
        id: trailing
        visible: root.boxed
        anchors.right: parent.right
        anchors.rightMargin: 9
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Rectangle {
            visible: root.shortcut !== "" && !root.capturing
            width: 24; height: 24; radius: 6
            color: clearHover.hovered ? Qt.alpha(Theme.textPrimary, 0.08) : "transparent"
            IconGlyph {
                anchors.centerIn: parent
                name: "close"
                color: Theme.textMuted
                width: 11; height: 11
            }
            HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.shortcutEdited("") }
        }

        Rectangle {
            width: 24; height: 24; radius: 6
            color: root.capturing ? Qt.alpha(Theme.accent, 0.2)
                 : (captureHover.hovered ? Qt.alpha(Theme.textPrimary, 0.06) : "transparent")
            IconGlyph {
                anchors.centerIn: parent
                name: "cornerDownLeft"
                color: root.capturing ? Theme.accentLight : Theme.textSecondary
                width: 14; height: 14; fit: true
            }
            HoverHandler { id: captureHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.captureRequested() }
        }
    }

    Item {
        id: keySink
        anchors.fill: parent
        focus: false
        Keys.onPressed: (event) => {
            if (!root.capturing)
                return
            if (event.key === Qt.Key_Escape) {
                root.captureCancelled()
                event.accepted = true
                return
            }
            if (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete) {
                root.shortcutEdited("")
                event.accepted = true
                return
            }
            if (event.text.length === 1 && /[a-zA-Z0-9]/.test(event.text)) {
                root.shortcutEdited(event.text)
                event.accepted = true
            }
        }
    }
}
