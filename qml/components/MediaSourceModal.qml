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
// Reused by Settings · Audio & Video's Media-kind "Browse..." option (an
// audio/video input's Source field) — the same grid picker instead of a
// bare native file dialog, restricted to one kind there via `fixedKind`.
//
// `items` defaults to the real Media Library (MediaLibraryService.items()),
// each { id, name, kind, path } — kind is "image" | "video" | "audio" (a
// video card gets the small play-badge overlay, audio a note badge; there's
// no real thumbnail rendering here, same placeholder-card situation as the
// canvas's own camera/media/audio/shape/timer/clock visuals — real
// thumbnails are future work, not blocking this picker). A consumer that
// wants a different set can still override `items` directly.
Item {
    id: root

    property bool open: false

    readonly property var libraryItems: {
        void MediaLibraryService.totalCount
        const list = MediaLibraryService.items()
        const out = []
        for (let i = 0; i < list.length; i++) {
            const it = list[i]
            out.push({ id: it.id, name: it.name, kind: it.kind, path: it.path })
        }
        return out
    }
    property var items: root.libraryItems

    // Single-kind mode — Settings · Audio & Video only ever wants ONE kind
    // (an audio input's Source can't offer video files). Non-empty hides
    // the tab row entirely and locks filtering to it.
    property string fixedKind: ""

    // "all" | "image" | "video" | "audio" — matches the ground truth's All/
    // Images/Videos tabs (their plural UI copy for the singular `kind`
    // values items are tagged with), plus Audio for the Settings reuse.
    property string activeTab: root.fixedKind !== "" ? root.fixedKind : "all"
    property string searchQuery: ""

    readonly property var filteredItems: root.items.filter((it) => {
        const tab = root.fixedKind !== "" ? root.fixedKind : root.activeTab
        if (tab !== "all" && it.kind !== tab)
            return false
        if (root.searchQuery.length > 0 && !it.name.toLowerCase().includes(root.searchQuery.toLowerCase()))
            return false
        return true
    })

    property string selectedId: ""
    readonly property var selectedItem: root.items.find((it) => it.id === root.selectedId) ?? null
    // Reset the pick whenever the modal (re)opens on a fresh item set —
    // an open with no items yet (async library scan) must not carry the
    // PREVIOUS session's pick forward as a silently-wrong default.
    onOpenChanged: if (root.open) root.selectedId = ""

    // Fired when "Insert" is clicked, carrying the picked media item.
    signal applied(var item)
    signal cancelled()
    // "Browse this PC..." clicked — the consumer resolves it (native
    // picker); this component has no PAL access of its own.
    signal browseRequested()

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
                    text: root.fixedKind === "audio" ? qsTr("Choose an audio file for this source")
                        : root.fixedKind === "video" ? qsTr("Choose a video file for this source")
                        : qsTr("Choose an image or video to add to this slide")
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
                    visible: root.fixedKind === ""
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 4

                    Repeater {
                        model: [
                            { key: "all", label: qsTr("All") },
                            { key: "image", label: qsTr("Images") },
                            { key: "video", label: qsTr("Videos") },
                            { key: "audio", label: qsTr("Audio") }
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
                        readonly property bool isAudio: mediaCard.modelData.kind === "audio"
                        // Audio has no visual frame to fetch — never even
                        // asks the engine for one; video/image do, and fall
                        // back to the icon below while it loads or if the
                        // engine has no decoder for that file (the provider
                        // answers with a 1x1 transparent image, matching
                        // MediaTile.qml's own hasPicture convention).
                        readonly property bool hasPicture: !mediaCard.isAudio
                            && still.status === Image.Ready && still.sourceSize.width > 1

                        width: (parent.width - 48) / 4
                        height: 110
                        radius: 8
                        clip: true
                        color: mediaCard.selected ? "#1a2240" : "#161823"
                        border.width: mediaCard.selected ? 1.5 : 1
                        border.color: mediaCard.selected ? "#6c5ce7" : "#262a38"
                        Behavior on border.color { ColorAnimation { duration: 100 } }
                        Behavior on color { ColorAnimation { duration: 100 } }

                        // The still — the engine's real first frame for
                        // video, or the image itself (MediaThumbnailProvider,
                        // same image://mediathumb URL MediaTile.qml uses for
                        // the main Media grid). Never requested for audio —
                        // there is nothing to decode.
                        Image {
                            id: still
                            anchors.fill: parent
                            visible: mediaCard.hasPicture
                            asynchronous: true
                            cache: true
                            fillMode: Image.PreserveAspectCrop
                            source: mediaCard.isAudio ? ""
                                : "image://mediathumb/250/-1/" + encodeURIComponent(mediaCard.modelData.path || "")
                        }

                        // Placeholder icon — the only visual for audio (it
                        // has no frame at all), or video/image while the
                        // engine is still decoding / has no decoder for it.
                        IconGlyph {
                            anchors.centerIn: parent
                            visible: !mediaCard.hasPicture
                            name: mediaCard.isAudio ? "music" : (mediaCard.isVideo ? "camera" : "layoutTemplate")
                            color: "#5c6475"
                            fit: true
                            width: 16; height: 16
                            scale: 2

                            // Breathes while the engine is still making the
                            // picture, so a loading tile doesn't read as "no
                            // picture" — same convention as MediaTile.qml.
                            SequentialAnimation on opacity {
                                running: !mediaCard.isAudio && still.status === Image.Loading
                                loops: Animation.Infinite
                                NumberAnimation { from: 1.0; to: 0.3; duration: 700; easing.type: Easing.InOutSine }
                                NumberAnimation { from: 0.3; to: 1.0; duration: 700; easing.type: Easing.InOutSine }
                            }
                        }

                        // Play badge — marks a video even when its still IS
                        // showing (a poster frame alone looks like a photo).
                        Rectangle {
                            visible: mediaCard.isVideo
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 6
                            width: 24; height: 24
                            radius: 12
                            color: "#b0000000"

                            Text {
                                anchors.centerIn: parent
                                anchors.horizontalCenterOffset: 1
                                text: "▶"
                                color: "#ffffff"
                                font.pixelSize: 11
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 28
                            color: mediaCard.hasPicture ? "#a0161823" : "transparent"

                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 12
                                horizontalAlignment: Text.AlignHCenter
                                text: mediaCard.modelData.name
                                color: "#c8cdd9"
                                font.family: "Segoe UI"
                                font.pixelSize: 12
                                elide: Text.ElideMiddle
                            }
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

            // Footer — Item, not a Row, so "Browse this PC..." (left) and
            // Cancel/Insert (right) can sit on opposite edges of the same
            // band (Row/Column forbid their own children's edge anchors).
            Item {
                width: parent.width
                height: 36

                // The Media Library only covers folders you've added — a
                // file that isn't indexed yet still needs a way in, so this
                // opens the native picker instead (browseRequested; the
                // consumer resolves it — see AudioVideoScreen.qml's
                // settingsMediaPicker for the Settings reuse).
                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Browse this PC…")
                    color: browseArea.containsMouse ? "#9b8ff5" : "#8a94a6"
                    font.family: "Segoe UI"
                    font.pixelSize: 14
                    Behavior on color { ColorAnimation { duration: 100 } }

                    MouseArea {
                        id: browseArea
                        anchors.fill: parent
                        anchors.margins: -6
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.browseRequested()
                    }
                }

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
}
