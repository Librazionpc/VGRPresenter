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

    property string level: "info"
    property string title: ""
    property string message: ""
    property int duration: 5000

    signal dismissed()

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

            Text {
                text: root.title
                textFormat: Text.PlainText
                visible: root.title.length > 0
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
                width: parent.width - 8 - Theme.space2 * 2 - closeGlyph.width
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

        Text {
            text: root.message
            textFormat: Text.PlainText   // engine/device text must never render as HTML
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.textSm
            width: parent.width
            wrapMode: Text.WordWrap
        }
    }

    Timer {
        interval: root.duration
        running: !hoverArea.containsMouse
        onTriggered: root.dismissed()
    }

    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }
}
