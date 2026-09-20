import QtQuick
import VGRPresenterUI
import "."

// One labeled settings SELECT box — the sibling of SettingsField for
// choices instead of free text (the reference dialogs' Resolution /
// Refresh rate boxes). A value box that opens a shared DropdownPanel
// underneath; the panel's items come from `options`
// ([{ label, value }] — value optional, defaults to label).
//
// The label is a BINDING-FRIENDLY property, not fixed text: bind it to the
// consumer's kind/selection so it can follow ("Device" ⇄ "Media") while
// the options list stays whatever the consumer feeds it.
//
// Contract: controlled component — `value` is owned by the consumer, the
// field reports via valuePicked(label). Options open under the box,
// clamped to `bounds` (pass root.Window.contentItem), and the list scrolls
// (DropdownPanel.maxHeight) when it would run past the cap.
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
            width: parent.width
            height: 34
            radius: Theme.radiusMd
            color: Theme.inset
            border.width: 1
            border.color: menu.visible ? Theme.accent : Theme.border
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

            Text {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "⌄"
                color: Theme.textMuted
                font.pixelSize: Theme.textMd
            }

            MouseArea {
                anchors.fill: parent
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
        visible: false
        model: {
            const list = []
            for (let i = 0; i < root.options.length; ++i) {
                const o = root.options[i]
                // `disabled` rides along (info-only rows — e.g. the NDI
                // "not enabled yet" notice — render dimmed and refuse
                // activation inside DropdownPanel).
                list.push({ label: o.label, value: o.value !== undefined ? o.value : o.label,
                            disabled: o.disabled === true })
            }
            return list
        }
        maxHeight: root.menuMaxHeight
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

    // Click/scroll contract: click-outside closes; scrolling does NOT —
    // this is a field-attached combobox, so the page scrolls under the open
    // menu and the menu rides along with its field (native combobox feel).
    // closesOnWheel: false opts out of MenuCatcher's scroll-dismiss (which
    // made the dropdown vanish on the first wheel tick — it reads as the
    // dropdown "disappearing" the moment you scroll the settings page);
    // wheelTarget forwards the scroll to the page Flickable the field sits
    // in.
    MenuCatcher {
        menu: menu
        closesOnWheel: false
    }
}
