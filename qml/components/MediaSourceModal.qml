import QtQuick

// "Select Media" picker — pops up when "Media" is picked from the Edit
// screen's "+" Add Content menu, matching
// 1BBTIwaya/VGRPresenter_Main_Screen_Edit_Add_Media.qml's media_modal_*/
// media_item*/media_btn_* elements (same flat-Figma-export-with-no-reusable-
// QML situation as VGRPresenter_Main_Screen_Edit_Add_Camera.qml). Built as
// real, model-driven QML on the same convention as CameraSourceModal.qml/
// BackgroundColorModal.qml: scrim + centered card, `open` to show/hide,
// `applied`/`cancelled` signals, no application state of its own beyond the
// picker UI itself.
//
// `items` is a plain list of { id, name, kind } — kind is "image" or
// "video" (a video card gets the small play-badge overlay the ground truth
// shows; there's no real thumbnail rendering here, same placeholder-card
// situation as the canvas's own camera/media/audio/shape/timer/clock
// visuals — real thumbnails are future work, not blocking this picker).
Item {
    id: root

    property bool open: false

    property var items: [
        { id: "sunday-bg",     name: "sunday-bg.jpg",     kind: "image" },
        { id: "worship-loop",  name: "worship-loop.mp4",  kind: "video" },
        { id: "cross-image",   name: "cross-image.png",   kind: "image" },
        { id: "stage-photo",   name: "stage-photo.jpg",   kind: "image" },
        { id: "choir-video",   name: "choir-video.mp4",   kind: "video" },
        { id: "logo-loop",     name: "logo-loop.mp4",     kind: "video" },
        { id: "sunset-bg",     name: "sunset-bg.jpg",     kind: "image" },
        { id: "title-card",    name: "title-card.png",    kind: "image" }
    ]

    // "all" | "image" | "video" — matches the ground truth's All/Images/
    // Videos tabs (their "Images"/"Videos" labels are plural UI copy for
    // the singular `kind` values items are tagged with).
    property string activeTab: "all"
    property string searchQuery: ""

    readonly property var filteredItems: root.items.filter((it) => {
        if (root.activeTab !== "all" && it.kind !== root.activeTab)
            return false
        if (root.searchQuery.length > 0 && !it.name.toLowerCase().includes(root.searchQuery.toLowerCase()))
            return false
        return true
    })

    property string selectedId: root.items.length > 0 ? root.items[0].id : ""
    readonly property var selectedItem: root.items.find((it) => it.id === root.selectedId) ?? null

    // Fired when "Insert" is clicked, carrying the picked media item.
    signal applied(var item)
    signal cancelled()

    anchors.fill: parent
    visible: root.open

    // Dim scrim behind the card; clicking it cancels, same as every other
    // overlay in this app (see BackgroundColorModal.qml). Shared ModalScrim
    // — also consumes wheel so the page behind can't scroll through it.
    ModalScrim {
        anchors.fill: parent
        onDismissed: root.cancelled()
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: 800
        height: content.height + 40
        radius: 14
        color: "#13151c"
        border.color: "#232530"
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
                        text: qsTr("Select Media")
                        color: "#f1f3f8"
                        font.family: "Segoe UI"
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
                        color: closeArea.containsMouse ? "#20222c" : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: "#8a94a6"
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
                    text: qsTr("Choose an image or video to add to this slide")
                    color: "#8a94a6"
                    font.family: "Segoe UI"
                    font.pixelSize: 14
                }
            }

            // Tabs (left) + search (right), same row like the ground truth.
            // A plain Item, not a Row — Row/Column/Grid forbid their own
            // children from using anchors.left/right/fill/centerIn (they
            // position children themselves), and both the tabs Row and the
            // search box below need anchors to sit on opposite edges.
            Item {
                width: parent.width
                height: 32

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    Repeater {
                        model: [
                            { key: "all", label: qsTr("All") },
                            { key: "image", label: qsTr("Images") },
                            { key: "video", label: qsTr("Videos") }
                        ]
                        delegate: Rectangle {
                            id: tabBtn
                            required property var modelData
                            readonly property bool active: root.activeTab === tabBtn.modelData.key

                            height: 28
                            width: tabLabel.implicitWidth + 24
                            radius: 8
                            color: tabBtn.active ? "#296c5ce7" : (tabArea.containsMouse ? "#1e2029" : "transparent")
                            border.color: tabBtn.active ? "#6c5ce7" : "transparent"
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }

                            Text {
                                id: tabLabel
                                anchors.centerIn: parent
                                text: tabBtn.modelData.label
                                color: tabBtn.active ? "#9b8ff5" : "#8a94a6"
                                font.family: "Segoe UI"
                                font.pixelSize: 14
                                font.weight: Font.Medium
                            }

                            MouseArea {
                                id: tabArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.activeTab = tabBtn.modelData.key
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 256
                    height: 32
                    radius: 8
                    color: "#1a1c26"
                    border.color: "#232530"
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
                            color: "#5c6475"
                            width: 12; height: 12
                            }

                        TextInput {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 24
                            color: "#c9cedd"
                            font.family: "Segoe UI"
                            font.pixelSize: 13
                            selectByMouse: true
                            onTextChanged: root.searchQuery = text

                            Text {
                                visible: parent.text.length === 0
                                text: qsTr("Search media...")
                                color: "#5c6475"
                                font.family: "Segoe UI"
                                font.pixelSize: 13
                            }
                        }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#232530" }

            // Media item cards — four columns, matching the ground truth's
            // 176-wide cards with 16px gutter in a 752-wide content area.
            Grid {
                width: parent.width
                columns: 4
                columnSpacing: 16
                rowSpacing: 16

                Repeater {
                    model: root.filteredItems
                    delegate: Rectangle {
                        id: mediaCard
                        required property var modelData
                        readonly property bool selected: root.selectedId === mediaCard.modelData.id
                        readonly property bool isVideo: mediaCard.modelData.kind === "video"

                        width: (parent.width - 48) / 4
                        height: 110
                        radius: 8
                        color: mediaCard.selected ? "#1a2240" : "#161823"
                        border.width: mediaCard.selected ? 1.5 : 1
                        border.color: mediaCard.selected ? "#6c5ce7" : "#262a38"
                        Behavior on border.color { ColorAnimation { duration: 100 } }
                        Behavior on color { ColorAnimation { duration: 100 } }

                        // Play badge for video items — no real thumbnail
                        // rendering, same placeholder-visual status as the
                        // canvas's own media/audio/shape/timer/clock items.
                        Rectangle {
                            visible: mediaCard.isVideo
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: -8
                            width: 28
                            height: 28
                            radius: 14
                            color: "#66000000"

                            Text {
                                anchors.centerIn: parent
                                anchors.horizontalCenterOffset: 1
                                text: "▶"
                                color: "#ffffff"
                                font.pixelSize: 13
                            }
                        }

                        Text {
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 10
                            width: parent.width
                            horizontalAlignment: Text.AlignHCenter
                            text: mediaCard.modelData.name
                            color: "#c8cdd9"
                            font.family: "Segoe UI"
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectedId = mediaCard.modelData.id
                        }
                    }
                }
            }

            Rectangle { width: parent.width; height: 1; color: "#232530" }

            Row {
                anchors.right: parent.right
                spacing: 10

                Rectangle {
                    width: 100
                    height: 36
                    radius: 10
                    color: cancelArea.containsMouse ? "#20222c" : "#1a1c26"
                    border.color: "#2a2f3a"
                    border.width: 1
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: "#c8cdd9"
                        font.family: "Segoe UI"
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
                    width: 100
                    height: 36
                    radius: 10
                    enabled: root.selectedItem !== null
                    opacity: root.selectedItem !== null ? 1 : 0.4
                    color: insertArea.containsMouse ? "#5a4cd6" : "#6c5ce7"
                    Behavior on color { ColorAnimation { duration: 100 } }

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("Insert")
                        color: "#ffffff"
                        font.family: "Segoe UI"
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        id: insertArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.applied(root.selectedItem)
                    }
                }
            }
        }
    }
}
