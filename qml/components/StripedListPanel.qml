import QtQuick
import VGRPresenterUI

// THE striped list surface — one inset, hairlined panel whose rows alternate
// between two faint tints. The action roster, the "choose action" catalog and
// the custom-activation list all sit in one of these, so the three can never
// drift apart again: the ground, the hairline, the inner padding, the row
// height and BOTH row colours are decided here and nowhere else.
//
// Rows go straight in (they land in `column`, a Column that already has the
// panel's inner width):
//
//     StripedListPanel {
//         id: panel
//         width: parent.width
//         Repeater {
//             model: ...
//             delegate: Rectangle {
//                 required property int index
//                 width: parent.width
//                 height: panel.rowHeight
//                 color: panel.rowColor(index, hover.hovered)
//             }
//         }
//     }
Item {
    id: root

    default property alias contentData: column.data

    property int padding: 5
    property int rowHeight: 38
    property int rowRadius: 6
    // false = no bordered/inset box around the rows — they spread over
    // whatever background the caller already sits on (the Settings nav
    // rail: "don't contain it, let it spread and use the base as its bg").
    // The striping/hover/padding stay identical either way.
    property bool showPanel: true
    // The second tint — the app's ONE striping rule (ListRows), so the
    // virtualized lists that stripe themselves match these panels exactly.
    property color stripeColor: ListRows.stripe

    implicitWidth: panel.width
    implicitHeight: column.height + root.padding * 2 + 2

    // The one place a row's resting colour is decided: hover wins, then the
    // alternating tint, then the panel's own ground shows through.
    function rowColor(index, hovered) {
        return ListRows.rowColor(index, hovered)
    }

    Rectangle {
        id: panel
        anchors.fill: parent
        radius: Theme.radiusMd
        color: root.showPanel ? Theme.inset : "transparent"
        border.width: root.showPanel ? 1 : 0
        border.color: Theme.border

        Column {
            id: column
            x: root.padding
            y: root.padding
            width: parent.width - root.padding * 2
            spacing: 0
        }
    }
}
