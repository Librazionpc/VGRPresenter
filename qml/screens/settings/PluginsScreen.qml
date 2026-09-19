import QtQuick
import VGRPresenterUI
import "../../components"

// Settings · Plugins — the installed-plugins list, same minimal skeleton
// as the other settings screens (Item root, Flickable, shared
// AppScrollBar, Theme tokens). One card of rows: name + version/tag
// subtitle, a status pill, and an enable toggle. The pill is DERIVED, not
// stored — "Active" (green) while enabled, "Installed" (amber) when
// switched off — so the row can never show contradictory state.
Item {
    id: root

    // name / version / tag are read-only facts; `enabled` is the setting.
    property var plugins: [
        { name: qsTr("Song Provider"), version: "v1.2", tag: qsTr("chords + transpose"), enabled: true },
        { name: qsTr("Bible Provider"), version: "v1.0", tag: qsTr("KJV + BBE"), enabled: true },
        { name: qsTr("NDI Broadcast"), version: "v1.1", tag: qsTr("network output"), enabled: true },
        { name: qsTr("MIDI Control"), version: "v0.9", tag: qsTr("hardware triggers"), enabled: false },
        { name: qsTr("Flow Automation"), version: "v1.0", tag: qsTr("service flows"), enabled: true }
    ]

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.height + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            y: Theme.space5
            width: flick.width - Theme.space6
            spacing: 16

            // ---- Page header ----
            Column {
                spacing: 2

                Text {
                    text: qsTr("Plugins")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXxl
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Extend VGR with providers, devices and integrations.")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                }
            }

            // ---- Installed plugins card ----
            Rectangle {
                width: parent.width
                height: pluginsCol.height + 36
                radius: Theme.radiusLg
                color: Theme.card
                border.color: Theme.border
                border.width: 1

                Column {
                    id: pluginsCol
                    x: 20
                    y: 14
                    width: parent.width - 40
                    spacing: 0

                    Text {
                        text: qsTr("Installed plugins")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        bottomPadding: 6
                    }

                    Repeater {
                        model: root.plugins
                        delegate: Column {
                            id: pluginRow
                            required property var modelData
                            required property int index
                            width: pluginsCol.width

                            Item {
                                width: pluginRow.width
                                height: 46

                                Column {
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2

                                    Text {
                                        text: pluginRow.modelData.name
                                        color: Theme.textPrimary
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textSm
                                    }
                                    Text {
                                        text: pluginRow.modelData.version + " · " + pluginRow.modelData.tag
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: Theme.textXs
                                    }
                                }

                                // Status pill — derived from the toggle.
                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: pillLabel.width + 20
                                    height: 20
                                    radius: 10
                                    color: pluginRow.modelData.enabled ? "#264ade80" : "#26f5c26b"

                                    Text {
                                        id: pillLabel
                                        anchors.centerIn: parent
                                        text: pluginRow.modelData.enabled ? qsTr("Active") : qsTr("Installed")
                                        color: pluginRow.modelData.enabled ? Theme.success : Theme.warning
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 10
                                        font.weight: Font.Medium
                                    }
                                }

                                SettingsToggle {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    checked: pluginRow.modelData.enabled
                                    onToggled: {
                                        const copy = root.plugins.slice()
                                        copy[pluginRow.index] = Object.assign({}, pluginRow.modelData, { enabled: !pluginRow.modelData.enabled })
                                        root.plugins = copy
                                    }
                                }
                            }
                            Rectangle { width: pluginRow.width; height: 1; color: Theme.border; visible: pluginRow.index < root.plugins.length - 1 }
                        }
                    }
                }
            }

            // ---- Browse plugin store ----
            Rectangle {
                width: 160
                height: 36
                radius: Theme.radiusMd
                color: browseArea.containsMouse ? Theme.chip : Theme.inset
                border.color: Theme.border
                border.width: 1
                Behavior on color { ColorAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    text: qsTr("Browse plugin store")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSm
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: browseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    // The plugin store is its own future surface — no
                    // ground-truth design exists for it yet.
                    onClicked: {}
                }
            }
        }
    }

    // Shared app scrollbar at the fixed right edge (sibling of the
    // Flickable — see GeneralScreen's note).
    AppScrollBar {
        x: parent.width - (Theme.space6 + Theme.space2 + width) / 2
        y: Theme.space3
        height: parent.height - Theme.space6
        flickable: flick
    }
}
