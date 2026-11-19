import QtQuick
import VGRPresenterUI

ModalCard {
    id: root

    title: qsTr("Search")
    subtitle: qsTr("Shows, slides, templates, overlays, settings, songs and Bible verses.")
    cardWidth: 640
    showFooter: false

    signal resultChosen(var result)

    property string query: ""
    property var results: []
    property int current: 0
    property bool exact: false
    property string highlightSource: query
    property bool searchPending: false
    readonly property bool canHighlight: {
        const words = root.highlightSource.trim().toLowerCase().split(/\s+/)
            .filter((w) => w.length >= 2)
        return words.length > 0
    }

    readonly property var kindInfo: ({
        "bible":    { group: qsTr("Bible"),      badge: qsTr("BIBLE"),    color: "#e0b04a" },
        "show":     { group: qsTr("Shows"),      badge: qsTr("SHOW"),     color: Theme.accent },
        "slide":    { group: qsTr("Slides"),     badge: qsTr("SLIDE"),    color: "#4aa3e0" },
        "template": { group: qsTr("Templates"),  badge: qsTr("TEMPLATE"), color: "#4ac9a4" },
        "overlay":  { group: qsTr("Overlays"),   badge: qsTr("OVERLAY"),  color: "#c46ce7" },
        "category": { group: qsTr("Categories"), badge: qsTr("CATEGORY"), color: Theme.textSecondary },
        "setting":  { group: qsTr("Settings"),   badge: qsTr("SETTING"),  color: Theme.textSecondary },
        "song":     { group: qsTr("Songs"),      badge: qsTr("SONG"),     color: "#e06c8a" },
        "table":    { group: qsTr("Sermons"),    badge: qsTr("SERMON"),   color: "#4ae0b0" },
        "media":    { group: qsTr("Media"),      badge: qsTr("MEDIA"),    color: "#e08a4a" }
    })

    function openSearch() {
        root.query = ""
        queryInput.text = ""
        root.searchPending = false
        root.searchToken++
        root.resolveToken++
        root.results = []
        root.current = 0
        root.highlightSource = ""
        hint.text = ""
        root.loadPicks()               // what was picked before, ready to float
        ShowService.refreshLibrary()   // pick up files added since the last look
        root.open()
        queryInput.forceActiveFocus()
    }

    // ---- pick memory (user call: "cache what was actually picked, picked first") ----
    readonly property string picksKey: "session.quickSearchPicks"
    property var picks: []               // [{ key, title, kind }] most-recent-first
    function loadPicks() {
        let raw = []
        try { raw = JSON.parse(String(SettingsService.values[picksKey] ?? "[]")) } catch (e) { raw = [] }
        root.picks = Array.isArray(raw) ? raw : []
    }
    function pickKeyOf(result) {
        if (result.kind === "table" || result.kind === "bible")
            return result.kind + ":" + result.bookId + ":" + result.chapter + ":" + result.verse
        return result.kind + ":" + (result.path !== "" ? result.path : result.id) + ":" + result.title
    }
    function rememberPick(result) {
        if (!result || result.kind === undefined)
            return
        const key = pickKeyOf(result)
        const next = root.picks.filter((p) => p.key !== key)
        next.unshift({ key: key })
        root.picks = next.slice(0, 40)
        // Key-only entries: the ranking below reads nothing else, and the settings
        // store refuses Text values past 4096 chars — slim keeps 40 picks far clear.
        SettingsService.setValue(picksKey, JSON.stringify(root.picks))
    }
    function queryMatchRank(result) {
        const words = root.highlightSource.toLowerCase().match(/[A-Za-z0-9]+/g) || []
        if (words.length === 0) return 3
        const esc = words.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
        const exactPhrase = new RegExp("\\b" + esc.join("\\b\\W+") + "\\b", "i")
        const nearPhrase = new RegExp("\\b" + esc.join("\\b\\W+(?:\\w+\\W+){0,2}") + "\\b", "i")
        let best = 3
        for (const value of [result.title, result.subtitle, result.text, result.fullText]) {
            const source = String(value ?? "")
            if (source === "") continue
            if (!words.every((word) => new RegExp("\\b" + word + "\\b", "i").test(source))) continue
            if (exactPhrase.test(source)) return 0
            best = Math.min(best, nearPhrase.test(source) ? 1 : 2)
        }
        return best
    }
    function queryCoverage(result) {
        const words = root.highlightSource.toLowerCase().match(/[A-Za-z0-9]+/g) || []
        if (words.length === 0) return 0
        const source = [result.title, result.subtitle, result.text, result.fullText]
            .map((v) => String(v ?? "")).join(" ")
        if (source === "") return 0
        let carried = 0
        for (let i = 0; i < words.length; ++i) {
            const safe = words[i].replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
            const re = new RegExp(i + 1 === words.length ? "\\b" + safe : "\\b" + safe + "\\b", "i")
            if (re.test(source)) carried++
        }
        return carried
    }
    readonly property var adjacencyBonus: [0.90, 0.55, 0.20, 0.0]
    function matchRating(result) {
        const words = root.highlightSource.toLowerCase().match(/[A-Za-z0-9]+/g) || []
        if (words.length === 0)
            return 0
        const carried = root.queryCoverage(result)
        const bonus = root.adjacencyBonus[root.queryMatchRank(result)] ?? 0
        return Math.min(100, Math.round(100 * (carried + bonus) / words.length))
    }
    // What the row's pill SAYS. matchRating() counts how many typed words the row carries, so every row that holds all of
    // them — the sentence itself, or the words a page apart — read "MATCH 100%". The shown figure also reflects how close
    // together they are: the phrase itself 100, nearly the phrase (a word or two between) 92, merely present 75.
    // (Only the label: the sort above still uses matchRating and queryMatchRank.)
    readonly property var closenessFactor: [1.0, 0.92, 0.75, 1.0]
    function matchDisplay(result) {
        return Math.round(root.matchRating(result) * (root.closenessFactor[root.queryMatchRank(result)] ?? 1.0))
    }
    function isExactWordsMatch(result) {
        const words = root.highlightSource.toLowerCase().match(/[A-Za-z0-9]+/g) || []
        if (words.length === 0)
            return false
        const source = [result.text, result.fullText]
            .map((v) => String(v ?? "")).join(" ")
        return words.every((word) => {
            const safe = word.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
            return new RegExp("\\b" + safe + "\\b", "i").test(source)
        })
    }
    function rankByPicks(results) {
        if (root.exact)
            results = results.filter((r) => (r.kind === "bible" || r.kind === "table")
                                            ? root.isExactWordsMatch(r) : true)
        if (results.length < 2)
            return results
        const groupOf = {}
        for (const r of results)
            if (groupOf[r.kind] === undefined) groupOf[r.kind] = Object.keys(groupOf).length
        const MISSING = -1
        const scoreOf = (r) => (typeof r.score === "number" ? r.score : null)
        const rankOf = (r) => {
            const key = pickKeyOf(r)
            const i = root.picks.findIndex((p) => p.key === key)
            return i < 0 ? MISSING : i   // unpicked = after every pick
        }
        return results.slice().sort((a, b) => {
            const ra = root.matchRating(a), rb = root.matchRating(b)
            if (ra !== rb) return rb - ra
            // Equal rating: the tighter phrase leads (the exact sentence before
            // a nearly-exact one before the words merely scattered).
            const ma = root.queryMatchRank(a), mb = root.queryMatchRank(b)
            if (ma !== mb) return ma - mb
            const ga = groupOf[a.kind], gb = groupOf[b.kind]
            if (ga !== gb) return ga - gb
            const sa = scoreOf(a), sb = scoreOf(b)
            if (sa !== null && sb !== null && sa !== sb) return sb - sa   // engine first
            const pa = rankOf(a), pb = rankOf(b)
            if (pa !== pb) return pa === MISSING ? 1 : (pb === MISSING ? -1 : pa - pb)
            return 0                                   // stable sort: engine order
        })
    }

    // The request counter for the async search: only the latest token's rows
    // are applied (SearchService drops stale ones too — belt and braces).
    property int searchToken: 0
    property int resolveToken: 0
    property var resolvedWords: []
    property bool pendingTabFix: false
    function fixQueryWith(words) {
        let fixed = root.query.trim()
        for (const w of words) {
            if (w.changed !== true) continue   // spelled right: leave it
            const parts = String(w.resolved).split(/\s+/)
            fixed = fixed.replace(new RegExp("\\b" + w.typed + "\\b", "i"), parts.join(" "))
        }
        queryInput.text = fixed
        root.query = fixed
        root.refresh()
    }
    function applyResolvedWords(token, words) {
        if (token !== root.resolveToken || root.query.trim() === "")
            return
        root.resolvedWords = words
        // A Tab pressed while this resolution was in flight: its correction is
        // what the user asked for, so it lands now.
        if (root.pendingTabFix) {
            root.pendingTabFix = false
            root.fixQueryWith(words)
            return
        }
        const interesting = words.filter((w) => w.changed === true
                                           || (w.candidates && w.candidates.length > 0))
        hint.text = interesting.length === 0 ? ""
            : interesting.map((w) => (w.candidates && w.candidates.length > 0)
                 ? w.typed + " \u2192 " + w.candidates.slice(0, 3).join(" \u00b7 ")
                 : w.typed + " \u2192 " + w.resolved).join("   ")
        root.highlightSource = words.length === 0 ? root.query
            : words.reduce((acc, w) => acc.concat(
                  (root.exact && Number(w.weight) >= 1.0
                   ? String(w.typed) : String(w.resolved)).split(/\s+/)), []).join(" ")
        if (root.exact)
            return
        root.results = root.rankByPicks(root.results)
    }
    function refresh() {
        root.resolvedWords = []      // never a correction from the PREVIOUS query
        root.pendingTabFix = false
        if (root.query.trim() === "") {
            root.searchToken++
            root.resolveToken++
            root.searchPending = false
            root.results = []
            root.current = 0
            hint.text = ""
            root.highlightSource = ""
        } else {
            root.searchToken++
            root.searchPending = true
            root.results = []
            root.current = 0
            SearchService.searchAsync(root.query, 8, root.searchToken, root.exact)
            root.resolveToken++
            SearchService.resolveWordsAsync(root.query, root.resolveToken)
            hint.text = ""
            root.highlightSource = root.query
        }
        flick.contentY = 0
        root.current = 0
    }
    // The padlock's click: flip the mode and re-run what is already typed, so the
    // result list changes under the user's eyes instead of on the next keystroke.
    function toggleExact() {
        root.exact = !root.exact
        if (root.query.trim() !== "")
            root.refresh()
    }

    function choose(result) {
        root.rememberPick(result)
        root.searchPending = false
        root.searchToken++
        root.resolveToken++
        root.close()
        Qt.callLater(function() { root.resultChosen(result) })
    }

    // Enter: the highlighted result (the query is run first if the debounce hasn't fired).
    function activate() {
        if (debounce.running) { debounce.stop(); root.refresh() }
        if (root.results.length > 0) root.choose(root.results[Math.min(root.current, root.results.length - 1)])
    }

    // ---- yellow highlight of the typed words in a result's text -------------------
    function highlight(text) {
        const source = String(text ?? "")
        if (source === "")
            return ""
        const words = root.highlightSource.trim().toLowerCase().split(/\s+/).filter((w) => w.length >= 2)
        if (words.length === 0)
            return source
        const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        const safeWords = words.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
        const boundary = root.exact ? "\\b" : ""
        const re = new RegExp(boundary + "(" + safeWords.join("|") + ")" + boundary, "gi")
        const gold = String(Theme.highlightInk)
        return esc(source).replace(re, '<b><font color="' + gold + '">$1</font></b>')
    }
    function move(delta) {
        if (root.results.length === 0) return
        root.current = (root.current + delta + root.results.length) % root.results.length
        root.revealCurrent()
    }

    // Scrolls the list just enough to show the highlighted row (arrow-key navigation).
    function revealCurrent() {
        const row = resultsRepeater.itemAt(root.current)
        if (!row) return
        if (row.y < flick.contentY)
            flick.contentY = row.y
        else if (row.y + row.height > flick.contentY + flick.height)
            flick.contentY = row.y + row.height - flick.height
    }

    onCancelled: root.close()

    // Typing is debounced so a fast typist doesn't query the engine per keystroke.
    Timer {
        id: debounce
        interval: 140
        onTriggered: root.refresh()
    }
    property int verseRevision: 0

    // A Bible finishing its background load makes new results possible.
    Connections {
        target: SearchService
        function onBibleChanged() { if (root.shown && root.query !== "") root.refresh() }
        function onVerseCached(bibleId, bookId, chapter, verse) { root.verseRevision++ }
        // The async search's rows: applied only when this request is still the
        // latest (a fast typist's older keystrokes answer into the void).
        function onResultsReady(token, rows) {
            if (token !== root.searchToken || !root.shown) return
            root.searchPending = false
            root.results = root.rankByPicks(rows)
        }
        function onResultsProgress(token, rows) {
            if (token !== root.searchToken || !root.shown) return
            root.results = root.rankByPicks(rows)
        }
        function onWordsResolved(token, rows) { root.applyResolvedWords(token, rows) }
    }

    // ---- the search box ----
    Rectangle {
        width: parent.width
        height: 42
        radius: Theme.radiusMd
        color: Theme.inset
        border.width: 1
        border.color: queryInput.activeFocus ? Theme.accent : Theme.border

        IconGlyph {
            id: glass
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            name: "search"
            color: Theme.textMuted
            width: 14
            height: 14
        }

        TextInput {
            id: queryInput
            anchors.left: glass.right
            anchors.leftMargin: 10
            // Room for the padlock at the field's far end (the exact-word lock).
            anchors.right: exactLock.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textMd
            clip: true
            selectByMouse: true
            onTextChanged: {
                root.query = text
                if (text.trim() === "") {
                    debounce.stop()
                    root.searchPending = false
                    root.searchToken++
                    root.resolveToken++
                    root.results = []
                    root.current = 0
                    hint.text = ""
                    root.highlightSource = ""
                } else {
                    root.searchPending = true
                    root.searchToken++
                    root.resolveToken++
                    root.results = []
                    debounce.restart()
                }
            }
            Keys.onDownPressed: root.move(1)
            Keys.onUpPressed: root.move(-1)
            Keys.onReturnPressed: root.activate()
            Keys.onEnterPressed: root.activate()
            Keys.onEscapePressed: root.close()
            Keys.onTabPressed: {
                if (hint.text === "") return
                // The words the hint was built from, when they have landed.
                if (root.resolvedWords.length > 0) {
                    root.fixQueryWith(root.resolvedWords)
                    return
                }
                // Still resolving: ask, and let the answer do the rewrite (see
                // applyResolvedWords). No synchronous scan on this thread.
                root.pendingTabFix = true
                root.resolveToken++
                SearchService.resolveWordsAsync(root.query, root.resolveToken)
            }

            Text {
                visible: queryInput.text.length === 0
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Search everything…  (try “John 3:16”)")
                color: Theme.textMuted
                font: queryInput.font
            }
        }

        Item {
            id: exactLock
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(22, exactLockLabel.implicitWidth + 30)
            height: 24
            Rectangle {
                anchors.fill: parent
                radius: 12
                // The locked chip is an ACCENT tint (never a literal purple): the
                // padlock reads in whatever accent Settings > General carries.
                color: exactLockHover.hovered ? (root.exact ? Qt.alpha(Theme.accent, 0.18) : Theme.hoverBg)
                                              : (root.exact ? Qt.alpha(Theme.accent, 0.12) : "transparent")
                border.width: root.exact ? 1 : 0
                border.color: Theme.accent
            }
            IconGlyph {
                id: exactLockIcon
                anchors.left: parent.left
                anchors.leftMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                name: root.exact ? "locked" : "unlocked"
                color: root.exact ? Theme.accent : Theme.textMuted
                width: 12; height: 12
                fit: true
            }
            Text {
                id: exactLockLabel
                anchors.left: exactLockIcon.right
                anchors.leftMargin: 5
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Exact")
                color: root.exact ? Theme.accent : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs + 1
                font.bold: root.exact
            }
            PositionHoverArea {
                id: exactLockHover
                objectName: "selfTestSearchExactLock"
                anchors.fill: parent
                checkAncestors: false
                onClicked: {
                    root.toggleExact()
                    queryInput.forceActiveFocus()
                }
            }
        }
    }

    Text {
        id: hint
        objectName: "selfTestSearchHint"   // (the self-test reads the gold spell-fix line)
        width: parent.width
        visible: text !== ""
        leftPadding: 4
        topPadding: 2
        color: Theme.highlightInk
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }

    // ---- results, grouped by kind (they scroll once they outgrow the window) ----
    Column {
        width: parent.width
        spacing: 2

        Item {
            id: listArea
            width: parent.width
            visible: root.results.length > 0
            height: visible ? Math.min(resultsCol.height, 340) : 0

            Flickable {
                id: flick
                anchors.fill: parent
                contentWidth: width
                contentHeight: resultsCol.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: resultsCol
                    width: flick.width - 10   // room for the scrollbar
                    spacing: 2

        Repeater {
            id: resultsRepeater
            model: root.results

            delegate: Column {
                id: entry
                // The hook the self-test hovers a specific row by (it reads the
                // preview off the item itself — see Main.qml's rank scenario).
                objectName: "selfTestQuickRow" + index
                required property var modelData
                required property int index
                readonly property var info: root.kindInfo[modelData.kind] || root.kindInfo["show"]
                readonly property bool firstOfKind: index === 0 || root.results[index - 1].kind !== modelData.kind
                readonly property bool selected: root.current === index
                readonly property bool sermonCodeResult: modelData.kind === "table"
                    && Number(modelData.verse) === 0
                readonly property bool isLocatedVerse: (modelData.kind === "bible" || modelData.kind === "table")
                                                        && modelData.bookId !== undefined && modelData.bookId !== ""
                                                        && !sermonCodeResult
                property string fullText: {
                    root.verseRevision
                    const seeded = String(modelData.fullText ?? "")
                    if (!rowHover.hovered || !isLocatedVerse)
                        return seeded
                    // A sermon paragraph (or the pair a spanned hit needs) comes from
                    // the library; a verse from SearchService.fullVerse.
                    if (modelData.kind === "table")
                        return TheTableService.paragraphPair(modelData.bookId, modelData.chapter,
                                                             modelData.verse,
                                                             modelData.spanned === true ? 2 : 1)
                    const asked = SearchService.fullVerse(modelData.bibleId, modelData.bookId,
                                                          modelData.chapter, modelData.verse)
                    return asked !== "" ? asked : seeded
                }
                width: parent.width

                // Group header
                Text {
                    visible: entry.firstOfKind
                    topPadding: index === 0 ? 2 : 10
                    bottomPadding: 4
                    leftPadding: 4
                    text: entry.info.group.toUpperCase()
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.bold: true
                    font.letterSpacing: 0.6
                }

                Rectangle {
                    width: parent.width
                    height: rowText.implicitHeight + 16
                    radius: Theme.radiusMd
                    color: entry.selected ? Qt.alpha(Theme.accent, 0.13) : "transparent"
                    border.width: entry.selected ? 1 : 0
                    border.color: Qt.alpha(Theme.accent, 0.33)

                    Rectangle {
                        id: badge
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.top: parent.top
                        anchors.topMargin: 10
                        width: 66
                        height: 18
                        radius: 4
                        color: Qt.rgba(Qt.color(entry.info.color).r, Qt.color(entry.info.color).g,
                                       Qt.color(entry.info.color).b, 0.16)
                        Text {
                            anchors.centerIn: parent
                            text: entry.info.badge
                            color: entry.info.color
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 0.5
                        }
                    }

                    Rectangle {
                        id: ratingPill
                        readonly property int percent: root.matchDisplay(entry.modelData)
                        readonly property color tone: percent >= 90 ? "#4ae0b0"
                                                     : (percent >= 60 ? "#e0b04a" : Theme.textSecondary)
                        visible: root.canHighlight && root.queryCoverage(entry.modelData) > 0
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.top: parent.top
                        anchors.topMargin: 10
                        width: ratingText.implicitWidth + 14
                        height: 18
                        radius: 4
                        color: Qt.rgba(tone.r, tone.g, tone.b, 0.16)
                        Text {
                            id: ratingText
                            anchors.centerIn: parent
                            text: qsTr("MATCH %1%").arg(ratingPill.percent)
                            color: ratingPill.tone
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 0.5
                        }
                    }

                    Column {
                        id: rowText
                        anchors.left: badge.right
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 12 + (ratingPill.visible ? ratingPill.width + 6 : 0)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        Text {
                            width: parent.width
                            text: entry.sermonCodeResult
                                  ? entry.modelData.subtitle
                                  : (root.canHighlight ? root.highlight(entry.modelData.title) : entry.modelData.title)
                            color: Theme.textPrimary
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm + 1
                            elide: Text.ElideRight
                            textFormat: root.canHighlight ? Text.StyledText : Text.PlainText
                        }
                        Text {
                            visible: !entry.sermonCodeResult && (entry.modelData.text !== "" || entry.fullText !== "")
                            width: parent.width
                            text: {
                                const source = rowHover.hovered && entry.fullText !== "" ? entry.fullText : entry.modelData.text
                                return root.canHighlight ? root.highlight(source) : source
                            }
                            color: "#aab2c4"
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textSm
                            wrapMode: Text.WordWrap
                            maximumLineCount: rowHover.hovered && entry.fullText !== "" ? 24 : 2
                            elide: Text.ElideRight
                            textFormat: root.canHighlight ? Text.StyledText : Text.PlainText
                        }
                        Text {
                            visible: !entry.sermonCodeResult
                            width: parent.width
                            text: entry.modelData.subtitle
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.textXs + 1
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                    }

                    PositionHoverArea {
                        id: rowHover
                        anchors.fill: parent
                        onHoveredChanged: {
                            if (hovered) root.current = entry.index
                        }
                        onClicked: root.choose(entry.modelData)
                    }
                }
            }
        }
                }   // resultsCol
            }   // Flickable

            AppScrollBar {
                x: parent.width - width
                height: parent.height
                flickable: flick
            }
        }   // listArea

        // Empty / hint states
        Text {
            visible: root.results.length === 0
            width: parent.width
            topPadding: 10
            bottomPadding: 6
            horizontalAlignment: Text.AlignHCenter
            text: root.query.trim() === ""
                  ? qsTr("Start typing to search. A Bible reference like “Romans 8:1” goes straight to the verse.")
                  : (root.searchPending
                     ? qsTr("Searching…")
                     : (root.exact
                     // Locked and nothing found: say why (the padlock is easy to
                     // forget), and how to get loose results back.
                     ? qsTr("No exact match for “%1” — every word must appear as a whole word in the same verse or sermon paragraph. Unlock it to search loosely.").arg(root.query.trim())
                     : (SearchService.bibleLoading
                        ? qsTr("No matches yet — the Bible is still loading…")
                        : qsTr("No results for “%1”.").arg(root.query.trim()))))
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
        }
        Text {
            visible: SearchService.bibleLoading && root.results.length > 0
            width: parent.width
            topPadding: 8
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Bible verses are still loading…")
            color: Theme.textMuted
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textXs + 1
        }
    }
}