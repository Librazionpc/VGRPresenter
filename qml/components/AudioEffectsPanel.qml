import QtQuick
import VGRPresenterUI
import "."

Column {
    id: root

    property var effects: []
    property string selectedKey: ""

    signal effectSelected(string key)
    signal effectToggled(string key)
    signal effectValueMoved(string key, real value)

    spacing: Theme.space3

    readonly property var selectedEffect: {
        for (let i = 0; i < root.effects.length; ++i) {
            if (root.effects[i].key === root.selectedKey)
                return root.effects[i]
        }
        return null
    }

    Grid {
        id: rack
        width: parent.width
        columns: 4
        spacing: Theme.space2

        Repeater {
            model: root.effects

            delegate: Rectangle {
                id: chip
                required property var modelData

                width: (rack.width - rack.spacing * (rack.columns - 1)) / rack.columns
                height: 44
                radius: Theme.radiusMd
                color: chip.modelData.enabled
                       ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.12)
                       : Theme.inset
                border.width: root.selectedKey === chip.modelData.key ? 1.5 : 1
                border.color: root.selectedKey === chip.modelData.key ? Theme.accent : Theme.border
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Column {
                    x: 10
                    y: 8
                    width: parent.width - 26
                    spacing: 1

                    Text {
                        width: parent.width
                        text: chip.modelData.label
                        color: chip.modelData.enabled ? Theme.textPrimary : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: chip.modelData.value + chip.modelData.suffix
                        color: chip.modelData.enabled ? Theme.textSecondary : Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textXs
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.effectSelected(
                        root.selectedKey === chip.modelData.key ? "" : chip.modelData.key)
                }

                // The green dot = TOGGLE (and select, since flipping an
                // effect on is usually the moment you want to edit it).
                Rectangle {
                    id: dot
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.top: parent.top
                    anchors.topMargin: 8
                    width: 10
                    height: 10
                    radius: 5
                    color: chip.modelData.enabled ? Theme.success : "transparent"
                    border.width: 1.4
                    border.color: chip.modelData.enabled ? Theme.success : Theme.borderSubtle
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -8
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.effectToggled(chip.modelData.key)
                            root.effectSelected(chip.modelData.key)
                        }
                    }
                }
            }
        }
    }

    // Selected-effect editor — appears only once something is selected.
    Column {
        width: parent.width
        spacing: Theme.space2
        visible: root.selectedEffect !== null

        Rectangle {
            width: parent.width
            height: 1
            color: Theme.border
        }

        LabeledSlider {
            width: parent.width
            label: root.selectedEffect ? root.selectedEffect.label : ""
            value: root.selectedEffect ? root.selectedEffect.value : 0
            minValue: root.selectedEffect ? root.selectedEffect.minValue : 0
            maxValue: root.selectedEffect ? root.selectedEffect.maxValue : 100
            suffix: root.selectedEffect ? root.selectedEffect.suffix : ""
            onMoved: (v) => root.effectValueMoved(root.selectedKey, v)
        }

        Text {
            text: qsTr("Click an effect to edit its parameter — the dot toggles it on or off.")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs
        }
    }
}