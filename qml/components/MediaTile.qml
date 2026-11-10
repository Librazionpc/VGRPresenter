import QtQuick
import VGRPresenterUI

Item {
    id: root

    property string name: ""
    property string path: ""
    property string kind: "image"   // "image" | "video" | "audio"
    property string fit: ""

    readonly property bool isVideo: kind === "video"
    readonly property bool isAudio: kind === "audio"
    // The glyph that stands for this kind of file (24-grid glyphs are fit to their box; the image one is hand-sized).
    readonly property string kindIcon: isVideo ? "camera" : (isAudio ? "music" : "layoutTemplate")
    readonly property bool gridIcon: isVideo || isAudio
    readonly property real previewHeight: Math.round((width - 12) * 9 / 16)
    readonly property bool onAir: LiveOutputService.mediaOnAir
                                  && LiveOutputService.mediaPath === root.path
    property bool selected: false
    property var dragItems: []

    signal activated(int modifiers)
    signal opened()
    signal contextMenuRequested(var source, real mx, real my)

    // The size to ask the engine for (it rounds up to a cache size).
    readonly property int pictureSize: width > 300 ? 500 : 250
    // -1 = the still; 0..frameSteps-1 = the moving frame under the pointer.
    property int frameStep: -1
    readonly property int steps: MediaLibraryService.frameSteps
    readonly property bool scrubbing: isVideo && hover.hovered && frameStep >= 0
    // The provider answers a picture it cannot make with a 1x1 transparent image (a null one would make
    // Image log a warning per tile) - that is "no picture": show the placeholder.
    readonly property bool hasPicture: still.status === Image.Ready && still.sourceSize.width > 1

    function pictureUrl(step) {
        return "image://mediathumb/" + root.pictureSize + "/" + step + "/" + encodeURIComponent(root.path)
    }

    // Off the tile, back to the poster.
    Connections {
        target: hover
        function onHoveredChanged() { if (!hover.hovered) root.frameStep = -1 }
    }

    // Underneath the card: the preview's scrub area above it takes no buttons, so a press falls through to here.
    DragSource {
        anchors.fill: parent
        payload: ({ kind: "media",
                    items: (root.dragItems && root.dragItems.length > 0)
                           ? root.dragItems
                           : [{ ref: root.path, name: root.name }] })
        label: (root.dragItems && root.dragItems.length > 1)
               ? qsTr("%1 files").arg(root.dragItems.length)
               : root.name
        onActivated: (modifiers) => root.activated(modifiers)
        onOpened: root.opened()
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: (mouse) => root.contextMenuRequested(root, mouse.x, mouse.y)
    }

    Rectangle {
        id: card
        anchors.fill: parent
        anchors.margins: 6
        radius: 6
        clip: true
        color: Theme.rowBg
        // The monitor wall indicates the on-air state; keep the library tile
        // free of an accent ring when the file is taken.
        border.width: 0

        // ---- preview ----
        Rectangle {
            id: preview
            width: parent.width
            height: root.previewHeight
            color: "#1c1d26"

            IconGlyph {
                anchors.centerIn: parent
                visible: !root.hasPicture
                name: root.kindIcon
                color: Theme.textMuted
                // The camera / music note are 24-grid glyphs (fit to 16); the image glyph is hand-sized at 9 x 9.
                fit: true
                width: root.gridIcon ? 16 : 9
                height: width
                scale: root.gridIcon ? 2 : 3.5

                // Breathes while the engine is still making the picture, so a tile that is loading does
                // not look like one that has no picture.
                SequentialAnimation on opacity {
                    running: still.status === Image.Loading
                    loops: Animation.Infinite
                    NumberAnimation { from: 1.0; to: 0.3; duration: 700; easing.type: Easing.InOutSine }
                    NumberAnimation { from: 0.3; to: 1.0; duration: 700; easing.type: Easing.InOutSine }
                }
            }

            // The poster.
            Image {
                id: still
                anchors.fill: parent
                asynchronous: true
                cache: true
                fillMode: Image.PreserveAspectCrop
                source: root.pictureUrl(-1)
            }

            // THE FILE ON AIR, DRAWN THE WAY THE MONITOR TILES DRAW IT: the one shared surface, fed by the
            // player the take started — never a thumbnail and never a second decode of the same file. So a
            // card that is playing shows the moving picture here (the poster stays underneath for the
            // moment before the first frame arrives).
            LiveMediaSurface {
                anchors.fill: parent
                active: root.onAir && !LiveOutputService.mediaIsAudio
                fit: root.fit !== "" ? root.fit : LiveOutputService.mediaFit
                sizeToItem: true
            }

            // The moving frame under the pointer, over the poster. It keeps showing the previous
            // frame while the next one loads, so scrubbing does not flicker to the poster.
            Image {
                id: frame
                anchors.fill: parent
                visible: root.scrubbing && frame.status === Image.Ready && frame.sourceSize.width > 1
                asynchronous: true
                cache: true
                retainWhileLoading: true
                fillMode: Image.PreserveAspectCrop
                source: root.scrubbing ? root.pictureUrl(root.frameStep) : ""
            }

            // Mouse position -> frame step (FreeShow: the tile is cut into `steps` bands).
            MouseArea {
                anchors.fill: parent
                enabled: root.isVideo
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
                onPositionChanged: (mouse) => {
                    if (root.steps > 0)
                        root.frameStep = Math.max(0, Math.min(root.steps - 1, Math.floor(mouse.x / width * root.steps)))
                }
            }

            // Scrub position.
            Rectangle {
                visible: root.scrubbing
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                height: 3
                width: parent.width * (root.frameStep + 1) / Math.max(1, root.steps)
                color: Theme.accent
            }

            // THIS FILE'S OWN FILL. Top-left, away from the play/music badge, so
            // the two never collide on a video tile.
            Pill {
                visible: root.fit !== ""
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 6
                text: root.fit === "cover" ? qsTr("Cover")
                      : root.fit === "fill" ? qsTr("Stretch")
                                             : qsTr("Contain")
                tint: false
                fontSize: Theme.textXs - 1
            }

            // Marks a video or an audio file (the picture alone looks like a photo).
            Rectangle {
                visible: (root.isVideo && !root.scrubbing && !hover.hovered) || root.isAudio
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 6
                width: 26; height: 26; radius: 13
                color: "#b0000000"
                IconGlyph {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.horizontalCenterOffset: root.isAudio ? 0 : 1   // a triangle looks centred a little right of its box centre
                    name: root.isAudio ? "music" : "play"
                    color: Theme.controlInk
                    fit: true
                    width: 12; height: 12
                }
            }
        }

        // ---- name ----
        Row {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: preview.bottom
            anchors.bottom: parent.bottom
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 6
            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -1   // on the text's letter height, not its line box
                name: root.kindIcon
                color: Theme.textSecondary
                fit: true
                width: root.gridIcon ? 14 : 9    // a hand-sized glyph is centred only in a box of its own size
                height: width
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 20
                text: root.name
                color: Theme.textPrimary
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 13
                textFormat: Text.PlainText
            }
        }
        HoverGlow {
            anchors.fill: parent
            radius: parent.radius
            hovered: hover.hovered && !root.onAir
            glowColor: "transparent"
            edge: true
            edgeColor: Theme.border
        }
        // PICKED — OVER the hover edge, and a step thicker, so "this one is part of
        // what Delete would act on" cannot be read as "the pointer is here".
        Rectangle {
            visible: root.selected
            anchors.fill: parent
            anchors.margins: -2
            radius: card.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
        }
    }

    // Hover feedback only (the pointing hand comes from the DragSource underneath).
    PositionHoverArea {
        id: hover
        anchors.fill: parent
        showCursor: false
        checkAncestors: false
        clickEnabled: false
    }
}