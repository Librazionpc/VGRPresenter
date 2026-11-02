import QtQuick
import VGRPresenterUI

Item {
    id: root

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.rightMargin: Theme.space6 + Theme.space2
        contentWidth: width
        contentHeight: layout.height + Theme.space6 * 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: layout
            x: Theme.space6
            y: Theme.space6
            width: flick.width - Theme.space6
            spacing: Theme.space6

            Column {
                spacing: Theme.space1
                Text {
                    text: qsTr("Appearance & Actions")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXl
                    font.weight: Font.DemiBold
                }
            }

            // ---- "Manage groups" row -------------------------------------
            Rectangle {
                width: layout.width
                height: 64
                radius: Theme.radiusMd
                color: Theme.card
                border.width: 1
                border.color: Theme.border

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space4
                    anchors.right: openManageGroups.left
                    anchors.rightMargin: Theme.space3
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    Text {
                        text: qsTr("Manage groups")
                        color: Theme.accentLight
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        font.weight: Font.Medium
                    }

                    Row {
                        id: previewRow
                        spacing: 6
                        readonly property var entries: {
                            void GroupListModel.rosterChanged
                            return GroupListModel.previewEntries(8)
                        }
                        Repeater {
                            model: previewRow.entries
                            delegate: Row {
                                required property var modelData
                                required property int index
                                spacing: 6
                                Text {
                                    text: modelData.name
                                    color: modelData.color
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    visible: index < previewRow.entries.length - 1
                                    text: "|"
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                }
                            }
                        }
                        Text {
                            visible: GroupListModel.rowCount() > previewRow.entries.length
                            text: "…"
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            visible: previewRow.entries.length === 0
                            text: qsTr("No groups yet")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                    }
                }

                Rectangle {
                    id: openManageGroups
                    objectName: "selfTestOpenManageGroups"
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space3
                    anchors.verticalCenter: parent.verticalCenter
                    width: 34; height: 34; radius: Theme.radiusMd
                    color: openGroupsHover.hovered ? Theme.hoverBg : "transparent"

                    IconGlyph {
                        anchors.centerIn: parent
                        name: "listBullets"
                        color: Theme.iconChrome
                        width: 16; height: 16
                        fit: true
                    }
                    MouseArea {
                        id: openGroupsHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: manageGroups.open()
                    }
                }
            }

            // ---- "Manage actions" row -------------------------------------
            Rectangle {
                width: layout.width
                height: 64
                radius: Theme.radiusMd
                color: Theme.card
                border.width: 1
                border.color: Theme.border

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space4
                    anchors.right: openManageActions.left
                    anchors.rightMargin: Theme.space3
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    Text {
                        text: qsTr("Manage actions")
                        color: Theme.accentLight
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        font.weight: Font.Medium
                    }

                    Row {
                        id: actionsPreviewRow
                        spacing: 6
                        readonly property var entries: {
                            void ActionListModel.rosterChanged
                            const out = []
                            for (let i = 0; i < ActionListModel.rowCount() && i < 8; ++i)
                                out.push(ActionListModel.getAction(i))
                            return out
                        }
                        Repeater {
                            model: actionsPreviewRow.entries
                            delegate: Row {
                                required property var modelData
                                required property int index
                                spacing: 6
                                Text {
                                    text: (modelData.name !== "" ? modelData.name : qsTr("Untitled"))
                                          + (modelData.shortcut !== "" ? " (" + modelData.shortcut + ")" : "")
                                    color: Theme.accentLight
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    visible: index < actionsPreviewRow.entries.length - 1
                                    text: "|"
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textSm
                                }
                            }
                        }
                        Text {
                            visible: ActionListModel.rowCount() > actionsPreviewRow.entries.length
                            text: "…"
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            visible: actionsPreviewRow.entries.length === 0
                            text: qsTr("No actions yet")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                        }
                    }
                }

                Rectangle {
                    id: openManageActions
                    objectName: "selfTestOpenManageActions"
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space3
                    anchors.verticalCenter: parent.verticalCenter
                    width: 34; height: 34; radius: Theme.radiusMd
                    color: openActionsHover.hovered ? Theme.hoverBg : "transparent"

                    IconGlyph {
                        anchors.centerIn: parent
                        name: "flag"
                        color: Theme.iconChrome
                        width: 16; height: 16
                        fit: true
                    }
                    MouseArea {
                        id: openActionsHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: manageActions.open()
                    }
                }
            }
        }
    }

    ManageGroupsModal {
        id: manageGroups
    }
    ManageActionsModal {
        id: manageActions
    }
}