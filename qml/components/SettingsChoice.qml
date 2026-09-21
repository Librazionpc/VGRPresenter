import QtQuick
import VGRPresenterUI

// The "pick one" control of a Settings row: the current choice's label and a chevron, opening a dropdown of the other
// choices. It is driven ENTIRELY by the engine's definition of the setting (SettingsService.definitions[key]): the list of
// choices, their labels and the current value all come from there, and a pick is sent back with SettingsService.setValue(),
// which the engine checks. Nothing about a setting is written into the screen that uses this.
Item {
    id: root

    // The engine's key, e.g. "appearance.language".
    property string settingKey: ""

    readonly property var def: SettingsService.definitions[root.settingKey]
    readonly property var choices: root.def ? root.def.choices : []
    readonly property var current: SettingsService.values[root.settingKey]
    readonly property string currentLabel: {
        for (let i = 0; i < root.choices.length; ++i)
            if (root.choices[i].value === root.current)
                return root.choices[i].label
        return root.current === undefined ? "" : String(root.current)
    }

    implicitWidth: pickerRow.width
    implicitHeight: 28

    function openMenu() {
        menu.model = root.choices.map((c) => ({ label: c.label, trailing: c.value === root.current ? "✓" : "" }))
        menu.openAt(root, root.width - menu.width, root.height + 4, root.Window.contentItem)
    }

    Row {
        id: pickerRow
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.currentLabel
            color: pickerArea.containsMouse ? Theme.textPrimary : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
        }
        IconGlyph {
            anchors.verticalCenter: parent.verticalCenter
            name: "chevronDown"
            color: Theme.textMuted
            width: 10; height: 10
        }
    }

    MouseArea {
        id: pickerArea
        anchors.fill: parent
        anchors.margins: -4
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: menu.visible ? menu.visible = false : root.openMenu()
    }

    MenuCatcher { menu: menu; closesOnWheel: false }

    DropdownPanel {
        id: menu
        visible: false
        z: 25
        width: 200
        maxHeight: 320
        onItemActivated: (label) => {
            menu.visible = false
            for (let i = 0; i < root.choices.length; ++i)
                if (root.choices[i].label === label) {
                    if (root.choices[i].value !== root.current)
                        SettingsService.setValue(root.settingKey, root.choices[i].value)
                    return
                }
        }
    }
}
