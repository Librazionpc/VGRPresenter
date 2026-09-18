import QtQuick
import VGRPresenterUI

// The 8px status dot beside a routing-board card's title — THE connection
// indicator, identical across all sources (per user's model):
//   accent (green/blue) — the row is routed into at least one bus
//   grey                — the row is not routed anywhere
// Muted rows dim the whole card (pill flips to MUTE); they don't get a
// third color. One meaning per element: connection lives HERE, the edge
// PortDots are affordances, the pill is the mute control.
Rectangle {
    id: root

    property bool connected: false
    property color accent: Theme.success

    x: 12; y: 14
    width: 8; height: 8; radius: 4
    color: root.connected ? root.accent : Theme.border
}
