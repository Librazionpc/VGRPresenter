import QtQuick
import VGRPresenterUI

// The 4px level meter on a routing-board card (audio rows, video media
// rows, buses). Extracted so track/fill geometry can't drift across
// delegates — place it and give it `value` (0..100, clamped).
//
// COLOR IS VALUE-DRIVEN, like a real VU meter: green in the safe zone,
// amber approaching the top, red in the hot zone. The old hardcoded green
// read "all good" at any level — a row pinned at 100 looked identical to
// one at 20, which is exactly the kind of dishonest visual this component
// exists to avoid. Pass fillColor ONLY for a deliberate fixed color
// (e.g. a neutral state); the default tracks the value.
Rectangle {
    id: root

    property real value: 0
    // Explicit override; when unset the fill follows the VU zones below.
    property color fillColor: root.zoneColor
    // Set true to keep a caller-supplied fillColor from being reinterpreted.
    property bool fixedColor: false

    // VU zones: green below 60, amber 60-84, red 85+.
    readonly property color zoneColor: root.value >= 85 ? Theme.danger
                                      : root.value >= 60 ? Theme.warning
                                      : Theme.success

    width: 100; height: 4; radius: 2
    color: Theme.chip

    Rectangle {
        width: parent.width * Math.max(0, Math.min(1, root.value / 100))
        height: parent.height
        radius: 2
        color: root.fixedColor ? root.fillColor : root.zoneColor
        Behavior on color { ColorAnimation { duration: 150 } }
    }
}
