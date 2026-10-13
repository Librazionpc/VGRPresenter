import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// "Select Camera Source" picker — pops up when "Camera" is picked from the
// Edit screen's "+" Add Content menu, matching
// 1BBTIwaya/VGRPresenter_Main_Screen_Edit_Add_Camera.qml's cam_modal_*/
// cam_card*/cam_btn_* elements (a flat Figma export with no reusable QML of
// its own — rebuilt here as a real, model-driven component, same convention
// as BackgroundColorModal.qml: scrim + centered card, `open` to show/hide,
// `applied`/`cancelled` signals, no application state of its own beyond the
// picker UI itself).
//
// `sources` is a plain list of { id, name, description, status } — status
// is "live" or "offline"; an offline source is still shown (browsable) but
// visually dimmed and not selectable, matching the ground truth's greyed
// "Webcam · Not connected" card. Consumers read `applied`'s payload (the
// picked source object) and decide what a "camera" canvas item does with it
// (e.g. label it) — this component doesn't own canvas state.
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false

    property var sources: [
        { id: "cam1", name: qsTr("CAM 1"), description: qsTr("Main platform camera"), status: "live" },
        { id: "cam2", name: qsTr("CAM 2"), description: qsTr("Side angle / choir view"), status: "live" },
        { id: "webcam", name: qsTr("Webcam"), description: qsTr("Not connected"), status: "offline" },
        { id: "ndi_stage", name: qsTr("NDI · Stage Cam"), description: qsTr("Network video source"), status: "live" }
    ]

    property string searchQuery: ""
    readonly property var filteredSources: root.searchQuery.length === 0
        ? root.sources
        : root.sources.filter((s) => s.name.toLowerCase().includes(root.searchQuery.toLowerCase()))

    // Selected by id rather than index — stable across the list being
    // filtered by search.
    property string selectedId: root.sources.length > 0 ? root.sources[0].id : ""
    readonly property var selectedSource: root.sources.find((s) => s.id === root.selectedId) ?? null

    // Fired when "Add Camera" is clicked, carrying the picked source object.
    signal applied(var source)
    signal cancelled()

    anchors.fill: parent
    visible: root.open

    // Dim scrim behind the card; clicking it cancels, same as every other
    // overlay in this app (see BackgroundColorModal.qml). Shared ModalScrim
    // — also consumes wheel so the page behind can't scroll through it.
    // The dialog belongs to the WINDOW, not to the pane that declares it. A modal
    // declared inside a settings page (every Add / Edit dialog in Settings) must dim
    // the whole app, not just that page's content column: parented in place its scrim
    // covered one side of the settings card, so the dialog read as half-modal (user
    // report: "the black box doesn't cover the whole settings modal, only one side").
    // Reparented to the window overlay, this component has no scaled ancestor to
    // inherit a scale from, so its CARD applies the app scale itself (see AppScale.qml).
    parent: Overlay.overlay

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        // The app's one scale, on the card (see AppScale.qml) — never on the root,
        // whose scrim IS the dialog's black box and has to stay window-sized.
        scale: AppScale.cardScale(card)
        transformOrigin: Item.Center
        anchors.centerIn: parent
        width: 640
        height: content.height + 40
        radius: 14
        color: Theme.surface
        border.color: Theme.border
        border.width: 1

        // Swallows clicks so they don't fall through to the scrim behind it.
        MouseArea { anchors.fill: parent; onClicked: {} }

        Column {
            id: content
            x: 24
            y: 20
            width: parent.width - 48
            spacing: 16

            Column {
                width: parent.width
                spacing: 4

                Item {
                    width: parent.width
                    height: closeBtn.height

                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Select Camera Source")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        id: closeBtn
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 28
                        height: 28
                        radius: 7
                        color: closeArea.containsMouse ? Theme.hoverBg : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: Theme.textSecondary
                            font.pixelSize: 14
                        }

                        MouseArea {
                            id: closeArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.cancelled()
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: qsTr("Choose a live camera feed to add to this slide")
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 14
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            // Search field — filters `sources` by name as you type.
            Rectangle {
                width: parent.width
                height: 32
                radius: 8
                color: Theme.inset
                border.color: Theme.borderSubtle
                border.width: 1

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    IconGlyph {
                        anchors.verticalCenter: parent.verticalCenter
                        name: "search"
                        color: Theme.textMuted
                        width: 12; height: 12
                        }

                    TextInput {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 24
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 14
                        selectByMouse: true
                        onTextChanged: root.searchQuery = text

                        Text {
                            visible: parent.text.length === 0
                            text: qsTr("Search cameras...")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 14
                        }
                    }
                }
            }

            // Camera source cards — two columns, matching the ground truth's
            // 288-wide cards with 16px gutter in a 592-wide content area.
            Grid {
                width: parent.width
                columns: 2
                columnSpacing: 16
                rowSpacing: 16

                Repeater {
                    model: root.filteredSources
                    delegate: Rectangle {
                        id: sourceCard
                        required property var modelData
                        readonly property bool isOffline: sourceCard.modelData.status === "offline"
                        readonly property bool selected: !sourceCard.isOffline && root.selectedId === sourceCard.modelData.id

                        width: (parent.width - 16) / 2
                        height: 160
                        radius: 10
                        color: "#0d0f16"
                        border.width: sourceCard.selected ? 1.5 : 1
                        border.color: sourceCard.selected ? Theme.accent : Theme.borderSubtle
                        opacity: sourceCard.isOffline ? 0.6 : 1
                        Behavior on border.color { ColorAnimation { duration: 100 } }

                        Row {
                            x: 14
                            y: 14
                            spacing: 6

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 6
                                height: 6
                                radius: 3
                                color: sourceCard.isOffline ? Theme.textMuted : "#2ed573"
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: sourceCard.isOffline ? qsTr("OFFLINE") : qsTr("LIVE")
                                color: sourceCard.isOffline ? Theme.textMuted : "#2ed573"
                                font.family: Theme.fontFamily
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                            }
                        }

                        // Simple centered lens glyph standing in for the
                        // ground truth's rendered camera-lens artwork — a
                        // real live-preview visual is future work (same
                        // status as the canvas's own camera placeholder).
                        IconGlyph {
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: -14
                            name: "camera"
                            fit: true
                            color: sourceCard.isOffline ? "#3a4155" : "#4a6b58"
                            width: 34; height: 34
                        }

                        Column {
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 12
                            width: parent.width
                            spacing: 2

                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                text: sourceCard.modelData.name
                                color: sourceCard.isOffline ? Theme.textSecondary : Theme.textPrimary
                                font.family: Theme.fontFamily
                                font.pixelSize: 15
                                font.weight: Font.Medium
                            }
                            Text {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                text: sourceCard.modelData.description
                                // The offline card's blurb is a shade dimmer than a live one's
                                // (Theme has no token below textMuted, so it is derived).
                                color: sourceCard.isOffline ? Qt.darker(Theme.textMuted, 1.2) : Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: !sourceCard.isOffline
                            cursorShape: sourceCard.isOffline ? Qt.ArrowCursor : Qt.PointingHandCursor
                            onClicked: root.selectedId = sourceCard.modelData.id
                        }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: Theme.border }

            Row {
                anchors.right: parent.right
                spacing: 10

                Rectangle {
                    width: 100
                    height: 36
                    radius: 10
                    color: cancelArea.containsMouse ? Theme.hoverBg : Theme.inset
                    border.color: Theme.borderSubtle
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.Medium
                    }

                    MouseArea {
                        id: cancelArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.cancelled()
                    }
                }

                Rectangle {
                    width: 128
                    height: 36
                    radius: 10
                    enabled: root.selectedSource !== null
                    opacity: root.selectedSource !== null ? 1 : 0.4
                    color: addArea.containsMouse ? Qt.darker(Theme.accent, 1.1) : Theme.accent
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Add Camera")
                        color: "#ffffff"
                        font.family: Theme.fontFamily
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: addArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.applied(root.selectedSource)
                    }
                }
            }
        }
    }
}
