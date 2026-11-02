import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property bool connected: false
    property color accent: Theme.success

    x: 12; y: 14
    width: 8; height: 8; radius: 4
    color: root.connected ? root.accent : Theme.border
}