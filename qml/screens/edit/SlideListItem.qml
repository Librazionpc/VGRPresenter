import QtQuick

// One row in the Edit screen's slide list. Pulled out of EditScreen.qml's
// Repeater so the CRUD-row presentation (selection, hover, delete) is a
// self-contained, reusable unit instead of an inline anonymous delegate —
// it only knows about its own props/signals, not the model.
//
// The thumbnail is a LIVE mini-canvas: `previewItems` are the slide's item
// objects straight from SlideCanvasStore (the same objects the canvas
// edits), each drawn at its real x/y/width/height — scaled to fit — with
// its own fill, border and radius, so moving/resizing/restyling on the
// canvas re-renders the row the moment it happens. No hardcoded previews,
// no JS shadow of the model. The model's own title/line1/line2/ref roles
// turned out to be dead data (only a removed preview indirection ever read
// them), so this component never took them.
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

    // The canvas's design size — items' x/y/width/height live in this space;
    // the mini-canvas scales them to fit the thumbnail. The consumer binds
    // these to the actual canvas item so the mapping can never drift.
    property real canvasWidth: 754
    property real canvasHeight: 428

    // { kind, icon, label } entries — the SAME array EditScreen.qml's "+"
    // Add Content menu and canvas placeholder use (passed straight through,
    // not duplicated), so a non-text item's thumbnail shows its icon
    // instead of rendering as an unlabeled blank tile.
    property var contentTypes: []
    function typeInfoFor(kind) {
        for (let i = 0; i < root.contentTypes.length; ++i) {
            if (root.contentTypes[i].kind === kind)
                return root.contentTypes[i]
        }
        return { icon: "?", label: kind }
    }

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

    height: 124
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
        height: 108
        width: 224
        border.color: "#262a38"
        border.width: 1
        color: "#0d0f16"
        radius: 8
        clip: true

        // Scale every item's real geometry into the thumbnail: fit the whole
        // canvas inside, centered. All bindings read the LIVE item objects,
        // so moving/resizing/restyling on the canvas updates this row in
        // real time — same binding chain the text preview used, one visual
        // level closer to the actual slide.
        readonly property real fit: Math.min((width - 8) / root.canvasWidth,
                                             (height - 8) / root.canvasHeight)
        readonly property real offX: (width - root.canvasWidth * fit) / 2
        readonly property real offY: (height - root.canvasHeight * fit) / 2

        Repeater {
            model: root.previewItems || []
            delegate: Rectangle {
                id: miniItem
                required property var modelData
                readonly property var st: modelData.style
                // text always shows its (possibly empty) text; camera/media
                // show the source/file name picked in their "+" menu popup
                // once one's actually been set — same live value the canvas
                // itself shows, not just a kind icon.
                readonly property bool showValueText: miniItem.modelData.kind === "text"
                    || (["camera", "media"].includes(miniItem.modelData.kind) && miniItem.modelData.text.length > 0)
                readonly property bool showLiveValue: ["clock", "timer"].includes(miniItem.modelData.kind)
                readonly property bool showIcon: !miniItem.showValueText && !miniItem.showLiveValue
                    && miniItem.modelData.kind !== "shape"

                x: canvasThumb.offX + modelData.x * canvasThumb.fit
                y: canvasThumb.offY + modelData.y * canvasThumb.fit
                width: Math.max(2, modelData.width * canvasThumb.fit)
                height: Math.max(2, modelData.height * canvasThumb.fit)
                radius: st ? Math.min(st.cornerRadius * canvasThumb.fit, width / 2) : 0
                // A transparent fill renders as a neutral tile so the item's
                // footprint stays legible in the thumbnail; a picked color
                // (or an enabled border) shows as-is.
                // Alpha test, not !== "transparent" — a QML color holding
                // "transparent" reads back as #00000000, so the string
                // comparison never matched (see EditScreen.qml's
                // shapeContent.hasFill for the full note).
                color: st && st.backgroundColor.a > 0
                       ? st.backgroundColor : "#1e2130"
                border.width: st && st.borderEnabled ? Math.max(1, st.borderWidth * canvasThumb.fit) : 1
                border.color: st && st.borderEnabled ? st.borderColor : "#343a4e"

                Text {
                    visible: miniItem.showValueText
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.family: "Inter"
                    // Same size ratio as the canvas label (16px at full
                    // scale), floored so it never vanishes entirely.
                    font.pixelSize: Math.max(3, 16 * canvasThumb.fit)
                    color: "#f2f4fa"
                    elide: Text.ElideRight
                    text: miniItem.modelData.text
                }

                // Live-ticking value for clock/timer kinds — same
                // LiveClock.qml shared component and math the canvas visual
                // itself uses (see EditScreen.qml's clockContent/
                // timerContent), so the thumbnail shows an actually-live
                // clock/countdown instead of a frozen or generic icon.
                LiveClock {
                    id: miniTicker
                    running: miniItem.showLiveValue
                }

                Text {
                    visible: miniItem.modelData.kind === "clock"
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.family: "Inter"
                    font.pixelSize: Math.max(3, 11 * canvasThumb.fit)
                    color: "#9b8ff5"
                    elide: Text.ElideRight
                    text: miniTicker.formatClock(miniTicker.now,
                        miniItem.modelData.meta.format !== "24",
                        miniItem.modelData.meta.showSeconds !== false)
                }

                Text {
                    visible: miniItem.modelData.kind === "timer"
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.family: "Inter"
                    font.pixelSize: Math.max(3, 11 * canvasThumb.fit)
                    color: "#9b8ff5"
                    elide: Text.ElideRight
                    text: miniTicker.formatDuration(miniTicker.timerSeconds(miniTicker.now,
                        miniItem.modelData.meta.mode ?? "countdown",
                        miniItem.modelData.meta.durationSeconds ?? 300,
                        miniItem.modelData.meta.startedAt ?? Date.now()))
                }

                // Every remaining kind with no live value of its own
                // (audio, or camera/media before a source is picked) — its
                // icon glyph, same as the canvas's own generic placeholder,
                // so it still reads as identifiable content instead of a
                // blank tile. "shape" is excluded: its own style-driven
                // fill/border above (the same one the canvas itself uses)
                // already reads as real content without an icon on top.
                Text {
                    visible: miniItem.showIcon
                    anchors.centerIn: parent
                    color: "#9b8ff5"
                    font.pixelSize: Math.max(6, Math.min(miniItem.width, miniItem.height) * 0.4)
                    text: root.typeInfoFor(miniItem.modelData.kind).icon
                }
            }
        }

        Text {
            visible: (root.previewItems || []).length === 0
            anchors.centerIn: parent
            color: "#4a4f60"
            font.family: "Inter"
            font.pixelSize: 9
            text: qsTr("Empty slide")
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
            font.family: "Inter"
            font.pixelSize: 8
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
            font.pixelSize: 10
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
