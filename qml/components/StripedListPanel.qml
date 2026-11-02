import QtQuick
import VGRPresenterUI

Item {
    id: root

    default property alias contentData: column.data

    property int padding: 5
    property int rowHeight: 38
    property int rowRadius: 6
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