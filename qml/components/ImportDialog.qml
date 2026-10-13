import QtQuick
import Qt.labs.platform as Platform
import VGRPresenterUI

// The Import screen (FreeShow's Import popup, File > Import): every file format the app reads, laid out the way FreeShow lays them
// out - its own files up front (song/presentation file, project file, "More options" for templates / actions / stage layouts /
// themes), "Paste from clipboard", Media import (Lessons.church, PDF, PowerPoint), Text import (a card for every song and lyric
// format), and the scripture and calendar formats.
//
// The list itself is the ENGINE's (ImportService.formats, from bps::import::ImportFormats): which formats exist, their sections,
// which are up front, the extensions each opens and the note that goes with it. This draws it, opens the file picker filtered to
// the format, and hands the picked files to the engine, which converts them. A format the engine has not written a converter for
// yet is shown dimmed and says so.
//
// Literal colours, not Theme.* (a modal opened from the window root, like the other modal cards).
Item {
    id: root
    // Shared top-level modal layer: keep the scrim and card above the page
    // so clicks and hover never reach the controls behind the dialog.
    z: 30000

    property bool open: false
    signal closed()

    // "" = the main page, "freeshow_more" = the rest of FreeShow's own formats
    property string page: ""
    property string pendingFormat: ""
    property string pendingFilter: ""

    anchors.fill: parent
    visible: root.open
    onOpenChanged: if (open) root.page = ""

    readonly property var formats: ImportService.formats
    function inSection(name, primaryOnly, restOnly) {
        return root.formats.filter((f) => f.section === name && (!primaryOnly || f.primary) && (!restOnly || !f.primary))
    }

    // Opens the file picker for a format (after showing its note, if it has one).
    function pick(format) {
        if (!format.available)
            return
        if (format.tutorial !== "")
            EventBus.notify(format.tutorial, "info", format.name, "import.tutorial")
        root.pendingFormat = format.id
        root.pendingFilter = format.filter
        // The format's OWN patterns only - "All files" is not offered beside
        // them. A file of another type could only be refused later by the
        // importer (that is what the format's extension list encodes), so the
        // second entry turned a wrong choice into a failed import instead of
        // letting the picker stop it. Formats that declare no extensions
        // (OpenSong, SongBeamer) arrive with the all-files filter AS their
        // filter, so they are unaffected.
        picker.nameFilters = [format.filter]
        picker.open()
    }

    function finish(answer) {
        // importFiles returns { ok, started } immediately and reports the real
        // outcome later via ImportService.finished — the dialog stays up with
        // a progress overlay while the sweep runs, closing on success.
        if (answer.started) return
        if (answer.ok)
            root.closed()
    }

    Connections {
        target: ImportService
        function onFinished(answer) {
            if (answer.ok) root.closed()
        }
    }

    Platform.FileDialog {
        id: picker
        title: qsTr("Import")
        fileMode: Platform.FileDialog.OpenFiles
        onAccepted: {
            const urls = picker.files.map((u) => u.toString())
            root.finish(ImportService.importFiles(root.pendingFormat, urls))
        }
    }

    ModalScrim {
        anchors.fill: parent
        onDismissed: root.closed()
    }

    // One format: an icon tile, its name, and a "Soon" chip while its converter is not written.
    component FormatCard: Rectangle {
        id: card
        required property var format
        property bool wide: false
        width: 200
        height: 60
        radius: 8
        color: hover.hovered && card.format.available ? Theme.hoverBg : Theme.rowBg
        // The "primary" format card reads as the brighter one (its border was a
        // lighter shade than a plain card's); Theme has no strong-border token, so
        // the brighter outline is derived from the divider token.
        border.color: card.format.primary ? Qt.lighter(Theme.borderSubtle, 1.3) : Theme.borderSubtle
        border.width: 1
        opacity: card.format.available ? 1 : 0.5

        Rectangle {
            x: 8; anchors.verticalCenter: parent.verticalCenter
            width: 44; height: 44; radius: 8
            color: Theme.inset
            IconGlyph {
                anchors.centerIn: parent
                name: card.format.icon
                fit: true
                color: card.format.available ? Theme.accentLight : Theme.textMuted
                width: 20; height: 20
            }
        }
        Column {
            x: 62; anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 70
            spacing: 2
            Text {
                width: parent.width
                text: card.format.name
                color: Theme.textPrimary; elide: Text.ElideRight
                font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.Medium
            }
            Text {
                visible: !card.format.available
                text: qsTr("Coming soon")
                color: Theme.warning
                font.family: Theme.fontFamily; font.pixelSize: 12
            }
            Text {
                visible: card.format.available && card.format.description !== ""
                width: parent.width
                text: card.format.description
                color: Theme.textMuted; elide: Text.ElideRight
                font.family: Theme.fontFamily; font.pixelSize: 12
            }
        }
        HoverHandler { id: hover; cursorShape: card.format.available ? Qt.PointingHandCursor : Qt.ArrowCursor }
        TapHandler { onTapped: root.pick(card.format) }
    }

    // A labelled divider between sections.
    component Rule: Item {
        property string title: ""
        width: parent.width; height: 26
        Rectangle { anchors.verticalCenter: parent.verticalCenter; width: parent.width; height: 1; color: Theme.border }
        Rectangle {
            anchors.centerIn: parent
            width: label.width + 16; height: parent.height
            color: Theme.surface
            Text { id: label; anchors.centerIn: parent; text: parent.parent.title; color: Theme.textSecondary; font.family: Theme.fontFamily; font.pixelSize: 13; font.weight: Font.DemiBold }
        }
    }

    // An outlined action button (Paste from clipboard, Back...).
    component ActionButton: Rectangle {
        id: btn
        property string text: ""
        signal clicked()
        width: parent.width; height: 40; radius: 8
        color: btnHover.hovered ? Theme.hoverBg : Theme.rowBg
        border.color: Theme.borderSubtle
        Text { anchors.centerIn: parent; text: btn.text; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 15 }
        HoverHandler { id: btnHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: btn.clicked() }
    }

    Rectangle {
        id: dialogCard
        // Hook for the modal-fit scenario — same convention as ModalCard's card:
        // the modal ROOT always fills the window, so only the card can show the
        // scale it is drawn at.
        objectName: root.objectName === "" ? "" : root.objectName + "_card"
        anchors.centerIn: parent
        // The app's one scale, on the card (see AppScale.qml) — never on the root
        // above, whose scrim IS the dialog's black box and has to stay
        // window-sized. Centred, so scaling about the centre leaves it where it
        // was, in proportion with the screens behind it.
        scale: AppScale.cardScale(dialogCard)
        transformOrigin: Item.Center
        width: Math.min(parent.width - 48, 900)
        height: Math.min(parent.height - 48, 720)
        radius: 14
        color: Theme.surface
        border.color: Theme.border
        border.width: 1
        clip: true

        // Swallows clicks so they don't fall through to the scrim behind it.
        MouseArea { anchors.fill: parent; onClicked: {} }

        // ---- title ----
        Item {
            id: header
            x: 24; y: 18; width: parent.width - 48; height: 32

            Rectangle {
                visible: root.page !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: 28; height: 28; radius: 7
                color: backHover.hovered ? Theme.hoverBg : "transparent"
                IconGlyph { anchors.centerIn: parent; name: "chevronRight"; rotation: 180; color: Theme.textSecondary; width: 12; height: 12 }
                HoverHandler { id: backHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.page = "" }
            }
            Text {
                x: root.page !== "" ? 36 : 0
                anchors.verticalCenter: parent.verticalCenter
                text: root.page === "freeshow_more" ? qsTr("More FreeShow formats") : qsTr("Import")
                color: Theme.textPrimary
                font.family: Theme.fontFamily; font.pixelSize: 20; font.weight: Font.DemiBold
            }
            Rectangle {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 28; height: 28; radius: 7
                color: closeHover.hovered ? Theme.hoverBg : "transparent"
                IconGlyph { anchors.centerIn: parent; name: "close"; color: Theme.textSecondary; width: 10; height: 10 }
                HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.closed() }
            }
        }
        Rectangle { x: 0; y: header.y + header.height + 12; width: parent.width; height: 1; color: Theme.border }

        Flickable {
            id: importFlick
            x: 0; y: header.y + header.height + 13
            width: parent.width; height: parent.height - y
            clip: true
            contentHeight: body.height + 32
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: body
                x: 24; y: 16
                width: parent.width - 48
                spacing: 10

                // ---------- the rest of FreeShow's own formats ----------
                Flow {
                    visible: root.page === "freeshow_more"
                    width: parent.width; spacing: 8
                    Repeater {
                        model: root.inSection("freeshow", false, true)
                        delegate: FormatCard { required property var modelData; format: modelData; width: (body.width - 16) / 3 }
                    }
                }

                // ---------- the main page ----------
                Column {
                    visible: root.page === ""
                    width: parent.width
                    spacing: 10

                    // FreeShow's own files, and "More options"
                    Flow {
                        width: parent.width; spacing: 8
                        Repeater {
                            model: root.inSection("freeshow", true, false)
                            delegate: FormatCard { required property var modelData; format: modelData; width: (body.width - 16) / 3 }
                        }
                        Rectangle {
                            width: (body.width - 16) / 3; height: 60; radius: 8
                            color: moreHover.hovered ? Theme.hoverBg : Theme.rowBg; border.color: Theme.borderSubtle
                            Text { x: 16; anchors.verticalCenter: parent.verticalCenter; text: qsTr("More options"); color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 15 }
                            IconGlyph { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; name: "chevronRight"; color: Theme.textSecondary; width: 12; height: 12 }
                            HoverHandler { id: moreHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: root.page = "freeshow_more" }
                        }
                    }

                    ActionButton {
                        text: qsTr("Paste from clipboard")
                        onClicked: root.finish(ImportService.importClipboard())
                    }

                    Rule { title: qsTr("Media import") }
                    Flow {
                        width: parent.width; spacing: 8
                        Repeater {
                            model: root.inSection("media", false, false)
                            delegate: FormatCard { required property var modelData; format: modelData; width: (body.width - 16) / 3 }
                        }
                    }

                    Rule { title: qsTr("Text import") }
                    Flow {
                        width: parent.width; spacing: 8
                        Repeater {
                            model: root.inSection("text", false, false)
                            delegate: FormatCard { required property var modelData; format: modelData; width: (body.width - 16) / 3 }
                        }
                    }

                    Rule { title: qsTr("Scripture") }
                    Flow {
                        width: parent.width; spacing: 8
                        Repeater {
                            model: root.inSection("bible", false, false)
                            delegate: FormatCard { required property var modelData; format: modelData; width: (body.width - 16) / 3 }
                        }
                    }

                    Rule { title: qsTr("Calendar") }
                    Flow {
                        width: parent.width; spacing: 8
                        Repeater {
                            model: root.inSection("calendar", false, false)
                            delegate: FormatCard { required property var modelData; format: modelData; width: (body.width - 16) / 3 }
                        }
                    }
                }
            }

            // All the format sections overflow the dialog — real scrollbar,
            // not wheel-only.
            AppScrollBar {
                flickable: importFlick
                anchors.top: parent.top; anchors.bottom: parent.bottom
                anchors.right: parent.right; anchors.rightMargin: 3
            }
        }
    }

    // The async import's progress overlay: the window would otherwise sit
    // silent (or frozen, in the pre-thread era) for a large batch. Declared
    // LAST (a sibling of dialogCard, after it) — QML paints later siblings
    // on top, and dialogCard's own opaque background was hiding this
    // completely even while ImportService.busy was genuinely true: the
    // sweep visibly ran (progress/status updated, the log showed it), but
    // nothing on screen said so, which is exactly what read as a hang.
    Rectangle {
        anchors.fill: parent
        visible: ImportService.busy
        color: Qt.alpha(Theme.windowBg, 0.9)

        MouseArea { anchors.fill: parent }   // block interaction while importing

        Rectangle {
            width: 320; height: 108
            anchors.centerIn: parent
            radius: 10
            color: Theme.rowBg
            border.color: Theme.borderSubtle
            border.width: 1

            Column {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 12
                Text {
                    width: parent.width
                    text: qsTr("Importing…")
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: 15; font.weight: Font.DemiBold
                }
                Text {
                    width: parent.width
                    text: ImportService.status
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily; font.pixelSize: 13
                }
                Rectangle {
                    width: parent.width; height: 6; radius: 3
                    color: Theme.chip
                    Rectangle {
                        width: Math.max(parent.width * ImportService.progress, 6)
                        height: parent.height
                        radius: 3
                        color: Theme.accentLight
                        Behavior on width { NumberAnimation { duration: 120 } }
                    }
                }
            }
        }
    }
}
