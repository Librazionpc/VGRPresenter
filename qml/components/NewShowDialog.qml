import QtQuick
import VGRPresenterUI

// "New show" (FreeShow's own New-show popup): a name, a category, and three
// ways to fill it — Quick lyrics (paste text, split into slides by the
// engine's own SongText/ChordPro splitter), Web search (not built yet — CCLI/
// SongSelect lookup is a separate feature), or an Empty show.
//
// A "song" is not a separate content system here — it is a SHOW filed under
// the "song" category, the same category Settings > Styles already lets you
// assign a template to (exactly like Scripture's "scripture" and The Table's
// "table"). Quick lyrics saves straight into the shows library through
// ImportService.importText (bps::import::ShowFromClipboardText) and opens the
// result on the Edit screen — there is no separate song data model to keep
// in sync with it.
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    // The saved show's path — the caller opens it on the Edit screen.
    signal showCreated(string path)
    // "Empty show" — the caller does today's blank-canvas reset, category is
    // informational only until a slide exists to carry it.
    signal emptyShowRequested(string name, string category)

    property bool shown: false
    property string stage: "pick"   // "pick" | "text"
    property string category: "song"

    readonly property var categories: [
        { id: "song", name: qsTr("Songs") },
        { id: "presentation", name: qsTr("Presentation") },
    ]

    function open() {
        root.stage = "pick"
        root.category = "song"
        nameField.text = ""
        lyricsEdit.text = ""
        root.shown = true
        Qt.callLater(nameField.focusInput)
    }
    function close() { root.shown = false }

    anchors.fill: parent
    visible: opacity > 0
    enabled: opacity > 0
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 130 } }

    Keys.onEscapePressed: root.close()

    function createEmpty() {
        const name = nameField.text.trim()
        const cat = root.category
        root.close()
        root.emptyShowRequested(name, cat)
    }

    function createFromLyrics() {
        if (lyricsEdit.text.trim().length === 0)
            return
        const answer = ImportService.importText(lyricsEdit.text, nameField.text.trim(), root.category)
        if (!answer.ok)
            return   // ImportService already toasted the reason
        root.close()
        root.showCreated(answer.firstShow)
    }

    // One of the three creation tiles on the picker stage.
    component OptionTile: Rectangle {
        id: tile
        property string label: ""
        property string icon: ""
        property bool tileEnabled: true
        property string badge: ""
        // Only glyphs on IconGlyph's Lucide 24-grid (see its own `grid24`
        // list) scale via `fit` + implicitWidth/Height — "search" and "plus"
        // are hand-sized, fixed-natural-size glyphs (their Loader centres
        // them, it does not stretch them), so implicitWidth does nothing for
        // them; TabSearchBox.qml's own big search icon is the precedent for
        // the fix: grow them with `scale` instead, and shrink `strokeWidth`
        // by the same factor so the line doesn't get chunky at 5-7x size.
        property bool iconGrid24: true
        // The glyph's own natural size in px (read from IconGlyph.qml's
        // Component defs) — only used when iconGrid24 is false.
        property real naturalIconPx: 24
        signal activated()

        // FreeShow's own new-show tiles (MaterialMultiChoice): a near-square
        // card with an 80px icon (Icon size=5, 1rem=16px) — this card is
        // narrower than FreeShow's popup, so the icon scales down with the
        // tile instead of copying that px figure literally.
        height: width
        radius: Theme.radiusMd
        color: mouse.containsMouse && tile.tileEnabled ? Theme.inset : "transparent"
        border.width: 1
        border.color: Theme.border
        opacity: tile.tileEnabled ? 1 : 0.45

        readonly property real iconTargetPx: tile.width * 0.4

        Column {
            anchors.centerIn: parent
            spacing: 12

            // A FIXED-size box, always iconTargetPx regardless of the icon
            // kind: `scale` (used to grow the non-grid24 icons) is a pure
            // visual transform — it does NOT change what an item reports as
            // its own size to a layout, so without this wrapper the Column
            // reserved only the icon's tiny PRE-scale footprint and the
            // scaled-up glyph visually spilled down over the label below it
            // (the "Web search" magnifying glass overlapping its own text).
            // Every tile's box is identically sized, so every label lands at
            // the same y regardless of which icon kind sits above it.
            Item {
                width: tile.iconTargetPx
                height: tile.iconTargetPx
                anchors.horizontalCenter: parent.horizontalCenter
                IconGlyph {
                    anchors.centerIn: parent
                    name: tile.icon
                    fit: tile.iconGrid24
                    color: Theme.textPrimary
                    implicitWidth: tile.iconGrid24 ? tile.iconTargetPx : tile.naturalIconPx
                    implicitHeight: implicitWidth
                    scale: tile.iconGrid24 ? 1 : tile.iconTargetPx / tile.naturalIconPx
                    strokeWidth: tile.iconGrid24 ? 1.5 : 2.2 / (tile.iconTargetPx / tile.naturalIconPx)
                }
            }
            Text {
                text: tile.label
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textMd
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }

        Rectangle {
            visible: tile.badge !== ""
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 6
            radius: Theme.radiusSm
            color: Theme.inset
            width: badgeText.implicitWidth + 10
            height: 16
            Text {
                id: badgeText
                anchors.centerIn: parent
                text: tile.badge
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 10
            }
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: tile.tileEnabled
            cursorShape: Qt.PointingHandCursor
            onClicked: tile.activated()
        }
    }

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.close()
    }

    Rectangle {
        width: Math.min(560, root.width - 80)
        height: Math.min(root.height - 80, bodyCol.implicitHeight + 2 * Theme.space6)
        anchors.centerIn: parent
        radius: Theme.radiusLg
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        clip: true

        Column {
            id: bodyCol
            x: Theme.space6
            y: Theme.space6
            width: parent.width - 2 * Theme.space6
            spacing: Theme.space5

            Item {
                width: parent.width
                height: Math.max(titleText.implicitHeight, closeIcon.height)

                Text {
                    id: titleText
                    anchors.left: parent.left
                    anchors.right: closeIcon.left
                    anchors.rightMargin: Theme.space3
                    text: qsTr("New show")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXl
                    font.weight: Font.DemiBold
                }
                IconGlyph {
                    id: closeIcon
                    anchors.right: parent.right
                    anchors.top: parent.top
                    name: "close"
                    color: Theme.textMuted
                    implicitWidth: 16
                    implicitHeight: 16
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -8
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }
            }

            // ---- stage: pick ----
            Column {
                width: parent.width
                spacing: Theme.space4
                visible: root.stage === "pick"

                SettingsField {
                    id: nameField
                    width: parent.width
                    label: qsTr("Name")
                    placeholder: qsTr("Untitled")
                    onAccepted: root.stage = "text"
                }

                // A dropdown, not chips: categories are user-extensible (per-
                // church custom setlists/folders on top of Song/Presentation),
                // so the list can grow past what a chip row can hold — the
                // shared SelectField already scrolls past menuMaxHeight.
                SelectField {
                    width: parent.width
                    label: qsTr("Category")
                    options: root.categories.map((c) => ({ label: c.name, value: c.id }))
                    value: (root.categories.find((c) => c.id === root.category) || {}).name || ""
                    onValuePicked: (v) => root.category = v
                }

                Row {
                    width: parent.width
                    spacing: Theme.space3

                    OptionTile {
                        width: (parent.width - 2 * Theme.space3) / 3
                        label: qsTr("Quick lyrics")
                        icon: "typeCase"
                        onActivated: root.stage = "text"
                    }
                    OptionTile {
                        width: (parent.width - 2 * Theme.space3) / 3
                        label: qsTr("Web search")
                        icon: "search"
                        iconGrid24: false
                        naturalIconPx: 10.5   // IconGlyph.qml's searchC: width/height 10.5
                        // FreeShow's own CreateShow.svelte: this tile brightens
                        // once a name is typed (it needs something to search
                        // for). The search itself (CCLI/SongSelect lookup) is
                        // not built yet — brightening it is honest about WHEN
                        // it would be usable, the badge stays to say it isn't
                        // yet, and clicking says so instead of doing nothing.
                        tileEnabled: nameField.text.trim().length > 0
                        badge: qsTr("Soon")
                        onActivated: EventBus.notify(qsTr("Web search isn't built yet — CCLI/SongSelect lookup is still on the list."),
                                                     "info", qsTr("New show"))
                    }
                    OptionTile {
                        width: (parent.width - 2 * Theme.space3) / 3
                        label: qsTr("Empty show")
                        icon: "plus"
                        iconGrid24: false
                        naturalIconPx: 8.17   // IconGlyph.qml's plusC: width/height 8.17
                        onActivated: root.createEmpty()
                    }
                }
            }

            // ---- stage: text (Quick lyrics) ----
            Column {
                width: parent.width
                spacing: Theme.space3
                visible: root.stage === "text"

                Row {
                    spacing: 6
                    IconGlyph {
                        name: "arrowLeft"
                        color: Theme.textMuted
                        implicitWidth: 14
                        implicitHeight: 14
                        anchors.verticalCenter: parent.verticalCenter
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -6
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.stage = "pick"
                        }
                    }
                    Text {
                        text: qsTr("Quick lyrics")
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Text {
                    width: parent.width
                    text: qsTr("Paste lyrics — a blank line starts a new slide, [Verse]/[Chorus] headers group them.")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                    wrapMode: Text.WordWrap
                }

                Rectangle {
                    width: parent.width
                    height: 220
                    radius: Theme.radiusMd
                    color: Theme.inset
                    border.width: 1
                    border.color: lyricsEdit.activeFocus ? Theme.accent : Theme.border

                    Text {
                        anchors.fill: parent
                        anchors.margins: 10
                        visible: lyricsEdit.text === "" && !lyricsEdit.activeFocus
                        text: qsTr("Verse 1\nAmazing grace, how sweet the sound\n\nChorus\n...")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.textSm
                        wrapMode: Text.WordWrap
                    }

                    Flickable {
                        anchors.fill: parent
                        anchors.margins: 10
                        clip: true
                        contentWidth: width
                        contentHeight: Math.max(height, lyricsEdit.contentHeight)
                        boundsBehavior: Flickable.StopAtBounds

                        TextEdit {
                            id: lyricsEdit
                            width: parent.width
                            wrapMode: TextEdit.Wrap
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                            selectByMouse: true
                        }
                    }
                }

                Row {
                    anchors.right: parent.right
                    spacing: Theme.space3
                    AppButton {
                        text: qsTr("Cancel")
                        variant: "secondary"
                        onClicked: root.close()
                    }
                    AppButton {
                        text: qsTr("Create")
                        variant: "primary"
                        enabled: lyricsEdit.text.trim().length > 0
                        onClicked: root.createFromLyrics()
                    }
                }
            }
        }
    }
}
