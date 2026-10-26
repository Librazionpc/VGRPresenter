import QtQuick
import VGRPresenterUI

// A collapsible section card — the Edit inspector's one card shape, ported from
// FreeShow's own edit tools (InspirationOrResources/FreeShow-main/src/frontend/
// components/edit/tools/EditValues.svelte + the .section/.title/.highlight rules
// at the bottom of it), so every group of controls in the edit panel collapses,
// hovers and resets the same way instead of each card hand-rolling its own
// header (which is what SizeStyleCard/FiltersCard/TextItemPanel did before this
// existed: three copies of the same 44px header + chevron + MouseArea, and no
// reset affordance anywhere).
//
// What was carried across from FreeShow, and why:
//
//   * A header that IS the toggle: section icon in the section's own colour
//     (FreeShow's values/item.ts `sectionColors`), the title at 13px/Medium in
//     a muted ink, and a chevron that ROTATES rather than swapping glyph — one
//     icon, so the direction of change is visible mid-animation.
//   * `hasChanges` — FreeShow paints a reset control in the header exactly when the
//     section's values differ from its defaults, and auto-EXPANDS the section
//     when that flips true, so a value you just changed is never hidden behind a
//     collapsed card. Aggregated here (one bool the consumer computes from its
//     own model) rather than per-input like FreeShow's EditValues, because each
//     card here already owns one object (a CanvasItemStyle, an item's meta bag)
//     and can answer that in a single expression.
//   * `alwaysOpen` — FreeShow's first section has no title bar at all (font
//     family/size/formatting). A headerless block keeps the primary controls
//     one glance away instead of stacking a card on top of them.
//   * `default property alias content` — the consumer drops plain QML children
//     in and the card supplies the gutter, the spacing and the scroll-safe
//     height, so a card's body is never a second layout to maintain.
//
// Not carried across: FreeShow's CSS-driven animation of the body (it just
// appears). Here the body collapses to height 0 with `visible` gating, the same
// pattern the rest of this panel already used — no height animation to fight
// with a slider's label widening mid-drag.
Rectangle {
    id: root

    // ---- Header ----
    property string title: ""
    // Any IconGlyph name; "" draws no icon (a headerless-free but titled card,
    // e.g. the Filters card's own rows).
    property string icon: ""
    // The section's hue — see FreeShow's sectionColors map. Kept per-section
    // rather than one accent so a long panel can be scanned by colour, which is
    // the whole reason FreeShow tints them.
    property color iconColor: Theme.textMuted
    // FreeShow's "section.alwaysOpen": no title bar, no chevron, cannot collapse.
    property bool alwaysOpen: false

    // ---- Collapse state ----
    // Owned by the consumer when it wants to drive it (or just left alone: the
    // header click writes to it, which is the normal case).
    property bool expanded: true

    // True when this section's values differ from their defaults — shows the
    // Reset chip and force-expands the section once, exactly like FreeShow's
    // hasChangedValues()/toggleSection() pair.
    property bool hasChanges: false
    property string resetText: qsTr("Reset")

    // Fired by the Reset chip; the consumer puts its values back to their
    // defaults (and brackets that with its own undo hook). Reset does NOT
    // collapse the section — the operator wants to see the defaults applied.
    signal resetRequested()

    // Where content goes: plain children become rows of the body, stacked with
    // `rowSpacing` inside the card's own gutter.
    default property alias content: contentColumn.data

    // FreeShow's `.content section { padding: 10px }`, widened to this app's
    // 14px card gutter so a row's label lines up with a card header's label.
    readonly property int gutter: 14
    readonly property int rowSpacing: 14
    readonly property int headerHeight: 38

    readonly property int bodyContentHeight: contentColumn.implicitHeight + root.gutter * 2
    readonly property bool bodyShown: root.alwaysOpen || root.expanded

    implicitWidth: 260
    implicitHeight: headerArea.height + bodyArea.height

    radius: 9
    color: Theme.card
    border.width: 1
    border.color: Theme.borderSubtle
    clip: true

    // Auto-open when the section starts differing from its defaults — the
    // FreeShow behaviour that makes "I just changed something" impossible to
    // hide. Only on the false -> true edge, so collapsing a section by hand
    // afterwards sticks.
    onHasChangesChanged: {
        if (root.hasChanges && !root.alwaysOpen && !root.expanded)
            root.expanded = true
    }

    // ---- Header ----
    Rectangle {
        id: headerArea
        x: 1
        y: 1
        width: parent.width - 2
        height: root.alwaysOpen ? 0 : root.headerHeight
        visible: !root.alwaysOpen
        // FreeShow's `--primary-darker` title bar: a step darker than the card
        // in the dark theme, a step greyer in the light one (panelBg and card
        // are both pure white there, which would leave the header invisible).
        color: headerHit.containsMouse && !root.alwaysOpen
            ? Theme.chip
            : (Theme.dark ? Theme.panelBg : Theme.inset)
        radius: 8
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

        // Square off the header's bottom corners: it is the top of one card, so
        // only the top two corners should read as rounded.
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.radius
            color: headerArea.color
        }
        // FreeShow's `border-bottom: 1px solid var(--primary-lighter)`.
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.borderSubtle
        }

        MouseArea {
            id: headerHit
            anchors.fill: parent
            hoverEnabled: !root.alwaysOpen
            cursorShape: root.alwaysOpen ? Qt.ArrowCursor : Qt.PointingHandCursor
            onClicked: root.expanded = !root.expanded
        }

        Row {
            x: root.gutter
            anchors.verticalCenter: parent.verticalCenter
            spacing: 9

            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.icon !== ""
                name: root.icon
                color: root.iconColor
                fit: true
                width: 14
                height: 14
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.title
                // 0.8rem / opacity 0.8 in FreeShow's stylesheet — the title is a
                // label, not a heading, so it never competes with a row's value.
                color: Theme.textPrimary
                opacity: 0.85
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            // The reset chip replaces nothing — it sits BESIDE the chevron, so a
            // changed section can still be folded away (FreeShow's own title
            // button is disabled while changed, which loses the collapse; this
            // keeps both).
            Rectangle {
                visible: root.hasChanges
                width: resetLabel.implicitWidth + 16
                height: 20
                radius: 10
                color: resetHit.containsMouse ? Theme.hoverBg : Theme.inset
                border.width: 1
                border.color: Theme.borderSubtle
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    id: resetLabel
                    anchors.centerIn: parent
                    text: root.resetText
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: resetHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.resetRequested()
                }
            }

            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                // One glyph, rotated — the change of direction is visible while
                // it turns (FreeShow swaps between two SVGs instead).
                name: "chevronDown"
                color: Theme.textMuted
                width: 11
                height: 11
                rotation: root.expanded ? 180 : 0
                Behavior on rotation { NumberAnimation { duration: 130; easing.type: Easing.OutQuad } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.expanded = !root.expanded
                }
            }
        }
    }

    // ---- Body ----
    Item {
        id: bodyArea
        anchors.top: headerArea.bottom
        width: parent.width
        height: root.bodyShown ? root.bodyContentHeight : 0
        visible: height > 0
        clip: true

        Column {
            id: contentColumn
            x: root.gutter
            y: root.gutter
            width: parent.width - root.gutter * 2
            spacing: root.rowSpacing
        }
    }
}
