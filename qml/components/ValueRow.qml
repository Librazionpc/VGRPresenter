import QtQuick
import VGRPresenterUI

// A read-out of the current choice with its actions at the end — the "Template: Table
// [None] [Change]" / "Background: Transparent [Change]" row of the Styles dialog and any
// form that picks something through a separate picker. Same inset surface, border and
// height as SelectField's box, so a dropdown and a picker row sit in one rhythm.
//
//   ValueRow {
//       text: qsTr("Transparent"); dim: true
//       swatchColor: "transparent"            // leading 22px swatch (omit for none)
//       PillButton { text: qsTr("Change"); onClicked: ... }   // the actions
//   }
Rectangle {
    id: root

    property string text: ""
    // A "nothing chosen" value reads quieter than a real one.
    property bool dim: false
    // Leading swatch: a colour, or an image (wins when set). `hasSwatch` turns it on.
    property bool hasSwatch: false
    property color swatchColor: "transparent"
    property url swatchImage: ""

    default property alias actions: actionRow.data

    implicitHeight: 46
    radius: Theme.radiusMd
    color: Theme.inset
    border.width: 1
    border.color: Theme.borderSubtle

    Rectangle {
        id: swatch
        visible: root.hasSwatch
        x: 14
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 24
        radius: 7
        clip: true
        color: root.swatchColor
        border.width: 1
        border.color: Theme.border

        Image {
            anchors.fill: parent
            visible: root.swatchImage.toString() !== ""
            source: root.swatchImage
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
        }
    }

    Text {
        x: root.hasSwatch ? swatch.x + swatch.width + 12 : 14
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, actionRow.x - x - 12)
        text: root.text
        elide: Text.ElideRight
        color: root.dim ? Theme.textMuted : Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
    }

    Row {
        id: actionRow
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
    }
}
