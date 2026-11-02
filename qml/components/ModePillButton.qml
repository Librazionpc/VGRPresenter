import QtQuick
import VGRPresenterUI
import "."

Item {
    id: root

    property string text: ""
    property var modes: []
    // Device-wide fps ceiling (0 = unknown → no gating).
    property int maxFps: 0
    signal modePicked(string mode)

    // "1920x1080p60" → 60 ("1920x1080p29.97" → 29.97). -1 when unparseable.
    function modeFps(m) {
        const p = m.lastIndexOf("p")
        if (p < 0) return -1
        const v = parseFloat(m.substring(p + 1))
        return isNaN(v) ? -1 : v
    }

    implicitWidth: pillLabel.implicitWidth + 16
    implicitHeight: pillLabel.implicitHeight + 8

    Rectangle {
        id: pill
        anchors.fill: parent
        radius: Theme.radiusSm
        color: menu.visible ? Theme.controlBg : Theme.chip
        border.width: menu.visible ? 1 : 0
        border.color: Theme.accent
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
        HoverGlow {
            anchors.fill: parent
            radius: parent.radius
            hovered: hover.containsMouse && !menu.visible
            glowColor: Theme.hoverBg
        }

        Text {
            id: pillLabel
            anchors.centerIn: parent
            text: root.text
            font.family: Theme.fontFamily
            font.pixelSize: 10
            font.weight: Font.Medium
            color: Theme.textSecondary
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                if (menu.visible) {
                    menu.visible = false
                    return
                }
                menu.openAt(pill, 0, pill.height + 4, root.Window.contentItem)
            }
        }
    }

    DropdownPanel {
        id: menu
        visible: false
        model: {
            const list = []
            for (let i = 0; i < root.modes.length; ++i) {
                const m = root.modes[i]
                const fps = root.modeFps(m)
                // Gated, not hidden: a mode the device can't reach stays
                // visible but greyed (maxFps 0 = unknown = everything open).
                const gated = root.maxFps > 0 && fps > root.maxFps
                list.push({
                    label: m,
                    value: m,
                    disabled: gated,
                    trailing: gated ? qsTr("exceeds max") : ""
                })
            }
            return list
        }
        maxHeight: 220
        onItemActivated: (label) => {
            menu.visible = false
            root.modePicked(label)
        }
    }

    // Click-outside closes; wheel does NOT dismiss (field-attached
    // combobox — same contract as SelectField).
    MenuCatcher {
        menu: menu
        closesOnWheel: false
    }
}