import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property var sections: [
        { key: "general",   label: "General",        icon: "sliders" },
        { key: "smart",     label: "Smart Config",    icon: "spark" },
        { key: "outputs",   label: "Outputs",         icon: "monitor" },
        { key: "styles",    label: "Styles",          icon: "layoutTemplate" },
        { key: "av",        label: "Audio & Video",   icon: "monitor" },
        { key: "recording", label: "Recording",       icon: "recordDot" },
        { key: "plugins",   label: "Plugins",         icon: "grid" }
    ]
    property string currentKey: "general"
    property string appVersion: "v0.0.1"

    signal sectionSelected(string key)

    implicitWidth: 206
    color: "transparent"

    Rectangle {
        // rail-edge divider
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.railDivider
    }

    Column {
        // x/y use plain numbers, not Theme.spaceN — see DropdownPanel.qml.
        x: 20
        y: 24
        width: parent.width - Theme.space5 * 2
        spacing: Theme.space2

        Text {
            text: "SETTINGS"
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
            font.weight: Font.Medium
            color: Theme.textMuted
            bottomPadding: Theme.space2
        }

        Repeater {
            model: root.sections
            delegate: NavItem {
                required property var modelData
                width: parent.width
                label: modelData.label
                iconKind: modelData.icon
                selected: modelData.key === root.currentKey
                onClicked: root.sectionSelected(modelData.key)
            }
        }
    }

    Column {
        x: 20
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space6
        spacing: 2

        Text {
            text: "Support"
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            color: Theme.footerSupport
        }
        Text {
            text: root.appVersion
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
            color: Theme.footerVersion
        }
    }
}
