import QtQuick
import VGRPresenterUI

// One row in the Edit screen's slide list. Pulled out of EditScreen.qml's
// Repeater so the CRUD-row presentation (selection, hover, delete) is a
// self-contained, reusable unit instead of an inline anonymous delegate —
// it only knows about its own props/signals, not the model.
//
// The thumbnail is a LIVE render of the slide, in the same 16:9 proportions as the stage (like FreeShow's slide
// thumbnails): `previewItems` are the slide's item objects straight from SlideCanvasStore - the very objects the canvas
// edits - handed to the SAME DesignPreview the overlay/template cards use, which draws every kind the way the canvas does
// (text with its weight / case / list / alignment / fitting, shapes with their fill and border, clocks and timers ticking,
// camera / media tiles, the slide's background or the transparency checkerboard). Because the objects are live, moving,
// typing or restyling on the canvas re-renders the row the moment it happens; there is no second, simplified drawing of
// the slide to keep in step with the real one. The model's own title/line1/line2/ref roles turned out to be dead data,
// so this component never took them.
//
// Literal colors, not Theme.* — same AOT-compiler limitation as
// AppMenuBar.qml at this nesting depth.
Rectangle {
    id: root

    required property int index
    required property int num
    required property bool active
    required property string tag
    required property string tagColor
    // The slide's STABLE id (SlideListModel's IdRole) — thumbnails key the
    // per-slide store on this, never on `num`, which renumbers.
    required property int slideId
    // The slide's CanvasItem objects — bound live, see the header note. May
    // be empty (a brand-new slide).
    property var previewItems: []
    // The slide's background colour (SlideCanvasStore.backgroundOf) - transparent shows the checkerboard, as on the canvas.
    property var previewBackground: "transparent"

    // Only a slide with a clock or a timer needs a ticking `now`; the rest never wake a timer.
    readonly property bool hasLiveContent: (root.previewItems || []).some((it) => it.kind === "clock" || it.kind === "timer")
    LiveClock { id: ticker; running: root.hasLiveContent }

    signal selected()
    signal duplicateRequested()
    signal deleteRequested()
    // (x, y) are in this item's own local coordinate space — the consumer
    // maps them into whatever coordinate space its context-menu popup
    // needs (see EditScreen.qml, which escapes the slide list's Flickable
    // clip via the shared DropdownPanel.openAt helper).
    signal contextMenuRequested(real x, real y)

    // Also true while hovering the delete button itself: it sits on top of
    // hoverArea, and Qt Quick only delivers hover to the topmost MouseArea
    // at a given point, so hoverArea.containsMouse alone would flip false
    // (and the button fade out) the instant the cursor reached it.
    readonly property bool hovered: hoverArea.containsMouse || deleteArea.containsMouse

    // The thumbnail keeps the stage's proportions (754 x 428), so the row is as tall as the picture needs.
    height: canvasThumb.height + 8
    width: 256
    border.color: root.active ? "#6c5ce7" : "#232530"
    border.width: root.active ? 1.2 : 1
    color: "#161823"
    radius: 9

    Rectangle {
        visible: root.active
        height: parent.height
        width: 3
        radius: 2
        color: "#6c5ce7"
    }

    Rectangle {
        id: canvasThumb
        x: 16
        y: 4
        width: 224
        height: Math.round((width - 2) * 428 / 754) + 2
        border.color: "#262a38"
        border.width: 1
        color: "#0d0f16"
        radius: 4

        DesignPreview {
            x: 1
            y: 1
            width: canvasThumb.width - 2
            blocks: root.previewItems || []
            background: root.previewBackground
            now: ticker.now
            checkerSize: 32
        }

        // A slide with nothing on it yet (over the background, so it reads on a colour or the checkerboard alike).
        Rectangle {
            visible: (root.previewItems || []).length === 0
            anchors.centerIn: parent
            width: emptyLabel.width + 14; height: emptyLabel.height + 6
            radius: 3
            color: "#cc0d0f16"
            Text {
                id: emptyLabel
                anchors.centerIn: parent
                color: "#8a91a3"
                font.family: "Segoe UI"
                font.pixelSize: 10
                text: qsTr("Empty slide")
            }
        }
    }

    Rectangle {
        x: 218
        y: 8
        height: 18
        width: 18
        color: "#1c2030"
        radius: 4

        Text {
            anchors.centerIn: parent
            color: "#c9cedd"
            font.family: "Segoe UI"
            font.pixelSize: 9
            text: String(root.num)
        }
    }

    // Full-row select/duplicate area — declared before the delete button so
    // the button's own MouseArea stacks on top and isn't swallowed by this.
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                root.contextMenuRequested(mouse.x, mouse.y)
            else
                root.selected()
        }
        onDoubleClicked: root.duplicateRequested()
    }

    // Hover-revealed delete button. A single click is too easy to trigger
    // by accident for a destructive action, so it stays a deliberate
    // button rather than living on the same click/double-click gesture as
    // select/duplicate.
    Rectangle {
        x: 196
        y: 8
        height: 18
        width: 18
        radius: 4
        opacity: root.hovered ? 1 : 0
        visible: opacity > 0
        color: deleteArea.containsMouse ? "#3a2230" : "#1c2030"
        Behavior on opacity { NumberAnimation { duration: 100 } }
        Behavior on color { ColorAnimation { duration: 100 } }

        Text {
            anchors.centerIn: parent
            color: "#ff6b61"
            font.pixelSize: 12
            text: "✕"
        }

        MouseArea {
            id: deleteArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.deleteRequested()
        }
    }
}
