import QtQuick
import VGRPresenterUI

// A labeled, tappable value box — "Font family / Segoe UI ⌄" shape: a small
// accent label above the current value, an optional chevron hinting it
// opens something, hover-lit, position-truth (not containsMouse — see
// fieldHit below). Pulled out of TextItemPanel.qml (where it lived as a
// private `component Field`) into its own file so other panels can reuse
// the exact same box instead of each one hand-rolling its own (user call:
// "make that box[es] a shared component so I can reuse in other
// serv[ic]es/screens").
//
// Usage: FieldBox { label: "Weight"; value: "SemiBold"; chevron: true;
// onTapped: menu.openAt(this, 0, height + 2, root) } — the consumer owns
// what "tapped" does (open a DropdownPanel, a color picker, anything); this
// component only draws the box and reports the tap.
Rectangle {
    id: field

    property string label: ""
    property string value: ""
    // Hints that tapping opens something (a dropdown, a picker) — purely
    // visual; the consumer decides what tapped() actually does.
    property bool chevron: false
    signal tapped()

    height: 50
    radius: 10
    color: fieldHit.hovered ? Theme.hoverBg : Theme.inset
    border.color: fieldHit.hovered ? Theme.border : Theme.borderSubtle
    border.width: 1
    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
    Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

    Column {
        x: 12
        width: field.width - (field.chevron ? 32 : 24)
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        Text {
            text: field.label
            color: Theme.accent
            font.family: Theme.fontFamily
            font.pixelSize: 11
            font.weight: Font.Medium
            font.letterSpacing: 0.2
            elide: Text.ElideRight
            width: parent.width
        }
        Text {
            text: field.value
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: 16
            elide: Text.ElideRight
            width: parent.width
        }
    }

    IconGlyph {
        anchors.right: parent.right
        anchors.rightMargin: 9
        anchors.verticalCenter: parent.verticalCenter
        visible: field.chevron
        name: "chevronDown"
        color: Theme.textMuted
        width: 12; height: 12
    }

    // Position truth, not containsMouse — hover-exit never delivers to
    // MouseAreas in this build (the "hover stays lit after I move away"
    // failure mode this box used to have).
    PositionHoverArea {
        id: fieldHit
        anchors.fill: parent
        checkAncestors: false
        onClicked: field.tapped()
    }
}
