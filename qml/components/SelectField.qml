import QtQuick
import VGRPresenterUI
import "."

Item {
    id: root

    property string label: ""
    // Two-column forms embed the field beside their own label — hide this
    // component's caption line entirely (no reserved empty line).
    property bool showLabel: true
    property string value: ""
    property string placeholder: ""
    // [{ label: "...", value: "..." }] — value falls back to label.
    property var options: []
    // Panel height cap — a long list (real device enumeration) scrolls
    // instead of running off-screen. 0 = size to content.
    property int menuMaxHeight: 240

    signal valuePicked(string value)

    implicitWidth: 200
    implicitHeight: labelCol.implicitHeight

    Column {
        id: labelCol
        width: parent.width
        spacing: 6

        Text {
            visible: root.showLabel
            text: root.label
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }

        Rectangle {
            id: box
            objectName: root.objectName === "" ? "" : root.objectName + "_box"
            width: parent.width
            height: 36
            radius: Theme.radiusMd
            color: Theme.inset
            border.width: 1
            border.color: menu.visible ? Theme.accent
                        : boxArea.containsMouse ? Theme.textMuted : Theme.border
            Behavior on border.color { ColorAnimation { duration: 100 } }

            Text {
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 40
                text: root.value !== "" ? root.value : root.placeholder
                color: root.value !== "" ? Theme.textPrimary : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                elide: Text.ElideRight
            }

            IconGlyph {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                // Optical centering: the chevron glyph's mass sits ~2px high
                // in its em box, so a geometric center reads as high.
                anchors.verticalCenterOffset: 2
                name: menu.visible ? "chevronUp" : "chevronDown"
                color: menu.visible || boxArea.containsMouse ? Theme.textSecondary : Theme.textMuted
                width: 12; height: 12
            }

            MouseArea {
                id: boxArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (menu.visible) {
                        menu.visible = false
                        return
                    }
                    menu.openAt(box, 0, box.height + 4, root.Window.contentItem)
                }
            }
        }
    }

    DropdownPanel {
        id: menu
        // Same hook as the box above (see its comment).
        objectName: root.objectName === "" ? "" : root.objectName + "_menu"
        visible: false
        model: {
            const list = []
            for (let i = 0; i < root.options.length; ++i) {
                const o = root.options[i]
                const v = o.value !== undefined ? o.value : o.label
                // The chosen entry is tinted + accent-coloured in the list (the same
                // "current" cue the font picker uses), so the list shows where you are.
                list.push({ label: o.label, value: v, disabled: o.disabled === true,
                            accent: root.value !== "" && (o.label === root.value || String(v) === root.value) })
            }
            return list
        }
        maxHeight: root.menuMaxHeight
        matchAnchorWidth: true
        onItemActivated: (label) => {
            // Resolve the option's value from its label (labels are unique
            // per select — they're what's displayed).
            let picked = label
            for (let i = 0; i < root.options.length; ++i) {
                if (root.options[i].label === label) {
                    picked = root.options[i].value !== undefined ? root.options[i].value : label
                    break
                }
            }
            menu.visible = false
            root.valuePicked(picked)
        }
    }

    MenuCatcher {
        menu: menu
        closesOnWheel: false
    }
}