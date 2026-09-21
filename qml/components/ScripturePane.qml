import QtQuick
import VGRPresenterUI

// The Scripture tab, laid out the way FreeShow's is (components/drawer/bible/Scripture.svelte + info/ScriptureInfo.svelte):
//
//   Bibles | Books | Chapters | Verses                                   | Preview
//                                                                          | Template (name, x, pick, edit)
//                                                                          | "Use default template" / "Convert to show"
//   [King James Version: Genesis 1:2]                    ( < > search )    | (options button: verse numbers, splitting...)
//
// IT OWNS NO SCRIPTURE: the installed Bibles, their books / chapters / verses, reference lookup and search, and the slide a
// chosen passage becomes when a scripture template is filled with it all come from the ENGINE (ScriptureService). Picking
// verses re-asks the engine for the preview, so what is drawn on the right is exactly the slide the engine would build - the
// same template placeholders, the same options (verse numbers, verses on individual lines, dividing long verses, smart
// split), the same reference wording. "Convert to show" hands those slides to the Edit screen.
Item {
    id: root

    // The tab bar's search box: a reference ("John 3:16") jumps there, anything else narrows the book list.
    property string filter: ""

    signal templateEditRequested(string templateId)
    // "Convert to show": the show's name and its slides ([{ title, background, blocks }]), from the engine.
    signal convertToShowRequested(string name, var slides)

    // ---- what is open ----
    property string bibleId: ""
    property var books: []
    property int bookIndex: 0
    property int chapterNumber: 1
    property var chapterVerses: []       // [{ number, text }]
    property var selected: []            // verse numbers, ascending
    property int anchorVerse: 0
    property bool optionsOpen: false
    property bool searching: false
    property var searchResults: []
    // Bumped whenever the engine's side changes (a Bible finished installing, the template or an option changed), so the bindings that
    // ask the engine again notice.
    property int engineRevision: 0

    readonly property var book: root.books.length > root.bookIndex ? root.books[root.bookIndex] : null
    readonly property var bibles: ScriptureService.bibles
    readonly property var currentBible: {
        for (let i = 0; i < root.bibles.length; ++i)
            if (root.bibles[i].id === root.bibleId)
                return root.bibles[i]
        return null
    }
    readonly property string referenceText: root.book ? ScriptureService.reference(root.book.name, root.chapterNumber, root.selected) : ""
    readonly property var preview: {
        root.engineRevision
        return root.book ? ScriptureService.preview(root.bibleId, root.book.id, root.chapterNumber, root.selected) : { blocks: [], background: "", reference: "", hasValues: true, slideCount: 0 }
    }
    readonly property string templateId: { root.engineRevision; return ScriptureService.templateId() }
    readonly property bool usingDefaultTemplate: root.templateId === ScriptureService.defaultTemplateId()
    readonly property var options: SettingsService.values

    Connections {
        target: ScriptureService
        function onChanged() {
            root.engineRevision++
            root.syncBibles()
        }
    }
    Component.onCompleted: root.syncBibles()

    // ---- opening things ----

    // Keeps a valid Bible open: the one used last, else the first installed.
    function syncBibles() {
        const list = root.bibles
        if (list.length === 0) {
            root.bibleId = ""
            root.books = []
            root.chapterVerses = []
            root.selected = []
            return
        }
        if (root.currentBible)
            return
        const last = String(SettingsService.values["session.scriptureBible"] ?? "")
        const known = list.find((b) => b.id === last)
        root.openBible((known ?? list[0]).id)
    }

    function openBible(id) {
        if (id === root.bibleId && root.books.length > 0)
            return
        const previousBook = root.book ? root.book.id : ""
        root.bibleId = id
        root.books = ScriptureService.books(id)
        const same = root.books.findIndex((b) => b.id === previousBook)
        root.openBook(same >= 0 ? same : 0, same >= 0 ? root.chapterNumber : 0)
        if (String(SettingsService.values["session.scriptureBible"] ?? "") !== id)
            SettingsService.setValue("session.scriptureBible", id)
    }

    function openBook(index, chapter) {
        root.bookIndex = Math.max(0, Math.min(index, root.books.length - 1))
        const b = root.book
        if (!b)
            return
        const wanted = chapter > 0 && b.chapters.indexOf(chapter) >= 0 ? chapter : b.chapters[0]
        root.openChapter(wanted, true)
    }

    function openChapter(number, selectFirst) {
        root.chapterNumber = number
        root.chapterVerses = root.book ? ScriptureService.chapter(root.bibleId, root.book.id, number) : []
        root.searching = false
        if (selectFirst && root.chapterVerses.length > 0) {
            root.selected = [root.chapterVerses[0].number]
            root.anchorVerse = root.chapterVerses[0].number
        } else {
            root.selected = []
        }
        versesFlick.contentY = 0
    }

    // A verse was clicked: alone, with Ctrl added or removed, with Shift as a range from the last one.
    function selectVerse(number, ctrl, shift) {
        let next
        if (shift && root.anchorVerse > 0) {
            const lo = Math.min(root.anchorVerse, number), hi = Math.max(root.anchorVerse, number)
            next = root.chapterVerses.map((v) => v.number).filter((n) => n >= lo && n <= hi)
        } else if (ctrl) {
            next = root.selected.indexOf(number) >= 0 ? root.selected.filter((n) => n !== number) : root.selected.concat([number])
            root.anchorVerse = number
        } else {
            next = [number]
            root.anchorVerse = number
        }
        root.selected = next.sort((a, b) => a - b)
    }

    // The previous / next verse (crossing into the next chapter or book).
    function step(direction) {
        if (!root.book || root.chapterVerses.length === 0)
            return
        const last = root.selected.length > 0 ? root.selected[root.selected.length - 1] : 0
        const numbers = root.chapterVerses.map((v) => v.number)
        const at = numbers.indexOf(direction > 0 ? last : root.selected[0])
        const to = at + direction
        if (to >= 0 && to < numbers.length) {
            root.selected = [numbers[to]]
            root.anchorVerse = numbers[to]
            return
        }
        const chapters = root.book.chapters
        const ci = chapters.indexOf(root.chapterNumber) + direction
        if (ci >= 0 && ci < chapters.length) {
            root.openChapter(chapters[ci], false)
            root.selected = [root.chapterVerses[direction > 0 ? 0 : root.chapterVerses.length - 1].number]
        } else if (root.bookIndex + direction >= 0 && root.bookIndex + direction < root.books.length) {
            root.openBook(root.bookIndex + direction, 0)
            if (direction < 0) {
                const b = root.book
                root.openChapter(b.chapters[b.chapters.length - 1], false)
                root.selected = [root.chapterVerses[root.chapterVerses.length - 1].number]
            }
        }
    }

    // Jumps to a passage the engine resolved from typed text: { bookId, chapter, verseStart, verseEnd }.
    function goTo(ref) {
        const index = root.books.findIndex((b) => b.id === ref.bookId)
        if (index < 0)
            return
        root.openBook(index, ref.chapter > 0 ? ref.chapter : 0)
        if (ref.verseStart > 0) {
            const to = ref.verseEnd > 0 ? ref.verseEnd : ref.verseStart
            const picked = root.chapterVerses.map((v) => v.number).filter((n) => n >= ref.verseStart && n <= to)
            if (picked.length > 0) {
                root.selected = picked
                root.anchorVerse = picked[0]
            }
        }
    }

    onFilterChanged: {
        const ref = ScriptureService.resolve(root.filter, root.bibleId)
        if (ref.bookId !== undefined)
            root.goTo(ref)
    }

    readonly property var visibleBooks: {
        const needle = root.filter.trim().toLowerCase()
        if (needle === "" || ScriptureService.resolve(root.filter, root.bibleId).bookId !== undefined)
            return root.books
        return root.books.filter((b) => b.name.toLowerCase().indexOf(needle) >= 0)
    }

    function runSearch(text) {
        root.searchResults = text.trim() === "" ? [] : ScriptureService.search(text, root.bibleId)
    }

    // What a dragged verse carries into a project: the passage (the selected verses when this one is among them, else just this one).
    function dragPayload(numbers) {
        if (!root.book)
            return null
        const nums = numbers.slice().sort((a, b) => a - b)
        const ref = ScriptureService.reference(root.book.name, root.chapterNumber, nums)
        return { kind: "scripture", items: [{ ref: ref, name: ref, meta: { bible: root.bibleId, book: root.book.id, chapter: root.chapterNumber, verses: nums } }] }
    }

    // "Convert to show": the whole passage becomes a show, one slide per screenful of verses.
    function convertToShow() {
        if (root.selected.length === 0)
            return
        const slides = ScriptureService.slides(root.bibleId, root.book.id, root.chapterNumber, root.selected)
        if (slides.length > 0)
            root.convertToShowRequested(root.referenceText, slides)
    }

    // ---- Bibles ----------------------------------------------------------------------------------------------------
    LibrarySidebar {
        id: sidebar
        height: parent.height
        defaultWidth: 150
        minWidth: 120
        maxWidth: 320

        Rectangle {
            id: biblesHeader
            x: 8; y: 8; width: parent.width - 16; height: 27
            color: "#12131a"; radius: 6
            Text {
                x: 10; y: 7
                text: qsTr("Bibles")
                color: Theme.textSecondary
                font.family: Theme.fontFamily; font.pixelSize: 11; font.bold: true
            }
        }

        Flickable {
            x: 8; y: 42; width: parent.width - 16; height: parent.height - 42 - 48
            clip: true
            contentHeight: bibleList.height
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: bibleList
                width: parent.width
                spacing: 4

                Repeater {
                    model: root.bibles
                    delegate: Rectangle {
                        id: bibleRow
                        required property var modelData
                        readonly property bool active: modelData.id === root.bibleId
                        width: bibleList.width; height: 29; radius: 6
                        color: active ? "#1a1414" : (bibleHover.hovered ? "#16171e" : "transparent")

                        Rectangle { visible: bibleRow.active; x: 0; y: 4; width: 3; height: 21; color: Theme.danger; radius: 1.5 }
                        IconGlyph {
                            x: 10; anchors.verticalCenter: parent.verticalCenter
                            name: "bookOpen"
                            color: bibleRow.active ? Theme.danger : Theme.textMuted
                            width: 12; height: 12
                        }
                        Text {
                            x: 28; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 34
                            text: bibleRow.modelData.name
                            color: bibleRow.active ? Theme.textPrimary : Theme.textSecondary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 11
                        }
                        HoverHandler { id: bibleHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.openBible(bibleRow.modelData.id) }
                    }
                }

                Text {
                    visible: root.bibles.length === 0
                    x: 4; width: parent.width - 8
                    text: ScriptureService.loading ? qsTr("Reading the Bibles…") : qsTr("No Bible is installed yet.")
                    color: Theme.textMuted
                    wrapMode: Text.Wrap
                    font.family: Theme.fontFamily; font.pixelSize: 11
                }
            }
        }

        SidebarAddButton {
            x: 8; y: parent.height - 40; width: parent.width - 16
            text: qsTr("New scripture")
            onClicked: ScriptureService.importBible()
        }
    }

    // ---- Books ---------------------------------------------------------------------------------------------------------
    Rectangle {
        id: booksCol
        x: sidebar.width; y: 0
        width: 150; height: parent.height
        color: "#12131a"

        Flickable {
            anchors.fill: parent
            clip: true
            contentHeight: booksList.height + 8
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: booksList
                x: 0; y: 4; width: parent.width - 8
                Repeater {
                    model: root.visibleBooks
                    delegate: Item {
                        id: bookRow
                        required property var modelData
                        readonly property bool active: root.book !== null && modelData.id === root.book.id
                        width: booksList.width; height: 28
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 2; radius: 4
                            color: bookRow.active ? "#1e1f28" : (bookHover.hovered ? "#1a1b23" : "transparent")
                            Rectangle { visible: bookRow.active; x: 0; y: 4; width: 2; height: parent.height - 8; color: Theme.danger }
                        }
                        Text {
                            x: 12; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 20
                            text: bookRow.modelData.name
                            color: bookRow.active ? Theme.textPrimary : Theme.textSecondary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 13
                        }
                        HoverHandler { id: bookHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.openBook(root.books.findIndex((b) => b.id === bookRow.modelData.id), 0) }
                    }
                }
            }
        }
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
    }

    // ---- Chapters --------------------------------------------------------------------------------------------------------
    Rectangle {
        id: chaptersCol
        x: booksCol.x + booksCol.width; y: 0
        width: 52; height: parent.height
        color: "#101118"

        Flickable {
            anchors.fill: parent
            clip: true
            contentHeight: chapterList.height + 8
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: chapterList
                x: 4; y: 4; width: parent.width - 8
                Repeater {
                    model: root.book ? root.book.chapters : []
                    delegate: Item {
                        id: chapterRow
                        required property int modelData
                        readonly property bool active: modelData === root.chapterNumber
                        width: chapterList.width; height: 28
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 2; radius: 4
                            color: chapterRow.active ? "#1e1f28" : (chapterHover.hovered ? "#1a1b23" : "transparent")
                            Rectangle { visible: chapterRow.active; x: 0; y: 4; width: 2; height: parent.height - 8; color: Theme.danger }
                        }
                        Text {
                            anchors.centerIn: parent
                            text: chapterRow.modelData
                            color: chapterRow.active ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontFamily; font.pixelSize: 13
                        }
                        HoverHandler { id: chapterHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.openChapter(chapterRow.modelData, true) }
                    }
                }
            }
        }
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }
    }

    // ---- Verses (or the search results) ---------------------------------------------------------------------------------------
    Rectangle {
        id: versesCol
        x: chaptersCol.x + chaptersCol.width; y: 0
        width: parent.width - x - previewCol.width; height: parent.height
        color: "#0f1015"

        Flickable {
            id: versesFlick
            anchors.fill: parent
            clip: true
            contentHeight: (root.searching ? searchList.height : versesList.height) + 60
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: versesList
                visible: !root.searching
                x: 0; y: 4; width: parent.width - 8
                Repeater {
                    model: root.chapterVerses
                    delegate: Item {
                        id: verseRow
                        required property var modelData
                        readonly property bool active: root.selected.indexOf(modelData.number) >= 0
                        width: versesList.width; height: 32
                        Rectangle {
                            anchors.fill: parent; anchors.margins: 1; radius: 4
                            color: verseRow.active ? "#2a2b35" : (verseSource.containsMouse ? "#16171e" : "transparent")
                        }
                        Text {
                            x: 6; anchors.verticalCenter: parent.verticalCenter
                            width: 34; horizontalAlignment: Text.AlignRight
                            text: verseRow.modelData.number
                            color: Theme.danger
                            font.family: Theme.fontFamily; font.pixelSize: 12; font.bold: true
                        }
                        Text {
                            x: 52; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 60
                            text: verseRow.modelData.text
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 12
                        }
                        // Click selects (Ctrl / Shift add); drag carries the selection into a project; double-click adds it to the open one.
                        DragSource {
                            id: verseSource
                            anchors.fill: parent
                            payload: root.dragPayload(verseRow.active ? root.selected : [verseRow.modelData.number])
                            label: payload ? payload.items[0].name : ""
                            onActivated: {
                                const mods = Qt.application.keyboardModifiers
                                root.selectVerse(verseRow.modelData.number, (mods & Qt.ControlModifier) !== 0, (mods & Qt.ShiftModifier) !== 0)
                            }
                            onOpened: {
                                root.selectVerse(verseRow.modelData.number, false, false)
                                const p = root.dragPayload(root.selected)
                                if (p) ProjectService.dropOnProject(p.kind, p.items)
                            }
                        }
                    }
                }
            }

            Column {
                id: searchList
                visible: root.searching
                x: 0; y: 4; width: parent.width - 8
                Repeater {
                    model: root.searchResults
                    delegate: Item {
                        id: hitRow
                        required property var modelData
                        width: searchList.width; height: 32
                        Rectangle { anchors.fill: parent; anchors.margins: 1; radius: 4; color: hitHover.hovered ? "#16171e" : "transparent" }
                        Text {
                            x: 10; anchors.verticalCenter: parent.verticalCenter; width: 170
                            text: hitRow.modelData.reference
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 12
                        }
                        Text {
                            x: 190; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 198
                            text: hitRow.modelData.snippet
                            color: Theme.textSecondary
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily; font.pixelSize: 12
                        }
                        HoverHandler { id: hitHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                root.goTo({ bookId: hitRow.modelData.bookId, chapter: hitRow.modelData.chapter, verseStart: hitRow.modelData.verse, verseEnd: hitRow.modelData.verse })
                                root.searching = false
                            }
                        }
                    }
                }
                Text {
                    visible: root.searchResults.length === 0
                    x: 12; y: 12
                    text: qsTr("Type words to find in this Bible, then press Enter.")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
            }
        }

        // Nothing installed / nothing loaded yet.
        Text {
            visible: root.bibles.length === 0
            anchors.centerIn: parent
            text: ScriptureService.loading ? qsTr("Reading the Bibles…") : qsTr("Install a Bible to browse scripture.")
            color: Theme.textMuted
            font.family: Theme.fontFamily; font.pixelSize: 13
        }

        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

        // ---- the floating reference chip (FreeShow's left FloatingInputs) ----
        Rectangle {
            visible: root.currentBible !== null && root.referenceText !== ""
            x: 12; y: parent.height - height - 12
            height: 30; width: Math.min(refRow.implicitWidth + 24, Math.max(60, parent.width - pill.width - 36)); radius: 15
            color: "#e6101118"
            border.color: Theme.border
            Row {
                id: refRow
                anchors.centerIn: parent
                spacing: 6
                Text {
                    visible: versesCol.width > 560   // a narrow pane keeps just the reference
                    text: (root.currentBible ? root.currentBible.name : "") + ":"
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
                Text {
                    text: root.referenceText
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
            }
        }

        // ---- the floating controls (FreeShow's right FloatingInputs): previous / next verse, search ----
        Rectangle {
            id: pill
            x: parent.width - width - 12; y: parent.height - height - 12
            height: 34; width: pillRow.width + (root.searching ? 8 : 12) + 8; radius: 17
            color: "#e6101118"
            border.color: Theme.border

            Row {
                id: pillRow
                anchors.centerIn: parent
                spacing: 2

                Item {   // search field, while searching
                    visible: root.searching
                    width: 220; height: 26
                    Rectangle { anchors.fill: parent; radius: 13; color: "#181a22"; border.color: Theme.border }
                    TextInput {
                        id: searchInput
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12
                        verticalAlignment: TextInput.AlignVCenter
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily; font.pixelSize: 12
                        clip: true
                        onAccepted: root.runSearch(text)
                        Keys.onEscapePressed: { root.searching = false }
                    }
                    Text {
                        visible: searchInput.text === ""
                        anchors.verticalCenter: parent.verticalCenter; x: 12
                        text: qsTr("Search in this Bible")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily; font.pixelSize: 12
                    }
                }

                Repeater {
                    model: root.searching ? [] : [ { glyph: "chevronUp", dir: -1, tip: "previous" }, { glyph: "chevronDown", dir: 1, tip: "next" } ]
                    delegate: Item {
                        id: stepBtn
                        required property var modelData
                        width: 28; height: 26
                        Rectangle { anchors.fill: parent; radius: 13; color: stepHover.hovered ? "#22242e" : "transparent" }
                        IconGlyph {
                            anchors.centerIn: parent
                            name: stepBtn.modelData.glyph
                            color: Theme.textPrimary
                            width: 12; height: 12
                        }
                        HoverHandler { id: stepHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.step(stepBtn.modelData.dir) }
                    }
                }

                Rectangle { visible: !root.searching; width: 1; height: 16; color: Theme.border; anchors.verticalCenter: parent.verticalCenter }

                Item {
                    width: 28; height: 26
                    Rectangle { anchors.fill: parent; radius: 13; color: searchHover.hovered || root.searching ? "#22242e" : "transparent" }
                    IconGlyph {
                        anchors.centerIn: parent
                        name: "search"
                        color: Theme.textPrimary
                        width: 12; height: 12
                    }
                    HoverHandler { id: searchHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            root.searching = !root.searching
                            if (root.searching) {
                                root.searchResults = []
                                searchInput.text = ""
                                searchInput.forceActiveFocus()
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Preview + template + options -------------------------------------------------------------------------------------------
    // The column never scrolls as a whole: the controls (template, Convert to show, the options button) are pinned to the bottom, the
    // preview takes whatever height is left above them (it shrinks to fit instead of pushing them off the screen), and the options -
    // when the round button turns them on - scroll in between.
    Rectangle {
        id: previewCol
        anchors.right: parent.right
        width: Math.max(320, Math.min(420, root.width * 0.3)); height: parent.height
        color: "#101118"

        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Theme.border }

        // ---- the controls, pinned to the bottom ----
        Column {
            id: controls
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: 10; anchors.rightMargin: 10; anchors.bottomMargin: 12
            spacing: 10

            // ---- the template (FreeShow's "Template" row, the note about old templates, the two buttons) ----
            Rectangle {
                visible: !root.optionsOpen
                width: parent.width; height: 56; radius: 6
                color: "#0f1015"; border.color: Theme.border
                clip: true

                Column {
                    x: 12; anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text { text: qsTr("Template"); color: Theme.danger; font.family: Theme.fontFamily; font.pixelSize: 11 }
                    Text {
                        width: 150
                        text: ScriptureService.templateName(root.templateId)
                        color: Theme.textPrimary; elide: Text.ElideRight
                        font.family: Theme.fontFamily; font.pixelSize: 14; font.weight: Font.DemiBold
                    }
                }

                Row {
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    spacing: 0

                    // back to the default (only when another one is chosen)
                    Item {
                        visible: !root.usingDefaultTemplate
                        width: 36; height: 56
                        IconGlyph { anchors.centerIn: parent; name: "close"; color: clearHover.hovered ? Theme.textPrimary : Theme.textSecondary; width: 10; height: 10 }
                        HoverHandler { id: clearHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: SettingsService.setValue("scripture.template", "") }
                    }
                    // choose another scripture template
                    Item {
                        id: pickBtn
                        width: 36; height: 56
                        IconGlyph { anchors.centerIn: parent; name: "layoutTemplate"; color: pickHover.hovered ? Theme.textPrimary : Theme.textSecondary; width: 14; height: 14 }
                        HoverHandler { id: pickHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.openTemplateMenu(pickBtn) }
                    }
                    // edit it on the Edit screen
                    Rectangle {
                        width: 46; height: 56; color: editHover.hovered ? "#1a1b23" : "#0b0c11"
                        IconGlyph { anchors.centerIn: parent; name: "penTool"; color: Theme.textPrimary; width: 14; height: 14 }
                        HoverHandler { id: editHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.templateEditRequested(root.templateId) }
                    }
                }
            }

            Text {
                visible: !root.optionsOpen && root.selected.length > 0 && !root.preview.hasValues
                width: parent.width
                text: qsTr("You are using a template with no scripture values!")
                color: Theme.textSecondary; opacity: 0.85
                wrapMode: Text.Wrap
                font.family: Theme.fontFamily; font.pixelSize: 12
            }

            component PanelButton: Rectangle {
                id: btn
                property string text: ""
                property string info: ""
                signal clicked()
                width: parent.width; height: 44; radius: 6
                color: btnHover.hovered ? "#181a22" : "#0f1015"; border.color: Theme.border
                Row {
                    anchors.centerIn: parent
                    spacing: 8
                    Text { text: btn.text; color: Theme.textPrimary; font.family: Theme.fontFamily; font.pixelSize: 13 }
                    Text { visible: btn.info !== ""; text: btn.info; color: Theme.textMuted; font.family: Theme.fontFamily; font.pixelSize: 12 }
                }
                HoverHandler { id: btnHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: btn.clicked() }
            }

            PanelButton {
                visible: !root.optionsOpen && !root.usingDefaultTemplate && root.selected.length > 0 && !root.preview.hasValues
                text: qsTr("Use default template")
                onClicked: SettingsService.setValue("scripture.template", "")
            }

            // Convert to show, with the round options button beside it (so it never floats over anything)
            Item {
                width: parent.width; height: 48

                PanelButton {
                    visible: !root.optionsOpen
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 58
                    text: qsTr("Convert to show")
                    info: root.preview.slideCount > 1 ? qsTr("%1 slides").arg(root.preview.slideCount) : ""
                    onClicked: root.convertToShow()
                }

                // The round button that swaps the panel between the template and the options (FreeShow's tune button).
                Item {
                    id: optionsButton
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                    width: 48; height: 48
                    // a soft shadow: two faint discs under the button
                    Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 3; width: parent.width + 6; height: width; radius: width / 2; color: "#26000000" }
                    Rectangle { anchors.centerIn: parent; anchors.verticalCenterOffset: 2; width: parent.width + 2; height: width; radius: width / 2; color: "#33000000" }
                    Rectangle {
                        anchors.fill: parent; radius: width / 2
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: root.optionsOpen ? (optionsHover.hovered ? "#ff6a5c" : "#ff5a4b") : (optionsHover.hovered ? "#2a2d3b" : "#222533") }
                            GradientStop { position: 1.0; color: root.optionsOpen ? "#e23f30" : "#181a24" }
                        }
                        border.width: 1
                        border.color: root.optionsOpen ? "#ff8a7e" : "#34384a"
                        IconGlyph { anchors.centerIn: parent; name: "sliders"; color: "#ffffff"; fit: true; width: 22; height: 22 }
                    }
                    HoverHandler { id: optionsHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: root.optionsOpen = !root.optionsOpen }
                }
            }
        }

        // ---- the slide the engine builds for the picked verses, in the room above the controls ----
        Item {
            id: previewArea
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            height: root.optionsOpen ? Math.min(previewCol.height * 0.34, 190) : Math.max(70, controls.y - 4)

            Rectangle {
                id: previewTile
                anchors.horizontalCenter: parent.horizontalCenter
                y: 8
                // the largest 16:9 that fits both the width and the height that is left
                height: Math.max(48, Math.min((parent.width - 16) * 428 / 754, parent.height - 16))
                width: Math.round(height * 754 / 428)
                color: "#000000"
                clip: true
                DesignPreview {
                    anchors.fill: parent
                    blocks: root.preview.blocks
                    background: (root.preview.background ?? "") === "" || root.preview.background === "transparent" ? "#000000" : root.preview.background
                }
                Text {
                    visible: root.selected.length === 0
                    anchors.centerIn: parent
                    text: qsTr("Pick a verse")
                    color: "#5c6475"
                    font.family: Theme.fontFamily; font.pixelSize: 12
                }
            }
        }

        // ---- the options (the round button turns this on): they scroll between the preview and the button ----
        Flickable {
            id: optionsFlick
            visible: root.optionsOpen
            anchors.top: previewArea.bottom; anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: controls.top; anchors.bottomMargin: 8
            clip: true
            contentHeight: optionsBody.height + 8
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: optionsBody
                width: parent.width

                Text {
                    x: 16; height: 26; verticalAlignment: Text.AlignVCenter
                    text: qsTr("SLIDE OPTIONS")
                    color: Theme.textMuted
                    font.family: Theme.fontFamily; font.pixelSize: 10; font.weight: Font.DemiBold; font.letterSpacing: 1.2
                }
                Rectangle {
                    x: 10; width: parent.width - 20; height: optionsColumn.height + 8
                    radius: 8; color: "#0f1015"; border.color: Theme.border

                    Column {
                        id: optionsColumn
                        x: 14; y: 4; width: parent.width - 28
                        spacing: 0

                        component OptionToggle: Item {
                            id: opt
                            property string settingKey: ""
                            width: parent.width; height: 40
                            readonly property var def: SettingsService.definitions[opt.settingKey]
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1a1c25" }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: opt.def ? opt.def.label : ""
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily; font.pixelSize: Theme.textSm
                            }
                            SettingsToggle {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                checked: root.options[opt.settingKey] === true
                                onToggled: SettingsService.setValue(opt.settingKey, !(root.options[opt.settingKey] === true))
                            }
                        }
                        // A number with - / + buttons: `step` per click, kept inside the engine's own range.
                        component OptionNumber: Item {
                            id: num
                            property string settingKey: ""
                            property int step: 1
                            width: parent.width; height: 40
                            readonly property var def: SettingsService.definitions[num.settingKey]
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#1a1c25" }
                            function nudge(delta) {
                                const next = Math.max(num.def.min, Math.min(num.def.max, Number(root.options[num.settingKey]) + delta))
                                if (next !== root.options[num.settingKey])
                                    SettingsService.setValue(num.settingKey, next)
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: num.def ? num.def.label : ""
                                color: Theme.textPrimary
                                font.family: Theme.fontFamily; font.pixelSize: Theme.textSm
                            }
                            Row {
                                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                Rectangle {
                                    width: 24; height: 24; radius: 6; color: minusHover.hovered ? "#22242e" : "#181a22"; border.color: Theme.border
                                    Text { anchors.centerIn: parent; text: "−"; color: Theme.textPrimary; font.pixelSize: 13 }
                                    HoverHandler { id: minusHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: num.nudge(-num.step) }
                                }
                                Text {
                                    width: 40; horizontalAlignment: Text.AlignHCenter; anchors.verticalCenter: parent.verticalCenter
                                    text: root.options[num.settingKey]
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily; font.pixelSize: Theme.textSm
                                }
                                Rectangle {
                                    width: 24; height: 24; radius: 6; color: plusHover.hovered ? "#22242e" : "#181a22"; border.color: Theme.border
                                    Text { anchors.centerIn: parent; text: "+"; color: Theme.textPrimary; font.pixelSize: 13 }
                                    HoverHandler { id: plusHover; cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: num.nudge(num.step) }
                                }
                            }
                        }

                        OptionToggle { settingKey: "scripture.verseNumbers" }
                        OptionToggle { settingKey: "scripture.versesOnIndividualLines" }
                        OptionToggle { settingKey: "scripture.splitLongVerses" }
                        Column {
                            visible: root.options["scripture.splitLongVerses"] === true
                            width: parent.width
                            OptionToggle { settingKey: "scripture.splitLongVersesSuffix" }
                            OptionNumber { settingKey: "scripture.longVersesChars"; step: 10 }
                            OptionNumber { settingKey: "scripture.longVersesTolerance"; step: 5 }
                        }
                        OptionToggle { settingKey: "scripture.smartSplit" }
                        OptionNumber { visible: root.options["scripture.smartSplit"] === false; settingKey: "scripture.versesPerSlide" }
                    }
                }
            }
        }
    }

    // ---- the template chooser ------------------------------------------------------------------------------------------------------
    property var templateMenuTemplates: []
    function openTemplateMenu(source) {
        const list = ScriptureService.templates()
        root.templateMenuTemplates = list
        templateMenu.model = list.map((t) => ({ label: t.name, trailing: t.id === root.templateId ? "✓" : "" }))
        templateMenu.openAt(source, source.width - templateMenu.width, source.height + 4, root)
    }
    MenuCatcher { menu: templateMenu }
    DropdownPanel {
        id: templateMenu
        visible: false
        z: 25
        maxHeight: 320
        onItemActivated: (label) => {
            templateMenu.visible = false
            const chosen = root.templateMenuTemplates.find((t) => t.name === label)
            if (chosen)
                SettingsService.setValue("scripture.template", chosen.id === ScriptureService.defaultTemplateId() ? "" : chosen.id)
        }
    }
}
