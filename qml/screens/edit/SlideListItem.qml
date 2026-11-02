import QtQuick
import VGRPresenterUI

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
    signal contextMenuRequested(real x, real y)

    readonly property bool hovered: hoverArea.containsMouse || deleteArea.containsMouse

    // The thumbnail keeps the stage's proportions (StageGrid.w x StageGrid.h), so the row is as tall as the picture needs.
    height: canvasThumb.height + 8
    width: 256
    border.color: root.active ? Theme.accent : Theme.border
    border.width: root.active ? 1.2 : 1
    color: Theme.surface
    radius: 9

    Rectangle {
        visible: root.active
        height: parent.height
        width: 3
        radius: 2
        color: Theme.accent
    }

    Rectangle {
        id: canvasThumb
        x: 16
        y: 4
        width: Math.max(96, parent.width - 32)
        height: StageGrid.heightForWidth(width - 2) + 2
        border.color: Theme.borderSubtle
        border.width: 1
        color: Theme.windowBg
        radius: 4

        DesignPreview {
            x: 1
            y: 1
            width: canvasThumb.width - 2
            blocks: root.previewItems || []
            background: root.previewBackground
            now: ticker.now
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
                color: Theme.textMuted
                font.family: Theme.fontFamily
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
            color: Theme.navLabelMuted
            font.family: Theme.fontFamily
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
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

        Text {
            anchors.centerIn: parent
            color: Theme.dangerLight
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