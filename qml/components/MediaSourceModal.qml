import QtQuick
import QtQuick.Controls
import VGRPresenterUI

// "Select Media" picker — the app's ONE media chooser. Opened from the Edit
// screen's + menu (Media → Picture / Video / Audio) and reused by Settings ·
// Audio & Video's Media-kind "Browse..." option (an audio/video input's Source
// field), restricted to one kind there via `fixedKind`.
//
// BUILT ON THE SHARED MODAL SHELL (ModalCard.qml) rather than its own scrim and
// card, which is what every dialog in this app uses: the modal shell is where
// the dim backdrop, the centered card, the pinned Cancel/Save footer, the
// scrollable content slot and the AppScrollBar live. This dialog used to draw
// its own chrome around a plain `Grid` — no scroll area and no scroll bar — so a
// media folder with more than eight files simply ran off the bottom of the card
// with no way to reach the rest. The shared shell caps the content against the
// window, scrolls it, and the bar appears on its own when there is more than
// fits (AppScrollBar hides itself otherwise).
//
// Cards carry the app's own hover language too: the tile under the pointer
// lifts and takes the accent border, so the pick reads as "this one" before the
// click, and a double-click inserts without reaching for the footer.
//
// `items` defaults to the real Media Library (MediaLibraryService.items()),
// each { id, name, kind, path } — kind is "image" | "video" | "audio" (a video
// card gets the small play-badge overlay, audio a note badge). A consumer that
// wants a different set can still override `items` directly.
Item {
    id: root

    property bool open: false

    // ---- the shared shell's own open/close --------------------------------
    // `open` stays the public property callers bind to (the shell's is a
    // function pair, and every use site here already speaks `open:`), so this is
    // the one translation: opening resets the pick and raises the card, closing
    // fades it out and releases its stacking slot.
    onOpenChanged: {
        if (root.open) {
            // Reset the pick whenever the modal (re)opens on a fresh item set —
            // an open with no items yet (async library scan) must not carry the
            // PREVIOUS session's pick forward as a silently-wrong default.
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

    // Single-kind mode — the Edit screen's three media chips and Settings only
    // ever want ONE kind (an audio input's Source can't offer video files).
    // Non-empty hides the tab row entirely and locks filtering to it.
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
        // A picker asks nothing of what is behind it (no form is being edited),
        // so a click on the dim backdrop is the same gesture as Cancel — the
        // behaviour this dialog always had.
        clickOutsideToDismiss: true
        // NO INSERT WITHOUT A PICK: the footer's Save IS this dialog's Insert,
        // so it is drawn only once there is something to insert — the same
        // reason the old hand-rolled button carried an `enabled` flag, done with
        // the shell's own switch so Enter cannot confirm an empty pick either
        // (ModalCard claims Enter only while a Save exists).
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

        // The media grid. Inside the shell's scrollable content, so a library of
        // hundreds scrolls (with the shell's own AppScrollBar) instead of
        // running off the card.
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
                    // Audio has no visual frame to fetch — never even asks the
                    // engine for one; video/image do, and fall back to the icon
                    // below while it loads or if the engine has no decoder for
                    // that file (the provider answers with a 1x1 transparent
                    // image, matching MediaTile.qml's own hasPicture rule).
                    readonly property bool hasPicture: !mediaCard.isAudio
                        && still.status === Image.Ready && still.sourceSize.width > 1

                    width: Math.floor((mediaGrid.width - mediaGrid.gap * (mediaGrid.columns - 1)) / mediaGrid.columns)
                    height: 118
                    radius: 8
                    clip: true
                    // HOVER + SELECT, the same two states every other grid in the
                    // app draws: the pointer's card lifts on its own (a fill and
                    // an accent border) before any click commits to it.
                    color: mediaCard.selected ? Qt.alpha(Theme.accent, 0.15)
                         : (mediaCard.hovered ? Theme.rowBg : Theme.inset)
                    border.width: mediaCard.selected ? 1.5 : 1
                    border.color: mediaCard.selected ? Theme.accent
                                : (mediaCard.hovered ? Qt.alpha(Theme.accent, 0.45) : Theme.borderSubtle)
                    Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }
                    Behavior on border.color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                    // The still — the engine's real first frame for video, or the
                    // image itself (MediaThumbnailProvider, the same
                    // image://mediathumb URL MediaTile.qml uses for the Media
                    // grid). Never requested for audio — nothing to decode.
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

                    // Placeholder icon — the only visual for audio (it has no
                    // frame at all), or video/image while the engine is still
                    // decoding / has no decoder for it.
                    IconGlyph {
                        anchors.centerIn: parent
                        visible: !mediaCard.hasPicture
                        name: mediaCard.isAudio ? "music" : (mediaCard.isVideo ? "camera" : "layoutTemplate")
                        color: Theme.textMuted
                        fit: true
                        width: 16; height: 16
                        scale: 2

                        // Breathes while the engine is still making the picture,
                        // so a loading tile doesn't read as "no picture" — same
                        // convention as MediaTile.qml.
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

        // The Media Library only covers folders you've added — a file that isn't
        // indexed yet still needs a way in, so this opens the native picker
        // instead (browseRequested; the consumer resolves it — see
        // AudioVideoScreen.qml's settingsMediaPicker for the Settings reuse).
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
