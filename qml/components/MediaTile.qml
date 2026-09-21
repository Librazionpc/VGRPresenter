import QtQuick
import VGRPresenterUI

// One image or video in the Media grid: a 16:9 preview with its name under it.
//
// The pictures come from the ENGINE's thumbnail cache (image://mediathumb, served by
// MediaThumbnailProvider) - this file only shows them, the way FreeShow's media card does:
//   STILL  - the poster: an image is itself, a video is its middle frame.
//   MOVING - hover a video and move the mouse across the tile: it scrubs through the video's
//            MediaLibraryService.frameSteps frames (FreeShow's hover-scrub, steps = 10), the
//            frame under the pointer replacing the poster. The engine makes each frame the
//            first time it is asked for and caches it, so scrubbing a video a second time is
//            instant. A thin bar at the foot of the preview shows how far along you are.
// When there is no picture (not decoded yet, or the engine has no decoder for that file) the
// tile shows an icon placeholder instead; a play badge marks a video either way.
Item {
    id: root

    property string name: ""
    property string path: ""
    property string kind: "image"   // "image" | "video" | "audio"

    readonly property bool isVideo: kind === "video"
    readonly property bool isAudio: kind === "audio"
    // The glyph that stands for this kind of file (24-grid glyphs are fit to their box; the image one is hand-sized).
    readonly property string kindIcon: isVideo ? "camera" : (isAudio ? "music" : "layoutTemplate")
    readonly property bool gridIcon: isVideo || isAudio
    readonly property real previewHeight: Math.round((width - 12) * 9 / 16)

    // Click: the file on the centre page. Double-click: into the open project. Drag: into a project.
    signal activated()
    signal opened()

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
        payload: ({ kind: "media", items: [{ ref: root.path, name: root.name }] })
        label: root.name
        onActivated: root.activated()
        onOpened: root.opened()
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
                color: "#6c5ce7"
            }

            // Marks a video or an audio file (the picture alone looks like a photo).
            Rectangle {
                visible: (root.isVideo && !root.scrubbing) || root.isAudio
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
                    color: "#ffffff"
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
                font.pixelSize: 11
                textFormat: Text.PlainText
            }
        }
    }

    // Hover feedback only (the pointing hand comes from the DragSource underneath).
    PositionHoverArea {
        id: hover
        anchors.fill: parent
        showCursor: false
    }
}
