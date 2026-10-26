import QtQuick
import VGRPresenterUI

// THE PER-LINE BACKGROUND BAND (FreeShow's `lineBg` + `lineRadius`) — the
// highlight drawn behind EACH line of a text block, following the wrapping, as
// opposed to the item's own background (one fill behind the whole box).
//
// TWO PAINTING MODES, both honouring the same colour and the same layout:
//
//   square (radius 0) — the band is painted by a TRANSPARENT rich-text copy of
//     the same text: Qt paints a span's background per line FRAGMENT, so the
//     bands wrap exactly as the glyphs do with no second wrap engine involved.
//     This is the default and the cheapest path.
//
//   rounded (radius > 0) — rich text cannot round a span's background, so each
//     line becomes a Rectangle. Its geometry comes from the label's OWN layout
//     (contentHeight / lineCount give the line step) and from a greedy wrap
//     measured with the same font through TextMetrics; the numbers were checked
//     against Qt's own band painting in a standalone render before this file
//     existed (band height = the font's natural line box, band top =
//     (step − lineBox) / 2 into the line's step).
//
// The caller points `layout` at the very Text that paints the glyphs, so the
// band can never disagree with the text about font, spacing, wrapping,
// alignment or the box it fills.
Item {
    id: root

    clip: true

    // The Text painting the glyphs. Everything below mirrors it.
    property var layout: null
    // A CSS colour; "" or "transparent" = no band.
    property string color: ""
    // Corner radius in the same px the label's box is measured in.
    property real radius: 0
    // The typed text case, applied to the band's own string: the label draws
    // its case through font.capitalization, which rich text ignores, and an
    // upper-cased verse would otherwise band at lower-case widths.
    property string textCase: "none"

    readonly property bool active: root.layout !== null
                                   && root.color !== "" && root.color !== "transparent"
    // Rounded needs LINE BOXES as well as a radius: if the measurer cannot be
    // built (or the box has no width yet) `rows` is empty, and the band falls
    // back to the rich-text path — square corners, still a band. A radius is a
    // shape choice, never a reason for the highlight to disappear.
    readonly property bool rounded: root.wantsRounded && root.rows.length > 0

    readonly property string shownText: root.layout ? String(root.layout.text ?? "") : ""
    readonly property real padLeft: root.layout ? (root.layout.leftPadding ?? 0) : 0
    readonly property real padRight: root.layout ? (root.layout.rightPadding ?? 0) : 0
    readonly property real contentWidth: root.layout ? (root.layout.width - root.padLeft - root.padRight) : 0

    // One line BOX — the font's natural height at the drawn size. The probe is a
    // Text rather than TextMetrics because only a Text reports what Qt's layout
    // actually uses (TextMetrics exposes no ascent/descent).
    readonly property real lineBox: probe.contentHeight
    readonly property real lineStep: (root.layout && root.layout.lineCount > 0)
                                     ? root.layout.contentHeight / root.layout.lineCount : root.lineBox
    readonly property real firstLineTop: {
        if (!root.layout)
            return 0
        const v = root.layout.verticalAlignment
        if (v === Text.AlignBottom)
            return root.layout.height - root.layout.contentHeight
        if (v === Text.AlignVCenter)
            return (root.layout.height - root.layout.contentHeight) / 2
        return 0
    }
    // `wantsRounded` is the REQUEST (a radius with a band); `rounded` is what can
    // actually be drawn (see below). Keeping them apart matters: asking `rows`
    // whether it is rounded while `rounded` asks `rows` its length is a binding
    // loop.
    readonly property bool wantsRounded: root.active && root.radius > 0
    readonly property var rows: (root.wantsRounded && root.contentWidth > 0)
        ? MetadataFields.lineBoxes(root.shownText, root.contentWidth, root.layout.font, root.textCase)
        : []

    // The square band: Qt's own per-fragment span backgrounds.
    Text {
        id: bandText
        anchors.fill: parent
        visible: root.active && !root.rounded
        textFormat: Text.RichText
        // Transparent glyphs, visible bands: only the span backgrounds paint.
        color: "transparent"
        font: root.layout ? root.layout.font : Qt.font({ family: Theme.fontFamily, pixelSize: 16 })
        lineHeight: root.layout ? root.layout.lineHeight : 1
        lineHeightMode: Text.ProportionalHeight
        wrapMode: root.layout ? root.layout.wrapMode : Text.WordWrap
        horizontalAlignment: root.layout ? root.layout.horizontalAlignment : Text.AlignLeft
        verticalAlignment: root.layout ? root.layout.verticalAlignment : Text.AlignTop
        leftPadding: root.padLeft
        rightPadding: root.padRight
        text: visible ? MetadataFields.lineBandText(root.shownText, root.color, root.textCase) : ""
    }

    // The measured line box (see `lineBox`).
    Text {
        id: probe
        visible: false
        text: "Xg"
        font: bandText.font
        lineHeight: 1
        lineHeightMode: Text.ProportionalHeight
    }

    // The rounded band: one Rect per line, placed from the label's own step.
    Repeater {
        model: root.rows
        delegate: Rectangle {
            required property var modelData
            required property int index

            visible: root.rounded
            radius: root.radius
            color: root.color
            width: modelData.width
            height: root.lineBox
            y: root.firstLineTop + index * root.lineStep + (root.lineStep - root.lineBox) / 2
            x: {
                if (!root.layout)
                    return 0
                const a = root.layout.horizontalAlignment
                if (a === Text.AlignRight)
                    return root.padLeft + root.contentWidth - modelData.width
                if (a === Text.AlignLeft || a === Text.AlignJustify)
                    return root.padLeft
                return root.padLeft + (root.contentWidth - modelData.width) / 2
            }
        }
    }
}
