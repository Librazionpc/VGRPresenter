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
    // THIS FILE'S OWN FILL ("" = the output's default): a choice made on the
    // picture itself (the tile menu's Fill row), which wins over every output's
    // Fill setting when it is on air — so the grid has to show it holds one, or
    // a picture that crops everywhere else reads as a bug on the other tiles.
    property string fit: ""

    readonly property bool isVideo: kind === "video"
    readonly property bool isAudio: kind === "audio"
    // The glyph that stands for this kind of file (24-grid glyphs are fit to their box; the image one is hand-sized).
    readonly property string kindIcon: isVideo ? "camera" : (isAudio ? "music" : "layoutTemplate")
    readonly property bool gridIcon: isVideo || isAudio
    readonly property real previewHeight: Math.round((width - 12) * 9 / 16)
    // On air? This exact file is on the program mix (video/image: the
    // monitor wall's compositor layer; audio: the output tile's L/R
    // meters are the only visible confirmation — it has no frame).
    readonly property bool onAir: LiveOutputService.mediaOnAir
                                  && LiveOutputService.mediaPath === root.path
    // PICKED (the grid's multi-selection). The tile draws its own ring for it, so a
    // selection reads on the picture itself rather than only in a count at the foot
    // of the pane.
    property bool selected: false
    // What THIS tile's drag carries: one file normally, the whole selection when the
    // tile is part of one (the pane builds it — see dragItemsFor). A list of
    // { ref, name }, the same shape every drop target in the app already takes.
    property var dragItems: []

    // Click: the file on the centre page AND on the output (a take). Double-click: into the
    // open project (and still on the output — a take never toggles). Drag: into a project.
    // The click carries its KEYBOARD MODIFIERS — Ctrl toggles this tile's pick, Shift
    // extends it from the last plain click (the pane decides; the tile only reports).
    signal activated(int modifiers)
    signal opened()
    // Right-click: the host builds and shows the item menu (rename / move / delete).
    // The same signature the design cards and sidebar rows report, so one host-side
    // menu pattern serves every library — the tile itself touches no file.
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

    // The RIGHT button only, declared over the DragSource: the left one (click,
    // double-click, drag onto a project) stays the DragSource's, and a button this
    // area does not accept is passed straight down to it.
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
        // THE POINTER'S EDGE — the app's shared hover layer, declared LAST so
        // it is drawn over the preview (a border on the card itself would sit
        // under every child and show only along the name strip). Its wash is
        // transparent: a tile's picture must not be tinted, only edged.
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
        // Geometry alone decides "is this tile on screen" — the same posture
        // DesignCard and the library rows take. Walking every ancestor's
        // visible/enabled made `shownChain` depend on a chain that itself reads
        // a hover-driven property inside the grid, which the binding loop
        // detector caught (MediaTile.qml: QML PositionHoverArea: Binding loop
        // detected for property "shownChain"). The open-popup case is still
        // covered by AppCursor.blocked() below.
        checkAncestors: false
        // HOVER ONLY — NO CLICKS. This area is the tile's LAST child, so it is drawn
        // over everything: with its click-sinking MouseArea enabled it swallowed
        // every press before the DragSource beneath it ever saw one. That is why a
        // click on a tile did
        // nothing to the output and a double-click never opened it — the gestures
        // were being delivered to a hover tracker. DesignCard's own hover layer sits
        // UNDER its DragSource for the same reason.
        clickEnabled: false
    }
}
