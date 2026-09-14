import QtQuick
import VGRPresenterUI

// Small status/label chip. Two modes:
//  - tint: true  -> bg = baseColor @ tintAlpha, text = lightColor  (selected-state chips, badges)
//  - tint: false -> bg = Theme.chip (flat neutral), text = textColor  (neutral tag chips)
Rectangle {
    id: root

    property string text: ""
    property color baseColor: Theme.accent
    property color lightColor: Theme.accentLight
    property bool tint: true
    property real tintAlpha: 0.14
    property color textColor: Theme.textSecondary
    property int horizontalPadding: Theme.space3
    property real fontSize: Theme.textXs

    implicitWidth: label.implicitWidth + horizontalPadding * 2
    implicitHeight: label.implicitHeight + Theme.space2
    radius: Theme.radiusSm

    // Hoisted out: chaining a color's .r/.g/.b inline inside a ternary
    // miscompiles under Qt's QML AOT compiler (see NavItem.qml).
    readonly property color tintedColor: Qt.rgba(root.baseColor.r, root.baseColor.g, root.baseColor.b, root.tintAlpha)
    color: root.tint ? root.tintedColor : Theme.chip

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        font.family: Theme.fontFamily
        font.pixelSize: root.fontSize
        font.weight: Font.Medium
        color: root.tint ? root.lightColor : root.textColor
    }
}
