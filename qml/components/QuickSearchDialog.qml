import QtQuick
import VGRPresenterUI

// The app's one search box. The header's Search button, Ctrl+K, the Projects
// panel's "Quick search" and the menu item all open this, and it asks the engine
// (SearchService) for shows, slides, templates, overlays, categories, settings,
// songs and Bible verses — type "John 3:16" to jump straight to a verse.
//
// Up / Down move through the results, Enter (or a click) picks one; the host
// decides what picking means (resultChosen carries the whole result:
// kind, title, subtitle, text, id, path, key).
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
    // EXACT WORDS (the padlock at the field's far end — user call: "add a pad
    // lock icon at the extreme end of the search so once it lock it means i want
    // exact word search for the both word search"). Locked, every typed word
    // must appear VERBATIM as a whole word: no completion ("word" stops finding
    // "words"), no inner substring, no typo correction — and the engine's
    // resolved-words hint (which offers those corrections) stays quiet, since
    // accepting it would contradict the lock. Kept across opens: the mode is the
    // user's, not the query's.
    property bool exact: false
    // What the engine actually searched for ("frend" -> "friend"): the highlight
    // must wrap the CORRECTED words or a typo query lights nothing (the snippet
    // says "friend", the raw query said "frend"). Falls back to the live query.
    property string highlightSource: query
    property bool searchPending: false
    // Snippets are engine-anchored on the first query-word occurrence, so the
    // highlighted words are inside the visible text — not past the 2-line cap
    // where they only appear on hover.
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
        "category": { group: qsTr("Categories"), badge: qsTr("CATEGORY"), color: "#8a94a6" },
        "setting":  { group: qsTr("Settings"),   badge: qsTr("SETTING"),  color: "#8a94a6" },
        "song":     { group: qsTr("Songs"),      badge: qsTr("SONG"),     color: "#e06c8a" },
        "table":    { group: qsTr("Sermons"),    badge: qsTr("SERMON"),   color: "#4ae0b0" },
        "media":    { group: qsTr("Media"),      badge: qsTr("MEDIA"),    color: "#e08a4a" }
    })

    function openSearch() {
        // Every open starts FRESH (user call, after the restore-on-reopen try:
        // "the search should close since im using the other search" — picking a
        // result hands over to The Table/Scripture, and a reopening dialog that
        // still shows the old query gets in the way. Tried restore+select-all
        // 2026-09-24; user rejected it the same day — do not reintroduce).
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
    // What the user CHOSE, keyed kind+id (a stable identity: a path for shows,
    // a reference for a verse, book+chapter+paragraph for a sermon), most recent
    // first, persisted in the session settings so it survives restarts. On a new
    // search, a result the user picked before floats to the TOP of its kind group —
    // the other results still show below (nothing is hidden, they just rank after
    // the proven pick).
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
    // Which KIND carries the query as a NEAR-VERBATIM PHRASE (the typed words in
    // order, close together) in one of its rows. Comparable across kinds without
    // pretending the Bible's and The Table's scores share a scale — and it is the
    // signal the user named: a sentence The Table has word-for-word must not sit
    // below a kind that merely has the words scattered around ("this exist in the
    // table but it showing bible at the top when the table has a better
    // percentage"). PUNCTUATION never counts as part of the match, for exact and
    // loose search alike (user call: "punctuation [is] not counted the same for
    // normal search as well") — "compulsory, that" and "compulsory that" are the
    // same phrase. UP TO 2 extra words may sit between each typed word too (user
    // call: "because i missed some words in between ... if it can find [a]
    // similar sentence['s] exact words" — a real connector the typed query itself
    // skipped, like "and" in "compelling AND compulsory, that", must not make an
    // otherwise near-exact sentence read as mere scattered words): bounded so a
    // genuinely scattered match (the words anywhere in a long paragraph) still
    // does not count as "the sentence".
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
    // HOW MUCH OF THE QUERY a single row carries: the count of the typed words
    // that appear in it WHOLE. The cross-kind order below needs a signal that is
    // comparable between a verse, a sermon paragraph and a show name, and this is
    // it — deliberately not the engines' own scores (a Bible score and a sermon
    // score are not the same scale) and deliberately not the kind groups (which
    // said a verse carrying only "you that" outranks the sermon that says
    // "compelling and compulsory, that you receive": the user's report, twice).
    function queryCoverage(result) {
        const words = root.highlightSource.toLowerCase().match(/[A-Za-z0-9]+/g) || []
        if (words.length === 0) return 0
        const source = [result.title, result.subtitle, result.text, result.fullText]
            .map((v) => String(v ?? "")).join(" ")
        if (source === "") return 0
        let carried = 0
        for (let i = 0; i < words.length; ++i) {
            const safe = words[i].replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
            // The LAST word is the one under the caret: it counts at a WORD START,
            // not only whole — the same prefix rule the engine matched with — so a
            // row that says "received" for a typed "rec" is not billed as missing
            // the word it plainly carries.
            const re = new RegExp(i + 1 === words.length ? "\\b" + safe : "\\b" + safe + "\\b", "i")
            if (re.test(source)) carried++
        }
        return carried
    }
    // How tightly a row's covered words sit together (an afterthought on top of
    // queryCoverage, ordered 0 = the exact sentence, 1 = nearly exact, 2 = the
    // words merely scattered, 3 = the row does not carry them all). Deliberately
    // worth LESS THAN ONE WORD, so adjacency can only order rows that carry the
    // SAME number of words — it can never lift a row that is missing a typed
    // word above one that has them all.
    readonly property var adjacencyBonus: [0.90, 0.55, 0.20, 0.0]
    // A ROW'S MATCH RATING: the percentage of the typed query the row itself
    // carries — the ranking key, shown on every row so the order reads instead of
    // having to be taken on trust (user call, looking at "compelling that you
    // recieve" listing verses that carry only "you that": "why is showing me that
    // when there is a better one with the word compelling in there").
    // Whole-word coverage over the row's own text, plus a sub-word adjacency
    // bonus; see rankByPicks — the same two terms, so a higher rating always
    // means a higher rank.
    function matchRating(result) {
        const words = root.highlightSource.toLowerCase().match(/[A-Za-z0-9]+/g) || []
        if (words.length === 0)
            return 0
        const carried = root.queryCoverage(result)
        const bonus = root.adjacencyBonus[root.queryMatchRank(result)] ?? 0
        return Math.min(100, Math.round(100 * (carried + bonus) / words.length))
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
        // The padlock keeps exact whole words, but allows other words between
        // them. Service-side ranking puts nearby/orderly matches first; this is
        // the final guard against broad index candidates.
        if (root.exact)
            results = results.filter((r) => (r.kind === "bible" || r.kind === "table")
                                            ? root.isExactWordsMatch(r) : true)
        if (results.length < 2)
            return results
        // QML's Array.sort is NOT stable: a comparator returning 0 for
        // different kinds let equal-key swaps SCRAMBLE the groups (sermons
        // ranked above the Bible reference row). The engine's group order
        // (first appearance) is authoritative — compare group index first,
        // pick rank second, so picks float WITHIN their group only.
        const groupOf = {}
        for (const r of results)
            if (groupOf[r.kind] === undefined) groupOf[r.kind] = Object.keys(groupOf).length
        // Within each kind group the ENGINE SCORE ranks the rows when the
        // service provides one (table rows do); a previously-picked row floats
        // only within a score tie, then the engine's own order stands. The old
        // pick-over-everything rule let one stale click bury the verbatim
        // "Then, friends" paragraph under ¶s the engine scored far lower —
        // the user saw the wrong passage lead and called the scoring faulty.
        // Rows without a score (bible etc.) keep pick-then-engine order.
        const MISSING = -1
        const scoreOf = (r) => (typeof r.score === "number" ? r.score : null)
        const rankOf = (r) => {
            const key = pickKeyOf(r)
            const i = root.picks.findIndex((p) => p.key === key)
            return i < 0 ? MISSING : i   // unpicked = after every pick
        }
        return results.slice().sort((a, b) => {
            // RELEVANCE FIRST: the rating shown on the row — how much of the
            // typed query it carries — decides the order across EVERY kind, so
            // the column of percentages never goes up as you read down. This is
            // the fix for the report this dialog's ranking kept failing: verses
            // carrying only "you that" sat above the sermon that says
            // "compelling and compulsory, that you receive".
            const ra = root.matchRating(a), rb = root.matchRating(b)
            if (ra !== rb) return rb - ra
            // Equal rating: the tighter phrase leads (the exact sentence before
            // a nearly-exact one before the words merely scattered).
            const ma = root.queryMatchRank(a), mb = root.queryMatchRank(b)
            if (ma !== mb) return ma - mb
            // Same relevance: keep the engine's kind groups together, in the
            // order the service found them (the Bible reference row, then its
            // verses, then the sermons, ...).
            const ga = groupOf[a.kind], gb = groupOf[b.kind]
            if (ga !== gb) return ga - gb
            // Within a kind the ENGINE SCORE ranks the rows when the service
            // provides one (table rows do); a previously-picked row floats only
            // within a score tie, then the engine's own order stands. The old
            // pick-over-everything rule let one stale click bury the verbatim
            // "Then, friends" paragraph under ¶s the engine scored far lower —
            // the user saw the wrong passage lead and called the scoring faulty.
            // Rows without a score (bible etc.) keep pick-then-engine order.
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
    function applyResolvedWords(token, words) {
        if (token !== root.resolveToken || root.query.trim() === "")
            return
        // The hint reads the corrections AND the half-typed last word's candidate
        // ladder (most confident first): "recieve -> receive" as before, and
        // "rec -> receive \u00b7 received \u00b7 record" while the word is still being
        // typed — several readings, the likeliest named first (user call).
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
        // OFF the GUI thread: a big query ("the", ~1,200 candidates) blocked
        // typing for hundreds of ms synchronously. Rows come back via
        // resultsReady; the empty query path clears inline as before.
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
            SearchService.searchAsync(root.query, 4, root.searchToken, root.exact)
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
        console.log("[QS] choose:", result.kind, result.title)
        root.rememberPick(result)
        root.searchPending = false
        root.searchToken++
        root.resolveToken++
        root.close()
        // Let the row's click finish and the modal close settle before the host
        // switches tabs and opens a pane. Doing both synchronously from the
        // dialog's own MouseArea could tear down the active delegate mid-event.
        Qt.callLater(function() { root.resultChosen(result) })
    }

    // Enter: the highlighted result (the query is run first if the debounce hasn't fired).
    function activate() {
        if (debounce.running) { debounce.stop(); root.refresh() }
        if (root.results.length > 0) root.choose(root.results[Math.min(root.current, root.results.length - 1)])
    }

    // ---- yellow highlight of the typed words in a result's text -------------------
    // The query's words (2+ chars — the same floor the search itself uses) are
    // wrapped in <b><span style="background-color:#7a5c00">…</span></b>. The text
    // MUST go through StyledText for that to render, so &<> and the needle words
    // are escaped first — otherwise a verse containing "<" would swallow markup.
    // Case-insensitive, every occurrence, on both the snippet and the hover preview.
    function highlight(text) {
        const source = String(text ?? "")
        if (source === "")
            return ""
        const words = root.highlightSource.trim().toLowerCase().split(/\s+/).filter((w) => w.length >= 2)
        if (words.length === 0)
            return source
        const esc = (s) => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        const safeWords = words.map((w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
        const re = new RegExp("(" + safeWords.join("|") + ")", "gi")
        // Unmissable: Qt's StyledText subset does NOT honor background-color on a
        // span (the first attempt: bold applied, the yellow silently dropped —
        // "it still showing white"). A colored <font> IS honored: bright gold on
        // the dark card reads as the yellow highlight.
        return esc(source).replace(re, '<b><font color="#ffd54a">$1</font></b>')
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
    // A Bible finishing its background load makes new results possible.
    Connections {
        target: SearchService
        function onBibleChanged() { if (root.shown && root.query !== "") root.refresh() }
        // The async search's rows: applied only when this request is still the
        // latest (a fast typist's older keystrokes answer into the void).
        function onResultsReady(token, rows) {
            if (token !== root.searchToken || !root.shown) return
            root.searchPending = false
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
                // Accept the engine's resolved words: "thn frend" + Tab -> "then
                // friends". Only when a hint is showing (a well-spelled query has
                // nothing to fix, and Tab keeps its normal focus behavior).
                if (hint.text === "") return
                const words = SearchService.resolveWords(root.query)
                if (words.length === 0) return
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

            Text {
                visible: queryInput.text.length === 0
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Search everything…  (try “John 3:16”)")
                color: Theme.textMuted
                font: queryInput.font
            }
        }

        // The exact-words padlock, at the extreme end of the search field (user
        // call). Open (dim) = the search's normal fuzzy matching; closed (accent)
        // = every typed word must appear verbatim as a whole word. Text on the
        // state so the meaning is not left to the icon alone.
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
                // The self-test names the CLICK-SINK itself (not the wrapper Item):
                // it is the object that knows whether the pointer is genuinely over
                // the control, and the one a synthetic click has to reach.
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

    // The engine's resolved-words hint: "thn frend → then friends", gold like the
    // match highlight. Only exists when a word needed fixing; Tab accepts it.
    // (No height binding — a Column skips invisible items, and height-from-
    // implicitHeight here was the binding loop Qt reported.)
    Text {
        id: hint
        objectName: "selfTestSearchHint"   // (the self-test reads the gold spell-fix line)
        width: parent.width
        visible: text !== ""
        leftPadding: 4
        topPadding: 2
        color: "#ffd54a"
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
                required property var modelData
                required property int index
                readonly property var info: root.kindInfo[modelData.kind] || root.kindInfo["show"]
                readonly property bool firstOfKind: index === 0 || root.results[index - 1].kind !== modelData.kind
                readonly property bool selected: root.current === index
                // A sermon-code hit names the whole sermon (verse 0). Its
                // citation already contains both the code and title; paragraph
                // snippets and hover fetches add noise and can stall the UI on
                // unusually large opening paragraphs.
                readonly property bool sermonCodeResult: modelData.kind === "table"
                    && Number(modelData.verse) === 0
                // A Bible verse or sermon paragraph with a known location: hovering
                // fetches the FULL text (the row normally carries only a capped
                // snippet; a spread-across-two-paragraphs sermon match needs BOTH
                // paragraphs) and expands to show it — the same hover-preview the
                // The Table pane's match rows give.
                readonly property bool isLocatedVerse: (modelData.kind === "bible" || modelData.kind === "table")
                                                        && modelData.bookId !== undefined && modelData.bookId !== ""
                                                        && !sermonCodeResult
                // SearchService already supplies the full matched passage for
                // Bible and sermon results. Reuse it here: fetching a sermon
                // chapter synchronously on hover can block the UI behind an
                // in-flight library search and make a result click look like a
                // crash.
                property string fullText: String(modelData.fullText ?? "")
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

                    // The row's MATCH RATING: the percentage of the typed query this
                    // row itself carries — the very key the list is ordered by (see
                    // matchRating), so it explains the order instead of asking the
                    // reader to trust it. Rows that carry none of the query (a kind
                    // match, a code hit) show no pill rather than a "0%".
                    Rectangle {
                        id: ratingPill
                        readonly property int percent: root.matchRating(entry.modelData)
                        readonly property color tone: percent >= 90 ? "#4ae0b0"
                                                     : (percent >= 60 ? "#e0b04a" : "#8a94a6")
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
                        // A verse's text (the reference's verses, or the words that matched).
                        // Hovered located verse: the FULL text replaces the snippet and the
                        // two-line cap lifts — the verse reads whole, in place. The query's
                        // words are highlighted yellow in both (highlight() escapes the
                        // text, so StyledText can't be abused by verse content).
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
                            // Hover-preview: fetch once, keep it cached. Bible verses
                            // through SearchService.fullVerse; sermon paragraphs through
                            // TheTableService.paragraphPair (paragraph + the next one).
                            if (hovered && entry.isLocatedVerse && entry.fullText === "") {
                                if (entry.modelData.kind === "table")
                                    // A spanned match (words across two paragraphs) previews
                                    // BOTH; a normal hit previews its one paragraph.
                                    entry.fullText = TheTableService.paragraphPair(entry.modelData.bookId,
                                                                                   entry.modelData.chapter,
                                                                                   entry.modelData.verse,
                                                                                   entry.modelData.spanned === true ? 2 : 1)
                                else
                                    entry.fullText = SearchService.fullVerse(entry.modelData.bibleId,
                                                                             entry.modelData.bookId,
                                                                             entry.modelData.chapter,
                                                                             entry.modelData.verse)
                            }
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
