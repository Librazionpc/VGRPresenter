import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    // ---- Header ----
    property string title: ""
    // Any IconGlyph name; "" draws no icon (a headerless-free but titled card,
    // e.g. the Filters card's own rows).
    property string icon: ""
    property color iconColor: Theme.textMuted
    // FreeShow's "section.alwaysOpen": no title bar, no chevron, cannot collapse.
    property bool alwaysOpen: false

    // ---- Collapse state ----
    // Owned by the consumer when it wants to drive it (or just left alone: the
    // header click writes to it, which is the normal case).
    property bool expanded: true

    property bool hasChanges: false
    property string resetText: qsTr("Reset")

    signal resetRequested()

    // Where content goes: plain children become rows of the body, stacked with
    // `rowSpacing` inside the card's own gutter.
    default property alias content: contentColumn.data

    // FreeShow's `.content section { padding: 10px }`, widened to this app's
    // 14px card gutter so a row's label lines up with a card header's label.
    readonly property int gutter: 14
    readonly property int rowSpacing: 14
    readonly property int headerHeight: 38

    readonly property int bodyContentHeight: contentColumn.implicitHeight + root.gutter * 2
    readonly property bool bodyShown: root.alwaysOpen || root.expanded

    implicitWidth: 260
    implicitHeight: headerArea.height + bodyArea.height

    radius: 9
    color: Theme.card
    border.width: 1
    border.color: Theme.borderSubtle
    clip: true

    onHasChangesChanged: {
        if (root.hasChanges && !root.alwaysOpen && !root.expanded)
            root.expanded = true
    }

    // ---- Header ----
    Rectangle {
        id: headerArea
        x: 1
        y: 1
        width: parent.width - 2
        height: root.alwaysOpen ? 0 : root.headerHeight
        visible: !root.alwaysOpen
        color: headerHit.containsMouse && !root.alwaysOpen
            ? Theme.chip
            : (Theme.dark ? Theme.panelBg : Theme.inset)
        radius: 8
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

        // Square off the header's bottom corners: it is the top of one card, so
        // only the top two corners should read as rounded.
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: parent.radius
            color: headerArea.color
        }
        // FreeShow's `border-bottom: 1px solid var(--primary-lighter)`.
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.borderSubtle
        }

        MouseArea {
            id: headerHit
            anchors.fill: parent
            hoverEnabled: !root.alwaysOpen
            cursorShape: root.alwaysOpen ? Qt.ArrowCursor : Qt.PointingHandCursor
            onClicked: root.expanded = !root.expanded
        }

        Row {
            x: root.gutter
            anchors.verticalCenter: parent.verticalCenter
            spacing: 9

            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.icon !== ""
                name: root.icon
                color: root.iconColor
                fit: true
                width: 14
                height: 14
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.title
                // 0.8rem / opacity 0.8 in FreeShow's stylesheet — the title is a
                // label, not a heading, so it never competes with a row's value.
                color: Theme.textPrimary
                opacity: 0.85
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Medium
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Rectangle {
                visible: root.hasChanges
                width: resetLabel.implicitWidth + 16
                height: 20
                radius: 10
                color: resetHit.containsMouse ? Theme.hoverBg : Theme.inset
                border.width: 1
                border.color: Theme.borderSubtle
                Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

                Text {
                    id: resetLabel
                    anchors.centerIn: parent
                    text: root.resetText
                    color: Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }

                MouseArea {
                    id: resetHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.resetRequested()
                }
            }

            IconGlyph {
                anchors.verticalCenter: parent.verticalCenter
                // One glyph, rotated — the change of direction is visible while
                // it turns (FreeShow swaps between two SVGs instead).
                name: "chevronDown"
                color: Theme.textMuted
                width: 11
                height: 11
                rotation: root.expanded ? 180 : 0
                Behavior on rotation { NumberAnimation { duration: 130; easing.type: Easing.OutQuad } }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.expanded = !root.expanded
                }
            }
        }
    }

    // ---- Body ----
    Item {
        id: bodyArea
        anchors.top: headerArea.bottom
        width: parent.width
        height: root.bodyShown ? root.bodyContentHeight : 0
        visible: height > 0
        clip: true

        Column {
            id: contentColumn
            x: root.gutter
            y: root.gutter
            width: parent.width - root.gutter * 2
            spacing: root.rowSpacing
        }
    }
}