import QtQuick
import VGRPresenterUI

// THE FEED-HEALTH PILL — the title-row chip that says an output has stopped
// receiving the program feed, and says so again for a few seconds when it
// comes back.
//
// It is a SECOND pill beside Connected, never a change to it: the engine is
// still connected while an output stalls (that is exactly why a frozen picture
// is so misleading — nothing anywhere says "this is not live"). So the two
// read as separate facts in the same row: "Connected" is the engine, this is
// the feed.
//
// The text is LiveOutputService's own (feedNoticeText), so the numbering is
// the service's too: M<n> is the n-th ENABLED output in roster order, which is
// the same label the monitor wall puts on the tile. One stalled output names
// it ("M2 · No live feed"); several list them ("M1, M2 · No live feed"); when
// every output is stalled the sentence is still the same one, because "no live
// feed" is true either way — the names in front say how wide it is.
//
// The service owns the timing: the pill is driven entirely by bindings and
// disappears on its own when the notice expires, so nothing here polls.
Rectangle {
    id: root
    objectName: "selfTestFeedStallPill"

    readonly property string noticeText: LiveOutputService.feedNoticeText
    readonly property string kind: LiveOutputService.feedNoticeKind
    readonly property bool stalled: kind === "stalled"
    readonly property bool restored: kind === "restored"

    // A stalled feed is a fault (danger); a recovery is a reassurance
    // (success). Both are drawn on the header's own ground so the pill reads
    // as chrome, not as a dialog.
    readonly property color ink: stalled ? Theme.dangerLight
        : restored ? Theme.successLight : Theme.textSecondary
    readonly property color tint: stalled ? Qt.alpha(Theme.danger, 0.14)
        : restored ? Qt.alpha(Theme.success, 0.12) : "transparent"
    readonly property color edge: stalled ? Qt.alpha(Theme.danger, 0.45)
        : restored ? Qt.alpha(Theme.success, 0.40) : "transparent"

    // Short enough to sit beside Connected without ever crowding the tabs: the
    // longest realistic line ("M1, M2, M3, M4 · No live feed") still fits.
    readonly property int maxWidth: 260

    visible: noticeText.length > 0 && opacity > 0
    height: 22
    // Width comes from the CONTENT ROW's own implicit width, and the row is laid
    // out from a fixed left inset — never "centred inside a pill whose width comes
    // from the row", which is a binding loop Qt resolves by leaving the pill at
    // zero width and letting the text spill out of it (the first cut did exactly
    // that, and the pill's text ran off the header's right edge).
    width: Math.min(maxWidth, noticeRow.implicitWidth + 20)
    radius: 6
    color: tint
    border.width: 1
    border.color: edge

    opacity: 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    // Fades in on the first notice and out when the service clears it — a pill
    // that blinks in mid-service would read as an alarm; one that eases in
    // reads as a status light.
    onNoticeTextChanged: opacity = noticeText.length > 0 ? 1 : 0
    Component.onCompleted: opacity = noticeText.length > 0 ? 1 : 0

    Row {
        id: noticeRow
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7

        // The tell, small: a dot. It PULSES while stalled because a static red
        // dot among the header's other static dots is too easy to read as
        // decoration; a steady green one on recovery is a confirmation, so it
        // does not move.
        Item {
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: 8

            Rectangle {
                anchors.centerIn: parent
                width: 8; height: 8; radius: 4
                color: root.ink
                SequentialAnimation on opacity {
                    running: root.stalled
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.25; duration: 700 }
                    NumberAnimation { to: 1.0; duration: 700 }
                }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.noticeText
            color: root.ink
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.DemiBold
            textFormat: Text.PlainText
            elide: Text.ElideRight
            width: Math.min(implicitWidth, root.maxWidth - 36)
        }
    }
}
