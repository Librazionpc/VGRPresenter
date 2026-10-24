import QtQuick
import VGRPresenterUI

// Reusable inline group selector. The roster is read from GroupListModel and
// the flyout owns its own close behavior, so callers only handle groupPicked.
Item {
    id: root
    property string selectedGroup: ""
    // Flat: this field IS a band of its parent surface (the action step card's
    // parameter band) — no frame of its own, just the hover wash.
    property bool flat: false
    signal groupPicked(string name)

    implicitWidth: 220
    implicitHeight: 32

    readonly property var selectedGroupData: {
        void GroupListModel.rosterChanged
        for (let i = 0; i < GroupListModel.rowCount(); ++i) {
            const group = GroupListModel.getGroup(i)
            if (group.name === root.selectedGroup)
                return group
        }
        return null
    }
    readonly property var menuModel: {
        void GroupListModel.rosterChanged
        const menu = []
        for (let i = 0; i < GroupListModel.rowCount(); ++i) {
            const group = GroupListModel.getGroup(i)
            menu.push({ label: group.name, payload: group.name })
        }
        if (menu.length === 0)
            menu.push({ label: qsTr("No groups available"), disabled: true })
        return menu
    }

    Rectangle {
        id: field
        anchors.fill: parent
        radius: root.flat ? 0 : Theme.radiusMd
        color: root.flat ? "transparent" : Theme.card
        border.width: root.flat ? 0 : 1
        border.color: Theme.border
        HoverGlow {
            anchors.fill: parent
            radius: parent.radius
            hovered: fieldHover.hovered
            glowColor: Theme.hoverBg
        }
        Row {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 8
            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                name: "listBullets"
                color: root.selectedGroupData ? root.selectedGroupData.color : Theme.textMuted
                width: 14; height: 14
                fit: true
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 42
                text: root.selectedGroup !== "" ? root.selectedGroup : qsTr("Choose group")
                color: root.selectedGroup !== "" ? Theme.textPrimary : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                elide: Text.ElideRight
            }
            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                name: "chevronDown"
                color: Theme.textMuted
                width: 10; height: 10
                fit: true
            }
        }
        HoverHandler { id: fieldHover }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: groupMenu.openAt(field, 0, field.height)
        }
    }

    DropdownPanel {
        id: groupMenu
        visible: false
        z: 40000
        maxHeight: 280
        model: root.menuModel
        onItemPicked: (index, payload) => {
            void index
            groupMenu.visible = false
            if (payload !== undefined && payload !== "")
                root.groupPicked(String(payload))
        }
    }

    Component.onDestruction: groupMenu.visible = false
}
