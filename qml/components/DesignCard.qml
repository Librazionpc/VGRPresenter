import QtQuick
import VGRPresenterUI

// One DESIGN - an overlay or a template - in a library grid: a 16:9 preview of its blocks
// (DesignPreview, drawn from what the ENGINE keeps) over a dark stage, and its name under it.
// Pointing at the card shows small actions on the preview - open it in the Edit screen, file it
// in a category, rename, duplicate, delete - and a RIGHT-CLICK on the card offers the same actions as a menu
// (contextMenuRequested; the host builds and shows the menu). Each is reported as a signal; the card changes
// nothing itself.
//
// A design that stays on screen when the slide changes (locked) carries a small padlock.
Item {
    id: root

    // { id, name, color, category, isDefault, locked, blocks, ... } from DesignLibraryService.designs().
    property var design: ({})
    property date now: new Date()

    readonly property real previewHeight: Math.round((width - 12) * 9 / 16)
    readonly property bool hovered: hover.hovered

    // `anchor` is the category button, so the host can open its menu right there.
    signal editRequested()
    signal categoryRequested(Item anchor)
    signal renameRequested()
    signal duplicateRequested()
    signal deleteRequested()
    // Right-click: `source` is this card and (mx, my) the point in its coordinates - the host opens its menu there.
    signal contextMenuRequested(Item source, real mx, real my)
    // The name was edited in place (double-click it): the new, non-empty, changed name.
    signal renameCommitted(string name)

    function beginRename() {
        nameEdit.text = root.design.name !== undefined ? root.design.name : ""
        nameEdit.visible = true
        nameEdit.forceActiveFocus()
        nameEdit.selectAll()
    }
    function finishRename(commit) {
        if (!nameEdit.visible)
            return
        const text = nameEdit.text.trim()
        nameEdit.visible = false            // (losing focus calls this again; the guard above ends that)
        if (commit && text !== "" && text !== root.design.name)
            root.renameCommitted(text)
    }

    // Underneath everything, so the actions on the preview (which sit above it) get their clicks.
    PositionHoverArea {
        id: hover
        anchors.fill: parent
        showCursor: false
    }

    Rectangle {
        id: card
        anchors.fill: parent
        anchors.margins: 6
        radius: 6
        clip: true
        color: "#16171e"
        border.width: hover.hovered ? 1 : 0
        border.color: "#4a4d5e"

        // ---- preview ----
        Rectangle {
            id: stage
            width: parent.width
            height: root.previewHeight
            color: "#0d0e14"

            DesignPreview {
                anchors.fill: parent
                blocks: root.design.blocks !== undefined ? root.design.blocks : []
                background: root.design.background !== undefined ? root.design.background : "transparent"
                now: root.now
            }

            // The accent colour the design was given.
            Rectangle {
                visible: root.design.color !== undefined && root.design.color !== ""
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: parent.width; height: 2
                color: root.design.color !== undefined ? root.design.color : "transparent"
            }

            // ---- actions, while pointed at ----
            Row {
                visible: hover.hovered
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 6
                spacing: 4

                component CardAction : Rectangle {
                    id: actionBtn
                    property string icon: ""
                    property string tip: ""
                    signal activated()
                    width: 24; height: 24; radius: 5
                    color: actHover.hovered ? "#2a2c3a" : "#191a24"
                    border.width: 1
                    border.color: "#333648"
                    IconGlyph {
                        anchors.centerIn: parent
                        name: actionBtn.icon
                        color: "#c7cdd8"
                        width: 12; height: 12
                    }
                    PositionHoverArea {
                        id: actHover
                        anchors.fill: parent
                        showCursor: true
                        // The action row itself is visible only while the card is hovered;
                        // re-walking that chain from here is what made the binding loop.
                        checkAncestors: false
                        onClicked: actionBtn.activated()
                    }
                }

                CardAction {
                    icon: "penTool"
                    tip: qsTr("Edit in the canvas")
                    objectName: "selfTestDesignEdit_" + root.design.name
                    onActivated: root.editRequested()
                }
                CardAction {
                    id: fileAction
                    icon: "folder"
                    tip: qsTr("File it in a category")
                    objectName: "selfTestDesignFile_" + root.design.name
                    onActivated: root.categoryRequested(fileAction)
                }
                CardAction {
                    icon: "fileText"
                    tip: qsTr("Rename")
                    objectName: "selfTestDesignRename_" + root.design.name
                    onActivated: root.renameRequested()
                }
                CardAction {
                    icon: "layers"
                    tip: qsTr("Duplicate")
                    objectName: "selfTestDesignDuplicate_" + root.design.name
                    onActivated: root.duplicateRequested()
                }
                CardAction {
                    icon: "close"
                    tip: qsTr("Delete")
                    objectName: "selfTestDesignDelete_" + root.design.name
                    onActivated: root.deleteRequested()
                }
            }

            // what stays on screen when the slide changes
            Row {
                visible: root.design.locked === true
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 6
                spacing: 4

                Rectangle {
                    visible: root.design.locked === true
                    width: 18; height: 18; radius: 9
                    color: "#20222e"
                    IconGlyph { anchors.centerIn: parent; name: "lock"; color: "#8f96a8"; width: 10; height: 10 }
                }
            }
        }

        // ---- name (double-click to rename) ----
        Item {
            width: parent.width
            height: 30
            y: root.previewHeight

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 16
                visible: !nameEdit.visible
                text: root.design.name !== undefined ? root.design.name : ""
                color: Theme.textPrimary
                font.family: Theme.fontFamily; font.pixelSize: 12
                elide: Text.ElideRight
            }

            Rectangle {
                visible: nameEdit.visible
                anchors.fill: parent
                anchors.margins: 3
                radius: 4
                color: "#0f1015"
                border.width: 1
                border.color: "#6c5ce7"
            }
            TextInput {
                id: nameEdit
                objectName: "selfTestDesignNameEdit"
                visible: false
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.textPrimary
                font.family: Theme.fontFamily; font.pixelSize: 12
                selectByMouse: true
                clip: true
                onAccepted: root.finishRename(true)
                Keys.onEscapePressed: root.finishRename(false)
                onActiveFocusChanged: if (!activeFocus) root.finishRename(true)
            }

            MouseArea {
                objectName: "selfTestDesignName"
                anchors.fill: parent
                enabled: !nameEdit.visible
                acceptedButtons: Qt.LeftButton
                onDoubleClicked: root.beginRename()
            }
        }
    }

    // Right-click: the same actions as the hover buttons, for people who reach for the context menu. Accepts only the
    // right button, so a left click still reaches the card and the action buttons underneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: (mouse) => root.contextMenuRequested(root, mouse.x, mouse.y)
    }
}
