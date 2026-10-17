import QtQuick

// THE CLOCK PANEL — the big time and the date that stand in the free right
// edge of a library pane (Overlays, Templates, Media).
//
// A card grid two or three rows deep leaves a wide, dead gutter on a
// full-size window, and an empty dark rectangle reads as a pane that failed to
// load. This is the same panel the earlier build put there: the time in the
// theme accent, the seconds and the AM/PM in the primary ink, the date under
// it in the muted one. Nothing here is a control; it is the pane's own
// furniture, so it carries no hover, no click, no focus.
//
// The ticking comes from LiveClock — the app's ONE shared second-by-second
// clock (the Clock/Timer pickers' previews and the canvas Clock/Timer items
// use it too), so this panel can never drift from the clock on the wall and it
// adds no second Timer of its own. `running` is wired to the host pane's
// visibility: a hidden pane's clock stops.
Item {
    id: root

    // Stops the tick when the pane is not on screen.
    property bool running: true
    // 12-hour with an AM/PM suffix (what the panel was asked for); a 24-hour
    // host just clears it.
    property bool hour12: true
    // The big time. Drops to a calmer size when the host asks for a narrow one.
    property int mainSize: 56

    implicitWidth: 330
    implicitHeight: column.implicitHeight
    // The panel is furniture: clicks and hovers belong to whatever is behind it.
    enabled: false

    LiveClock { id: ticker; running: root.running }

    function pad2(n) { return n < 10 ? "0" + n : String(n) }

    // Read as separate strings so the parts can be styled (and so each is a
    // plain property read at the call site — the shape LiveClock's own note
    // asks for).
    readonly property date now: ticker.now
    readonly property bool afternoon: now.getHours() >= 12
    readonly property int shownHour: {
        if (!root.hour12)
            return root.now.getHours()
        const h = root.now.getHours() % 12
        return h === 0 ? 12 : h
    }
    readonly property string hourMinute: pad2(root.shownHour) + ":" + pad2(root.now.getMinutes())
    readonly property string seconds: ":" + pad2(root.now.getSeconds())
    readonly property string meridiem: root.hour12 ? (root.afternoon ? " PM" : " AM") : ""
    // "Saturday 17. October 2026" — the app's own locale renders the names.
    readonly property string dateText: Qt.formatDate(root.now, "dddd d. MMMM yyyy")

    Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        // Centred on the panel's own box, so a host can centre this item
        // wherever it has room and get the whole block placed for it.
        anchors.verticalCenter: parent.verticalCenter
        spacing: 12

        // The time is one BASELINE row, not a Row: the seconds and the suffix
        // have to sit on the big digits' baseline, and a positioner aligns its
        // children by their tops.
        Item {
            anchors.horizontalCenter: parent.horizontalCenter
            width: timeRow.width
            height: timeRow.height

            Item {
                id: timeRow
                width: hourText.width + secondsText.width + meridiemText.width
                height: hourText.height

                Text {
                    id: hourText
                    text: root.hourMinute
                    color: Theme.accent
                    font.family: Theme.fontFamily
                    font.pixelSize: root.mainSize
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.5
                }
                Text {
                    id: secondsText
                    anchors.left: hourText.right
                    anchors.leftMargin: 4
                    anchors.baseline: hourText.baseline
                    text: root.seconds
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Math.round(root.mainSize * 0.4)
                    font.weight: Font.DemiBold
                }
                Text {
                    id: meridiemText
                    anchors.left: secondsText.right
                    anchors.leftMargin: 4
                    anchors.baseline: hourText.baseline
                    text: root.meridiem
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Math.round(root.mainSize * 0.26)
                    font.weight: Font.DemiBold
                }
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.dateText
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: 15
            font.weight: Font.DemiBold
            font.letterSpacing: 2
            opacity: 0.85
        }
    }
}
