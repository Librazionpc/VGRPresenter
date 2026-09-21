import QtQuick
import QtQuick.Shapes
import VGRPresenterUI

// The Scripture-architecture library browser — the multi-column reference
// layout (collections sidebar → books → numbered verses → live output
// preview) shared by the Scripture tab and The Table tab. IT OWNS NO DATA:
// the engine drives everything. The host (or the engine bridge, once
// wired) calls setLibrary() with the engine's normalized document and the
// same architecture browses it — sidebar groups, book list, numbered verse
// rows, and a preview tile, exactly like the reference screenshots.
// No file loading happens here — no JSON, no XHR; data arrives in memory
// from the engine, the single source of truth for all library content.
//
// Accepted document (setLibrary normalizes both shapes):
//   Scripture:  { "name": "King James Version", "groups": [
//                 { "name": "Old Testament", "items": [
//                   { "name": "Genesis", "chapters": [ ["v1", "v2"], … ] } ] } ] }
//   The Table:  { "name": "…", "collections": [
//                 { "name": "Sunday Services", "books": [
//                   { "title": "…", "sections": [ { "verses": ["…", …] }, … ] } ] } ] }
// groups=collections, items=books, chapters=sections — isomorphic, so one
// component serves both libraries.
Item {
    id: root

    // Loaded content — assigned only through setLibrary() (engine-driven).
    property var groups: []          // [{ name, items: [{ name, chapters }] }]
    property string sourceName: ""
    readonly property bool hasSource: groups.length > 0
    // Sidebar's secondary flavor ("Bibles" vs "Collections") — same layout
    // both tabs, only the label follows the library.
    property string sidebarLabel: qsTr("Bibles")
    property string newEntryLabel: qsTr("New scripture")
    // The tab bar's search for this tab: only the books whose name contains it are listed.
    property string filter: ""
    // The "+ New ..." pill was tapped (the host decides what creating means:
    // Scripture opens a file dialog for a Bible, The Table imports a sermon).
    signal newEntryActivated()

    // Compact metrics — sized for the 760×353 media_table dock the tabs
    // live in. Exposed as properties so a wider host (full-window pane)
    // only overrides numbers.
    // The sidebar owns its width (drag its divider); the columns beside it follow.
    readonly property int sidebarW: sidebar.width
    property int booksW: 130
    readonly property int versesX: sidebarW + booksW
    property int previewW: 260
    property int previewH: 150

    // Selection state
    property int currentGroup: 0
    property int currentBook: 0
    property int currentChapter: 0
    // Preview tile: the reference shows the chapter's first verse; clicking
    // a verse row pins that verse instead.
    property string previewRef: ""
    property string previewText: ""

    // Engine entry point: replace the whole document in one call. Called by
    // the host/bridge when the engine reports library content; the pane
    // never fetches anything itself.
    function setLibrary(doc) {
        if (!doc || typeof doc !== "object") { groups = []; sourceName = ""; return }
        loadNormalized(doc)
    }

    // Normalizes both export shapes into groups[].items[].chapters[][]
    function loadNormalized(doc) {
        const raw = doc.groups !== undefined ? doc.groups
                  : (doc.collections !== undefined ? doc.collections : [])
        if (doc.name) sourceName = doc.name
        const out = []
        for (let g = 0; g < raw.length; ++g) {
            const gin = raw[g]
            const itemsIn = gin.items !== undefined ? gin.items
                          : (gin.books !== undefined ? gin.books : [])
            const gitems = []
            for (let b = 0; b < itemsIn.length; ++b) {
                const bin = itemsIn[b]
                const chIn = bin.chapters !== undefined ? bin.chapters
                           : (bin.sections !== undefined ? bin.sections : [])
                const chapters = []
                for (let c = 0; c < chIn.length; ++c) {
                    const row = chIn[c]
                    if (Array.isArray(row)) chapters.push(row.map(String))
                    else if (typeof row === "string") chapters.push([row])
                    else if (row && row.verses !== undefined) chapters.push(row.verses.map(String))
                }
                gitems.push({ name: String(bin.name !== undefined ? bin.name : (bin.title !== undefined ? bin.title : "")), chapters: chapters })
            }
            out.push({ name: String(gin.name !== undefined ? gin.name : (gin.title !== undefined ? gin.title : "")), items: gitems })
        }
        groups = out
        currentGroup = 0; currentBook = 0; currentChapter = 0
        updatePreview()
    }

    function bookChapters() {
        const g = groups[currentGroup]
        return (g && g.items[currentBook]) ? g.items[currentBook].chapters : []
    }

    // Whole-library search. The search box feeds `filter`; when it is longer
    // than a book-name fragment (two+ words) the verses column shows the
    // matches as "Book C:V — paragraph" rows instead of the chapter list.
    // The HOST supplies the hits (its engine's search); this only renders.
    property var searchHits: []          // [{ reference, snippet }]
    readonly property bool searching: {
        const words = filter.trim().split(/\s+/).filter((w) => w.length > 0)
        return words.length >= 2
    }
    function updatePreview() {
        const chs = bookChapters()
        const ch = currentChapter < chs.length ? chs[currentChapter] : null
        if (ch && ch.length) {
            const book = groups[currentGroup].items[currentBook]
            previewRef = book.name + " " + (currentChapter + 1) + ":1"
            previewText = ch[0]
        } else { previewRef = ""; previewText = "" }
    }
    function selectBook(b) { currentBook = b; currentChapter = 0; updatePreview() }
    function selectChapter(c) { currentChapter = c; updatePreview() }

    // The verses column's rows: search hits when searching, else the open
    // chapter's paragraphs (a function — the QML compiler rejects a binding
    // mixing a conditional with an object literal here).
    function verseRows() {
        if (root.searching) return root.searchHits
        const chs = root.bookChapters()
        return root.currentChapter < chs.length ? chs[currentChapter] : []
    }

    // Search-hit row tapped: jump to its book + chapter, pin the verse.
    function goToHit(bookName, chapter, verse) {
        for (let g = 0; g < groups.length; ++g) {
            const bi = groups[g].items.findIndex((b) => b.name === bookName)
            if (bi >= 0) {
                currentGroup = g
                selectBook(bi)
                selectChapter(chapter - 1)
                pinVerse(verse - 1)
                return
            }
        }
    }
    function pinVerse(v) {
        const cut = previewRef.lastIndexOf(":")
        if (cut > 0) previewRef = previewRef.slice(0, cut + 1) + (v + 1)
        previewText = bookChapters()[currentChapter][v]
    }

    // ---- Sidebar (collections / Bibles) ---------------------------------
    LibrarySidebar {
        id: sidebar
        height: parent.height
        defaultWidth: 150
        minWidth: 120
        maxWidth: 320

        Rectangle {
            x: 8; y: 8; width: parent.width - 16; height: 27
            color: "#12131a"; radius: 6
            Text {
                x: 10; y: 7
                text: root.sidebarLabel
                color: Theme.textSecondary
                font.family: Theme.fontFamily; font.pixelSize: 11; font.bold: true
            }
        }
        // The current source as the sidebar's live entry (the "King James
        // Version" row in the reference) — red active edge + book icon.
        Rectangle {
            x: 8; y: 42; width: parent.width - 16; height: 29
            color: "#1a1414"; radius: 6
            Rectangle { x: 0; y: 4; width: 3; height: 21; color: Theme.danger; radius: 1.5 }
            Shape {
                x: 10; y: 8.5
                width: 8; height: 10
                preferredRendererType: Shape.CurveRenderer
                ShapePath {
                    fillColor: "transparent"
                    strokeColor: Theme.danger
                    strokeWidth: 2
                    capStyle: ShapePath.RoundCap
                    PathSvg { path: "M 0 8.75 L 0 1.25 C 0 0.92 0.13 0.60 0.37 0.37 C 0.60 0.13 0.92 0 1.25 0 L 7.5 0 C 7.63 0 7.76 0.05 7.85 0.15 C 7.95 0.24 8 0.37 8 0.5 L 8 9.5 C 8 9.63 7.95 9.76 7.85 9.85 C 7.76 9.95 7.63 10 7.5 10 L 1.25 10 C 0.92 10 0.60 9.87 0.37 9.63 C 0.13 9.40 0 9.08 0 8.75 Z M 0 8.75 C 0 8.42 0.13 8.10 0.37 7.87 C 0.60 7.63 0.92 7.5 1.25 7.5 L 8 7.5" }
                }
            }
            Text {
                x: 26; y: 8; width: parent.width - 34
                text: root.sourceName !== "" ? root.sourceName
                    : qsTr("Waiting for engine…")
                color: Theme.textPrimary
                elide: Text.ElideRight
                font.family: Theme.fontFamily; font.pixelSize: 11
            }
        }
        // The "+ New ..." pill (reference's "+ New collection" box): the HOST
        // decides what creating means — this pane only signals.
        SidebarAddButton {
            x: 8; y: parent.height - 40; width: parent.width - 16
            text: root.newEntryLabel
            onClicked: root.newEntryActivated()
        }
    }

    // ---- Books column ----------------------------------------------------
    Rectangle {
        id: booksCol
        x: root.sidebarW; y: 0
        width: root.booksW; height: parent.height
        color: "#12131a"

        Flickable {
            anchors.fill: parent
            clip: true
            contentHeight: booksColList.height
            Column {
                id: booksColList
                x: 0; y: 4; width: parent.width - 8
                Repeater {
                    model: {
                        const g = root.groups[root.currentGroup]
                        const all = g ? g.items : []
                        const needle = root.filter.trim().toLowerCase()
                        return needle === "" ? all : all.filter((b) => String(b.name).toLowerCase().indexOf(needle) >= 0)
                    }
                    delegate: Item {
                        required property var modelData
                        required property int index
                        width: booksColList.width
                        height: 28
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 2
                            radius: 4
                            color: root.currentBook === index ? "#1e1f28"
                                 : (bookMouse.containsMouse ? "#1a1b23" : "transparent")
                        }
                        Text {
                            x: 12; y: 7; width: parent.width - 20
                            text: modelData.name
                            color: root.currentBook === index ? Theme.textPrimary : Theme.textSecondary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 13
                        }
                        MouseArea {
                            id: bookMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectBook(index)
                        }
                    }
                }
            }
        }
        Rectangle { anchors.right: parent.right; width: 2; height: parent.height; color: Theme.border }
    }

    // ---- Verses column + live preview ------------------------------------
    Rectangle {
        id: versesCol
        x: root.versesX; y: 0
        width: parent.width - root.versesX; height: parent.height
        color: "#0f1015"

        Flickable {
            anchors.fill: parent
            clip: true
            contentHeight: versesColList.height
            Column {
                id: versesColList
                x: 0; y: 4; width: parent.width - 12
                Repeater {
                    model: root.verseRows()
                    delegate: Item {
                        id: verseRow
                        required property var modelData
                        required property int index
                        // Search rows carry { reference, snippet }; chapter rows are plain strings.
                        readonly property bool isHit: root.searching
                        readonly property string hitRef: isHit ? String(modelData.reference ?? "") : ""
                        readonly property string hitSnippet: isHit ? String(modelData.snippet ?? "") : ""
                        width: versesColList.width
                        height: verseText.implicitHeight + 14
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            radius: 4
                            color: verseMouse.containsMouse ? "#16171e" : "transparent"
                        }
                        Text {
                            x: 8; y: 8
                            width: verseRow.isHit ? 76 : 22
                            horizontalAlignment: Text.AlignRight
                            text: verseRow.isHit ? verseRow.hitRef : (index + 1)
                            color: Theme.danger
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 12; font.bold: true
                        }
                        Text {
                            id: verseText
                            x: verseRow.isHit ? 92 : 40; y: 8
                            width: parent.width - root.previewW - (verseRow.isHit ? 108 : 56)
                            text: verseRow.isHit ? verseRow.hitSnippet : modelData
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 12
                        }
                        MouseArea {
                            id: verseMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (verseRow.isHit) {
                                    // "1953 12:3" -> book "1953", chapter 12, verse 3
                                    const parts = verseRow.hitRef.split(" ")
                                    const refParts = (parts[1] ?? "").split(":")
                                    root.goToHit(parts[0], parseInt(refParts[0]) || 1, parseInt(refParts[1]) || 1)
                                } else {
                                    root.pinVerse(index)
                                }
                            }
                        }
                    }
                }
            }
        }
        // Live preview tile — the reference's right-hand output preview.
        Rectangle {
            x: parent.width - root.previewW - 8; y: 6
            width: root.previewW; height: root.previewH
            color: "#000000"; radius: 2
            Text {
                anchors.fill: parent
                anchors.margins: 14
                anchors.bottomMargin: 34
                text: root.previewText
                color: "#ffffff"
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                font.family: "Georgia"; font.pixelSize: 19
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 8
                text: root.previewRef
                color: "#ffffff"
                font.family: "Georgia"; font.pixelSize: 16
            }
        }
        Rectangle { anchors.right: parent.right; width: 2; height: parent.height; color: Theme.border }
    }

    // ---- Engine-waiting empty state ---- declared LAST so it paints above
    // the opaque column backgrounds: until the bridge pushes the library
    // document (setLibrary), the columns would read as a black void; this
    // overlay states exactly what's happening instead.
    Rectangle {
        visible: !root.hasSource
        anchors.fill: parent
        color: "#0f1015"
        Column {
            anchors.centerIn: parent
            spacing: Theme.space2
            IconGlyph {
                name: "book"
                color: Theme.textMuted
                width: 24; height: 24
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.sourceName !== "" ? root.sourceName : qsTr("Library not loaded yet")
                color: Theme.textPrimary
                font.family: Theme.fontFamily; font.pixelSize: 18; font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Waiting for the engine to publish this library…")
                color: Theme.textMuted
                font.family: Theme.fontFamily; font.pixelSize: Theme.textSm
            }
        }
    }
}
