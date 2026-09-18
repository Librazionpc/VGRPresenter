import QtQuick
import VGRPresenterUI

// The 4px level meter on a routing-board card (audio rows, video media
// rows, buses). Extracted so track/fill geometry can't drift across
// delegates — place it, give it `value` (0..100, clamped) and a fill color.
Rectangle {
    id: root

    property real value: 0
    property color fillColor: Theme.success

    width: 100; height: 4; radius: 2
    color: Theme.chip

    Rectangle {
        width: parent.width * Math.max(0, Math.min(1, root.value / 100))
        height: parent.height
        radius: 2
        color: root.fillColor
    }
}
