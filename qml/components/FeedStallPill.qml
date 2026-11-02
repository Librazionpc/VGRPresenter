import QtQuick
import VGRPresenterUI

Rectangle {
    id: root
    objectName: "selfTestFeedStallPill"

    readonly property string noticeText: LiveOutputService.feedNoticeText
    readonly property string kind: LiveOutputService.feedNoticeKind
    readonly property bool stalled: kind === "stalled"
    readonly property bool restored: kind === "restored"

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
    width: Math.min(maxWidth, noticeRow.implicitWidth + 20)
    radius: 6
    color: tint
    border.width: 1
    border.color: edge

    opacity: 0
    Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    onNoticeTextChanged: opacity = noticeText.length > 0 ? 1 : 0
    Component.onCompleted: opacity = noticeText.length > 0 ? 1 : 0

    Row {
        id: noticeRow
        x: 10
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7

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