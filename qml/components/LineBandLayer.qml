import QtQuick
import VGRPresenterUI

Item {
    id: root

    clip: true

    // The Text painting the glyphs. Everything below mirrors it.
    property var layout: null
    // A CSS colour; "" or "transparent" = no band.
    property string color: ""
    // Corner radius in the same px the label's box is measured in.
    property real radius: 0
    property string textCase: "none"

    readonly property bool active: root.layout !== null
                                   && root.color !== "" && root.color !== "transparent"
    readonly property bool rounded: root.wantsRounded && root.rows.length > 0

    readonly property string shownText: root.layout ? String(root.layout.text ?? "") : ""
    readonly property real padLeft: root.layout ? (root.layout.leftPadding ?? 0) : 0
    readonly property real padRight: root.layout ? (root.layout.rightPadding ?? 0) : 0
    readonly property real contentWidth: root.layout ? (root.layout.width - root.padLeft - root.padRight) : 0

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