import QtQuick
import QtQuick.Controls
import VGRPresenterUI

Item {
    id: root

    property bool open: false

    // ---- the shared shell's own open/close --------------------------------
    onOpenChanged: {
        if (root.open) {
            root.selectedId = ""
            card.open()
        } else {
            card.close()
        }
    }

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

    property string fixedKind: ""

    // "all" | "image" | "video" | "audio" — the tabs' own plural labels are UI
    // copy for the singular `kind` values items are tagged with.
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

    // Fired when "Insert" is clicked (or a card is double-clicked), carrying
    // the picked media item.
    signal applied(var item)
    signal cancelled()
    // "Browse this PC..." clicked — the consumer resolves it (native picker);
    // this component has no PAL access of its own.
    signal browseRequested()

    ModalCard {
        id: card
        title: qsTr("Select Media")
        subtitle: root.fixedKind === "audio" ? qsTr("Choose an audio file")
                : root.fixedKind === "video" ? qsTr("Choose a video file")
                : root.fixedKind === "image" ? qsTr("Choose a picture")
                : qsTr("Choose a picture, video or audio file")
        cardWidth: 900
        clickOutsideToDismiss: true
        showSave: root.selectedItem !== null
        saveText: qsTr("Insert")
        cancelText: qsTr("Cancel")
        onCancelled: root.cancelled()
        onAccepted: {
            if (root.selectedItem !== null)
                root.applied(root.selectedItem)
        }

        // Tabs (left) + search (right), same row like the ground truth.
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
                        color: tabBtn.active ? Qt.alpha(Theme.accent, 0.16)
                             : (tabArea.containsMouse ? Theme.hoverBg : "transparent")
                        border.color: tabBtn.active ? Theme.accent : "transparent"
                        border.width: 1
                        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                        Text {
                            id: tabLabel
                            anchors.centerIn: parent
                            text: tabBtn.modelData.label
                            color: tabBtn.active ? Theme.accentLight : Theme.textSecondary
                            font.family: Theme.fontFamily
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
                color: Theme.inset
                border.color: Theme.border
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
                        font.pixelSize: 13
                        selectByMouse: true
                        onTextChanged: root.searchQuery = text

                        Text {
                            visible: parent.text.length === 0
                            text: qsTr("Search media...")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: 13
                        }
                    }
                }
            }
        }

        Grid {
            id: mediaGrid
            readonly property int gap: 14
            width: parent.width
            columns: 4
            columnSpacing: mediaGrid.gap
            rowSpacing: mediaGrid.gap

            Repeater {
                model: root.filteredItems
                delegate: Rectangle {
                    id: mediaCard
                    required property var modelData
                    readonly property bool selected: root.selectedId === mediaCard.modelData.id
                    readonly property bool hovered: cardHover.containsMouse
                    readonly property bool isVideo: mediaCard.modelData.kind === "video"
                    readonly property bool isAudio: mediaCard.modelData.kind === "audio"
                    readonly property bool hasPicture: !mediaCard.isAudio
                        && still.status === Image.Ready && still.sourceSize.width > 1

                    width: Math.floor((mediaGrid.width - mediaGrid.gap * (mediaGrid.columns - 1)) / mediaGrid.columns)
                    height: 118
                    radius: 8
                    clip: true
                    color: mediaCard.selected ? Qt.alpha(Theme.accent, 0.15)
                         : (mediaCard.hovered ? Theme.rowBg : Theme.inset)
                    border.width: mediaCard.selected ? 1.5 : 1
                    border.color: mediaCard.selected ? Theme.accent
                                : (mediaCard.hovered ? Qt.alpha(Theme.accent, 0.45) : Theme.borderSubtle)
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                    Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

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

                    IconGlyph {
                        anchors.centerIn: parent
                        visible: !mediaCard.hasPicture
                        name: mediaCard.isAudio ? "music" : (mediaCard.isVideo ? "camera" : "layoutTemplate")
                        color: Theme.textMuted
                        fit: true
                        width: 16; height: 16
                        scale: 2

                        SequentialAnimation on opacity {
                            running: !mediaCard.isAudio && still.status === Image.Loading
                            loops: Animation.Infinite
                            NumberAnimation { from: 1.0; to: 0.3; duration: 700; easing.type: Easing.InOutSine }
                            NumberAnimation { from: 0.3; to: 1.0; duration: 700; easing.type: Easing.InOutSine }
                        }
                    }

                    // Play badge — marks a video even when its still IS showing
                    // (a poster frame alone looks like a photo).
                    Rectangle {
                        visible: mediaCard.isVideo
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 6
                        width: 24; height: 24
                        radius: 12
                        color: "#b0000000"

                        IconGlyph {
                            anchors.centerIn: parent
                            name: "play"
                            color: "#ffffff"
                            width: 11; height: 11
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
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }
                    }

                    // Click selects; double-click is the whole gesture (pick AND
                    // insert) for a file the operator is sure about.
                    MouseArea {
                        id: cardHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.selectedId = mediaCard.modelData.id
                        onDoubleClicked: {
                            root.selectedId = mediaCard.modelData.id
                            root.applied(mediaCard.modelData)
                        }
                    }
                }
            }
        }

        // Nothing to show yet (an empty library, a fresh install, or a search
        // that matched nothing) — said in words rather than as a blank card.
        Text {
            visible: root.filteredItems.length === 0
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.items.length === 0
                ? qsTr("No media yet — add a folder below, or copy files into one you have added.")
                : qsTr("Nothing here matches that search.")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: 13
            wrapMode: Text.WordWrap
        }

        Text {
            text: qsTr("Browse this PC…")
            color: browseArea.containsMouse ? Theme.accentLight : Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: 14
            Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

            MouseArea {
                id: browseArea
                anchors.fill: parent
                anchors.margins: -6
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.browseRequested()
            }
        }
    }
}