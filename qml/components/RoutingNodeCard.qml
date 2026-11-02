pragma ComponentBehavior: Bound

import QtQuick
import VGRPresenterUI

Rectangle {
    id: root

    property string nodeKey: ""
    property string title: ""
    // The second line: a bus's signal type, a screen's kind + resolution, a
    // sink's accepted signals.
    property string subtitle: ""
    property string iconName: "layers"
    // A colour swatch instead of the glyph (a style's own background colour).
    // Fully transparent = draw the glyph.
    property color swatch: "transparent"
    // The node's own signal colour — the type pill and the lit border.
    property color accent: Theme.accent
    // Short typed-signal tag: "A", "V" or "A+V". Empty = none.
    property string badge: ""
    // A short state on the subtitle line: "No signal", "2 screens", "silent".
    property string status: ""
    property color statusColor: Theme.textMuted
    property bool statusBadge: false
    // A connect-drag is live and this card is / is not a legal target.
    property bool dropValid: false
    property bool dropInvalid: false
    property string actionLabel: ""
    property bool actionActive: false

    property bool gateChips: false
    property bool overlayOn: true
    property bool mediaOn: true
    property bool enableToggle: false
    property bool outputEnabled: true

    readonly property bool hovered: hover.hovered

    signal hoverChanged(string key, bool isHovered)
    signal contextRequested(string key, var card, real localX, real localY)
    signal activated(string key)
    // The transport pill was pressed. The canvas decides what it MEANS per layer
    // (a REC / STREAM card starts or stops a take on its own node).
    signal actionTriggered(string key)
    // A gate chip was pressed. "overlay" or "media"; the canvas flips that row's
    // flag (OutputListModel) and the chip fades to its new state.
    signal gateToggled(string key, string gate)
    signal outputEnabledToggled(string key)

    implicitHeight: 64
    radius: Theme.radiusLg
    color: Theme.card
    border.width: 1
    border.color: root.dropValid ? root.accent
                : root.hovered ? Theme.borderSubtle
                : Theme.border
    // The design's own feedback for a typed edge: while a drag is live, a card
    // that cannot take it steps back instead of refusing silently on release.
    opacity: root.dropInvalid ? 0.32 : 1

    Behavior on opacity { NumberAnimation { duration: 110 } }
    Behavior on border.color { ColorAnimation { duration: 110 } }

    HoverGlow {
        anchors.fill: parent
        radius: parent.radius
        hovered: root.hovered
        edge: false
    }

    HoverHandler {
        id: hover
        onHoveredChanged: root.hoverChanged(root.nodeKey, hover.hovered)
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: root.activated(root.nodeKey)
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        cursorShape: Qt.ArrowCursor
        onClicked: (mouse) => root.contextRequested(root.nodeKey, root, mouse.x, mouse.y)
    }

    Row {
        anchors.fill: parent
        // The card's edge is where the port dots sit (see the canvas), so the
        // content stops short of both edges.
        anchors.leftMargin: 12
        anchors.rightMargin: 14
        anchors.topMargin: 9
        anchors.bottomMargin: 9
        spacing: 9

        Rectangle {
            id: chip
            width: 26
            height: 26
            radius: Theme.radiusSm
            anchors.verticalCenter: parent.verticalCenter
            color: root.swatch.a > 0.001 ? root.swatch : Qt.alpha(root.accent, 0.14)
            border.width: root.swatch.a > 0.001 ? 1 : 0
            border.color: Theme.border

            Item {
                anchors.fill: parent
                visible: root.swatch.a <= 0.001 && !root.enableToggle
                IconGlyph {
                    anchors.centerIn: parent
                    width: 13
                    height: 13
                    fit: true
                    name: root.iconName
                    color: root.accent
                }
            }

            StateIconButton {
                anchors.fill: parent
                visible: root.enableToggle
                iconName: root.iconName
                iconColor: root.accent
                checked: root.outputEnabled
                enabledTitle: qsTr("Disable output")
                disabledTitle: qsTr("Enable output")
                enabledDetail: qsTr("Click to stop this output")
                disabledDetail: qsTr("Click to resume this output")
                onClicked: root.outputEnabledToggled(root.nodeKey)
            }
        }

        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - chip.width - parent.spacing
                   - (root.badge.length > 0 ? badgePill.width + 6 : 0)
                   - (statusPill.visible ? statusPill.width + 6 : 0)
                   - (actionPill.visible ? actionPill.width + 6 : 0)
            spacing: 2

            Text {
                width: parent.width
                text: root.title
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                font.weight: Font.Medium
            }

            Row {
                width: parent.width
                spacing: 6

                Text {
                    width: Math.max(0, parent.width
                                    - (gateRow.visible ? gateRow.implicitWidth + 6 : 0)
                                    - (statusText.visible ? statusText.implicitWidth + 6 : 0))
                    text: root.subtitle
                    elide: Text.ElideRight
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSub
                }

                Row {
                    id: gateRow
                    visible: root.gateChips
                    spacing: 4
                    // The Row positioner owns x; only y is ours to place.
                    y: 1

                    Repeater {
                        model: [
                            { gate: "overlay", label: "O", on: root.overlayOn },
                            { gate: "media", label: "M", on: root.mediaOn }
                        ]

                        delegate: Item {
                            id: gateChip
                            required property var modelData

                            implicitWidth: gateChipRow.implicitWidth
                            implicitHeight: 12

                            Row {
                                id: gateChipRow
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 5
                                    height: 5
                                    radius: 2.5
                                    color: gateChip.modelData.on
                                           ? Theme.accent
                                           : Qt.alpha(Theme.textMuted, 0.45)
                                    border.width: gateChip.modelData.on ? 0 : 1
                                    border.color: Qt.alpha(Theme.textMuted, 0.6)
                                    Behavior on color { ColorAnimation { duration: 110 } }
                                }

                                // The stub: the connector line that makes this read
                                // as a node rather than a letter on the card.
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 6
                                    height: 1
                                    color: gateChip.modelData.on
                                           ? Qt.alpha(Theme.accent, 0.75)
                                           : Qt.alpha(Theme.textMuted, 0.35)
                                    Behavior on color { ColorAnimation { duration: 110 } }
                                }

                                Text {
                                    id: gateChipLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: gateChip.modelData.label
                                    color: gateChip.modelData.on
                                           ? Theme.accentLight
                                           : Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.textXs
                                    font.weight: Font.DemiBold
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onPressed: (mouse) => mouse.accepted = true
                                onClicked: root.gateToggled(root.nodeKey, gateChip.modelData.gate)
                            }
                        }
                    }
                }

                Text {
                    id: statusText
                    visible: root.status.length > 0 && !root.statusBadge
                    text: root.status
                    color: root.statusColor
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSub
                    font.weight: Font.Medium
                }
            }
        }

        Rectangle {
            id: statusPill
            visible: root.statusBadge && root.status.length > 0
            anchors.verticalCenter: parent.verticalCenter
            width: statusRow.width + 14
            height: 18
            radius: 9
            color: Qt.alpha(root.statusColor, 0.14)

            Row {
                id: statusRow
                anchors.centerIn: parent
                spacing: 5

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 5
                    height: 5
                    radius: 2.5
                    color: root.statusColor
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.status
                    color: root.statusColor
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                    font.weight: Font.Medium
                }
            }
        }

        Rectangle {
            id: actionPill
            visible: root.actionLabel.length > 0
            anchors.verticalCenter: parent.verticalCenter
            width: actionRow.width + (root.actionActive ? 22 : 18)
            height: 22
            radius: 11
            color: root.actionActive ? Theme.armedLiveBg : Theme.armedStopBg
            border.width: 1
            border.color: root.actionActive ? Qt.alpha(Theme.dangerLight, 0.45)
                                            : Theme.border

            Behavior on color { ColorAnimation { duration: 110 } }

            Row {
                id: actionRow
                anchors.centerIn: parent
                spacing: 5

                Rectangle {
                    visible: root.actionActive
                    width: 6
                    height: 6
                    radius: 3
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.danger
                }

                Text {
                    id: actionLabelText
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.actionLabel
                    color: root.actionActive ? Theme.dangerLight : Theme.textSecondary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                // One press = one action; the card's TapHandler must not read it as
                // the start of a double-click edit.
                onPressed: (mouse) => mouse.accepted = true
                onClicked: root.actionTriggered(root.nodeKey)
            }
        }

        Rectangle {
            id: badgePill
            visible: root.badge.length > 0
            anchors.verticalCenter: parent.verticalCenter
            width: badgeLabel.implicitWidth + 10
            height: 16
            radius: 8
            color: Qt.alpha(root.accent, 0.16)

            Text {
                id: badgeLabel
                anchors.centerIn: parent
                text: root.badge
                color: root.accent
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
        }
    }
}