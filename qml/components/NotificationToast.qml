import QtQuick
import VGRPresenterUI
import "."

// One toast card, rendered by NotificationOverlay.qml. Auto-dismisses after
// `duration` unless hovered — hovering holds the Timer at 0% (Timer resets
// its remaining time whenever `running` flips back to true, which here
// means "give the full duration again once you stop reading it," the same
// hover-grace idea as AppMenuBar's settingsCloseTimer).
Rectangle {
    id: root
    // Hook for the modal-fit scenario's toast step (qml/Main.qml's sweep): the
    // notice's own card, so the check can measure what the user actually sees.
    objectName: "selfTestToastCard"

    property string level: "info"
    property string title: ""
    property string message: ""
    // The NAMES this notice is about (a device, a source, a show…) — the crux the
    // eye should land on first. Rendered bold inside the message, the rest of the
    // sentence stays plain ("not the whole text, just the main crux"). A string
    // or a list of strings; producers that know their subject pass it (the boot
    // and missing-device toasts pass the device names). Messages that carry no
    // crux of their own still get the app's own convention emphasized — anything
    // the text QUOTES is a name too (see emphasised()).
    property var crux: []
    // ONE clickable action this notice offers — {"id": "...", "label": "Open it"}.
    // The id names work QML's own code registered with
    // NotificationCenter.registerAction (a payload is data, so it cannot carry a
    // function): the card's button does not know what the action IS, it just runs
    // it, which is what makes "the notice that tells you about the recovered show"
    // able to open it in one click instead of sending you to find it. Null on a
    // notice that offers nothing, where the card is exactly what it was before.
    property var action: null
    property int duration: 5000
    // How many identical notices this ONE card stands for (NotificationCenter
    // coalesces a repeat onto the card already on screen instead of stacking a
    // second copy), and a nonce that moves on every one of them.
    property int groupCount: 1
    property int nonce: 1

    signal dismissed()
    signal actionTriggered()

    // Is there a real action to draw? A label is what makes one visible, so an
    // action without one is treated as no action at all rather than an empty
    // button nobody can name.
    readonly property bool hasAction: root.action !== null
                                      && typeof root.action === "object"
                                      && String(root.action.label || "").length > 0

    // The pieces of `message` to draw bold. Two sources, in order: the names the
    // producer handed over, then every quoted span in the text itself — the
    // app's own way of naming a thing ("“New Bus 2” already carries…",
    // "…changes to \"Show\" were saved as \"Show (recovered)\""). Longer
    // first, so a name that contains a shorter one is bolded whole.
    function cruxSpans() {
        let spans = []
        const given = root.crux
        if (typeof given === "string") {
            if (given.length > 0)
                spans.push(given)
        } else if (given) {
            for (let i = 0; i < given.length; ++i)
                if (given[i])
                    spans.push(String(given[i]))
        }
        const quoted = /\u201C([^\u201D]{1,120})\u201D|"([^"]{1,120})"/g
        let m
        while ((m = quoted.exec(root.message)) !== null)
            spans.push(m[1] !== undefined ? m[1] : m[2])
        // Drop the empties and the repeats, longest first (see above).
        spans = spans.filter((s, i) => s.length > 0 && spans.indexOf(s) === i)
        spans.sort((a, b) => b.length - a.length)
        return spans
    }

    // HTML-escape: engine, device and file text must never be parsed as markup.
    // Only the <b> tags built below may reach the renderer.
    function escapeHtml(text) {
        return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;")
                           .replace(/>/g, "&gt;").replace(/"/g, "&quot;")
    }

    // The message as rich text: escaped ONCE, then each crux span wrapped in
    // <b>. Spans are matched longest-first and a match overlapping one already
    // claimed is skipped, so a name nested in a longer name ("How Deep" inside
    // "How Deep (recovered)") is bolded once, as part of the longer run —
    // replacing span by span produced <b><b>How Deep</b> (recovered)</b>.
    function emphasised() {
        const spans = root.cruxSpans()
        if (spans.length === 0)
            return ""
        const flat = root.escapeHtml(root.message)
        let runs = []
        for (let i = 0; i < spans.length; ++i) {
            const needle = root.escapeHtml(spans[i])
            let at = flat.indexOf(needle)
            while (at !== -1) {
                if (!runs.some(function (r) { return at < r.at + r.len && r.at < at + needle.length }))
                    runs.push({ at: at, len: needle.length })
                at = flat.indexOf(needle, at + 1)
            }
        }
        runs.sort(function (a, b) { return a.at - b.at })
        let html = ""
        let cursor = 0
        for (let i = 0; i < runs.length; ++i) {
            html += flat.slice(cursor, runs[i].at) + "<b>"
                    + flat.substr(runs[i].at, runs[i].len) + "</b>"
            cursor = runs[i].at + runs[i].len
        }
        return html + flat.slice(cursor)
    }
    readonly property bool hasEmphasis: root.cruxSpans().length > 0

    readonly property color accentColor: {
        switch (root.level) {
        case "error": return Theme.danger
        case "warning": return Theme.warning
        case "success": return Theme.success
        default: return Theme.info
        }
    }

    width: 320
    implicitHeight: contentColumn.implicitHeight + Theme.space4 * 2
    radius: Theme.radiusLg
    color: Theme.card
    border.width: 1
    border.color: Theme.border

    Rectangle {
        width: 3
        radius: Theme.radiusSm
        color: root.accentColor
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
    }

    Column {
        id: contentColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.space4 + Theme.space2
        anchors.rightMargin: Theme.space4
        spacing: Theme.space1

        Row {
            width: parent.width
            spacing: Theme.space2

            Rectangle {
                width: 8; height: 8; radius: 4
                color: root.accentColor
                anchors.verticalCenter: parent.verticalCenter
            }

            // "×6" for a notice that arrived six times: the coalescing is stated
            // rather than hidden, so a user who saw one toast can still tell that
            // the action kept failing.
            Rectangle {
                id: repeatBadge
                visible: root.groupCount > 1
                width: repeatText.implicitWidth + Theme.space2
                height: repeatText.implicitHeight + 2
                radius: height / 2
                color: Theme.chip
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    id: repeatText
                    anchors.centerIn: parent
                    text: "×" + root.groupCount
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                }
            }

            Text {
                text: root.title
                textFormat: Text.PlainText
                visible: root.title.length > 0
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
                width: parent.width - 8 - Theme.space2 * 2 - closeGlyph.width
                       - (repeatBadge.visible ? repeatBadge.width + Theme.space2 : 0)
                elide: Text.ElideRight
            }

            IconGlyph {
                id: closeGlyph
                name: "close"
                color: Theme.textMuted
                width: 10; height: 10
                anchors.verticalCenter: parent.verticalCenter
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.dismissed()
                }
            }
        }

        // The message, with its crux bold (see cruxSpans). A message with no crux
        // renders exactly as before — plain text, no markup parsing at all; only
        // a notice that HAS something to emphasize is drawn as the rich text this
        // card built itself (escaped first, so nothing in the text can inject
        // markup).
        Text {
            // Hook for the modal-fit scenario's toast step: the rendered message,
            // so the sweep can assert what the renderer was handed (see
            // reportToastCrux in qml/Main.qml).
            objectName: "selfTestToastMessage"
            text: root.hasEmphasis ? root.emphasised() : root.message
            textFormat: root.hasEmphasis ? Text.RichText : Text.PlainText
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            width: parent.width
            wrapMode: Text.WordWrap
        }

        // The offer, one click — only on a notice that carries one. A ghost
        // button (the app's own quiet variant) so an action never competes with
        // the message for attention, and it sits UNDER the text it belongs to.
        AppButton {
            objectName: "selfTestToastAction"
            visible: root.hasAction
            text: root.hasAction ? String(root.action.label) : ""
            variant: "ghost"
            horizontalPadding: Theme.space3
            onClicked: root.actionTriggered()
        }
    }

    Timer {
        id: autoTimer
        interval: root.duration
        running: !hoverArea.containsMouse
        onTriggered: root.dismissed()
    }

    // A repeat of this same notice refreshed this card instead of stacking a
    // second one, so the countdown starts over: the newest occurrence gets the
    // same reading time as the first. Not while the pointer is holding the card
    // open, where the timer is deliberately off (see the hover behaviour above).
    onNonceChanged: if (!hoverArea.containsMouse) autoTimer.restart()

    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }
}
