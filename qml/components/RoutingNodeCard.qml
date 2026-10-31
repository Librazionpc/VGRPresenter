pragma ComponentBehavior: Bound

import QtQuick
import VGRPresenterUI

// One node on the Settings · Routing canvas — a bus, a screen, a style or an
// output (sink). Pure presentation: the card carries no ports and no wire
// state. The canvas owns both, so a wire's endpoint and the dot it leaves from
// are computed from ONE set of formulas and can never disagree (the reason the
// Audio & Video board draws its connector endpoints the same way).
//
// The card only reports hover, and only so the canvas can lift the wires that
// belong to it.
Rectangle {
    id: root

    // The canvas's key for this node ("bus:3", "screen:Congregation",
    // "sink:ndi"), passed straight back with hoverChanged so the canvas never has
    // to guess which delegate the pointer is over.
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
    // Draw that state as a compact BADGE at the card's right end (FreeShow's own
    // node cards) instead of as text sharing the subtitle's line: the subtitle then
    // owns the whole line, so "Audio · system endpoint" is not cut down to "Audio ·
    // system en…" just to leave room for four words of state.
    property bool statusBadge: false
    // A connect-drag is live and this card is / is not a legal target.
    property bool dropValid: false
    property bool dropInvalid: false
    // A transport button on the card's right end: START/STOP a take on the node
    // THIS card is wired to. An empty label = no button, so the bus, style, screen
    // and speaker cards keep exactly the look they had. `actionActive` is the live
    // state of the take this card owns (the canvas answers it from the engine's
    // own session), which is what turns the pill red and the label into "Stop".
    property string actionLabel: ""
    property bool actionActive: false

    // THE TWO GATES ON THE CARD ITSELF — an output's own Overlays / Media
    // switches (OutputListModel::setShowOverlay / setShowMedia). Only output
    // cards set gateChips; the bus, style and speaker cards keep the look they
    // had.
    //
    // They are DRAWN as connector nodes — a dot, a short stub of line, then the
    // label — because that is what everything else on this canvas is, and a gate
    // is the one thing on a card that is an allow-list fact rather than a picture.
    // They are deliberately NOT ports: nothing can be wired to them, the canvas's
    // port hit-test never sees them (they are inside the card, not on its edge),
    // and their own MouseArea claims the press so neither the card's double-click
    // nor the canvas's drag starts from one. A press simply flips the gate.
    property bool gateChips: false
    property bool overlayOn: true
    property bool mediaOn: true

    readonly property bool hovered: hover.hovered

    signal hoverChanged(string key, bool isHovered)
    // A right-click on the card. The canvas opens its shared DropdownPanel at the
    // click point, so a right-click here behaves like one on a roster row
    // anywhere else in Settings; the card reports the point rather than hosting
    // the menu, because the menu is the canvas's (it is what knows the layer).
    signal contextRequested(string key, var card, real localX, real localY)
    // A DOUBLE-CLICK opens this node's editor. The canvas decides what "its
    // editor" means per layer (a bus lives on Audio & Video, a style on Styles,
    // an output in its own dialog), so the card only reports the gesture — the
    // same split the right-click menu uses.
    signal activated(string key)
    // The transport pill was pressed. The canvas decides what it MEANS per layer
    // (a REC / STREAM card starts or stops a take on its own node).
    signal actionTriggered(string key)
    // A gate chip was pressed. "overlay" or "media"; the canvas flips that row's
    // flag (OutputListModel) and the chip fades to its new state.
    signal gateToggled(string key, string gate)

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

    // The hover wash is the app's shared layer (HoverGlow), not a colour binding
    // on this card's own `color`: one animated layer fades instead of two colours
    // racing (see HoverGlow's header), so a card on this canvas lights up exactly
    // like a row or a tile elsewhere. No edge here — the card already owns its
    // accent border for drop validity.
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

    // DOUBLE-CLICK = EDIT. A TapHandler, not a MouseArea, on purpose: it claims
    // the pointer only when it recognises the gesture, so a single press on the
    // body still reaches the canvas exactly as before, and the port dots (their
    // own items, drawn above the cards) keep their drags untouched. The card's
    // right-click MouseArea stays as it is — one gesture per handler keeps the
    // two from fighting over acceptedButtons.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: root.activated(root.nodeKey)
    }

    // Right button only: the LEFT press belongs to the canvas, whose ports and
    // drop targets are hit-tested by coordinate (see RoutingScreen::nodeAt), so
    // a card-level handler must never swallow it.
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

            IconGlyph {
                anchors.centerIn: parent
                width: 13
                height: 13
                fit: true
                name: root.iconName
                color: root.accent
                visible: root.swatch.a <= 0.001
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
                    // The status owns the right-hand end of the line, so the
                    // subtitle gives up exactly its width: a long screen name
                    // elides instead of pushing "No signal" out of the card.
                    // The gate chips share this line too (see gateRow), so they are
                    // subtracted the same way — on a card that carries both a
                    // transport and a state pill the line is short, and this is what
                    // keeps the chips from drawing over them.
                    width: Math.max(0, parent.width
                                    - (gateRow.visible ? gateRow.implicitWidth + 6 : 0)
                                    - (statusText.visible ? statusText.implicitWidth + 6 : 0))
                    text: root.subtitle
                    elide: Text.ElideRight
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.textSub
                }

                // THE TWO GATE NODES (see gateChips): a dot, a stub of line and the
                // gate's initial — the connector vocabulary this canvas already
                // speaks, at subtitle height so they cost the card no extra row.
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

                                // The port-style dot. Lit when the gate is open;
                                // muted and hollow when it is closed, so the state
                                // reads before the letter does.
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

                            // One press = one flip. Accepting the press keeps the
                            // card's TapHandler from reading it as the first half of
                            // a double-click edit.
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

        // The state badge. Its own ground so the state reads at a glance without
        // spending the subtitle's line on it — the dot beside the label carries the
        // colour (warning for a wired-but-dark feed, muted for nothing wired).
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

        // The transport pill. A voice of its own: it is the only thing on this
        // canvas that CHANGES STATE rather than describing one, so it is filled
        // (the app's armedStopBg / armedLiveBg washes) instead of outlined like the
        // type badge beside it. Its own MouseArea keeps the click off the card's
        // double-click handler and off the canvas behind it.
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

                // The live dot, in the same red every other live pill in the app
                // uses, so "this is recording" reads the same here as on the
                // Recording screen.
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
