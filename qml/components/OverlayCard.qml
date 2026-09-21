import QtQuick
import VGRPresenterUI

// One overlay in the Overlays grid: a 16:9 preview of it (OverlayPreview, drawn from the elements the
// engine keeps) over a dark stage, and its name under it. Pointing at the card shows four small actions
// on the preview - file it in a category, rename, duplicate, delete - each reported as a signal; the card
// changes nothing itself.
//
// An overlay that came with the app carries a small "Default" chip and one that stays on screen when the
// slide changes a padlock.
Item {
    id: root

    // { id, name, color, category, isDefault, locked, displayDuration, elements } from OverlayLibraryService.
    property var overlay: ({})
    property date now: new Date()

    readonly property real previewHeight: Math.round((width - 12) * 9 / 16)
    readonly property bool hovered: hover.hovered

    // `anchor` is the category button, so the host can open its menu right there.
    signal categoryRequested(Item anchor)
    signal renameRequested()
    signal duplicateRequested()
    signal deleteRequested()

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

            // A faint stage so a light overlay (a vignette, a frame) reads as an overlay, not as a picture.
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#1b1c26" }
                    GradientStop { position: 1.0; color: "#101118" }
                }
            }
            OverlayPreview {
                anchors.fill: parent
                elements: root.overlay.elements !== undefined ? root.overlay.elements : []
                now: root.now
            }

            // The accent colour the overlay was given.
            Rectangle {
                visible: root.overlay.color !== undefined && root.overlay.color !== ""
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                width: parent.width; height: 2
                color: root.overlay.color !== undefined ? root.overlay.color : "transparent"
            }

            // ---- actions, while pointed at ----
            Row {
                visible: hover.hovered
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 6
                spacing: 4

                Repeater {
                    model: [
                        { icon: "folder",   tip: "category",  signalName: "category" },
                        { icon: "penTool",  tip: "rename",    signalName: "rename" },
                        { icon: "layers",   tip: "duplicate", signalName: "duplicate" },
                        { icon: "close",    tip: "delete",    signalName: "delete" }
                    ]
                    delegate: Rectangle {
                        id: action
                        required property var modelData
                        width: 24; height: 24; radius: 6
                        color: actionArea.pressed ? "#2a2d3a" : (actionArea.containsMouse ? "#2a2d3a" : "#cc12131a")
                        border.width: 1
                        border.color: "#33364a"
                        IconGlyph {
                            anchors.centerIn: parent
                            name: action.modelData.icon
                            color: action.modelData.signalName === "delete" && actionArea.containsMouse ? Theme.danger : Theme.textPrimary
                            width: 11; height: 11
                        }
                        // A plain MouseArea: this row is only visible while the card is hovered, so a hover area
                        // here would read its ancestors' visibility and loop.
                        MouseArea {
                            id: actionArea
                            objectName: "overlayAction_" + action.modelData.signalName
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                switch (action.modelData.signalName) {
                                case "category": root.categoryRequested(action); break
                                case "rename": root.renameRequested(); break
                                case "duplicate": root.duplicateRequested(); break
                                default: root.deleteRequested()
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- name bar ----
        Item {
            y: stage.height
            width: parent.width
            height: parent.height - stage.height

            Rectangle {
                id: dot
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 8; height: 8; radius: 4
                color: root.overlay.color !== undefined && root.overlay.color !== "" ? root.overlay.color : Theme.textMuted
            }
            Text {
                x: 24
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 24 - badges.width - 14
                text: root.overlay.name !== undefined ? root.overlay.name : ""
                color: Theme.textPrimary
                elide: Text.ElideRight
                font.family: Theme.fontFamily; font.pixelSize: 12
            }
            Row {
                id: badges
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                IconGlyph {
                    visible: root.overlay.locked === true
                    anchors.verticalCenter: parent.verticalCenter
                    name: "lock"
                    color: Theme.textMuted
                    width: 11; height: 11
                }
                Text {
                    visible: root.overlay.isDefault === true
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Default")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 10
                }
            }
        }
    }
}
