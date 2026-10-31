import QtQuick
import VGRPresenterUI

// THE primary add button — the accent-filled "+ Add …" bar a page uses for its
// one call-to-action that CREATES something.
//
// It lived inline in OutputsScreen ("+ Add Screen") until Settings · Routing
// needed the same control at the foot of each of its four columns. Two copies of
// an accent fill, a drawn plus, a hover colour and a 40 px height would have
// drifted apart on the first retune — and the two surfaces sit one click from
// each other, so the drift would be visible. Extracted, so a screen and a bus
// are created by the same-looking button.
//
// WHY NOT AppButton: AppButton is a ROW-level action (34 px, `primary` fills it
// with the accent). This one is a section's headline action — 40 px, with more
// horizontal padding, the label DemiBold in the on-accent ink, and a plus DRAWN
// from two bars rather than IconGlyph's "plus" path, which renders visibly
// off-centre (the reason PlusGlyph exists). OutputsScreen's own comment made
// that distinction; it travels with the control now.
//
// Width is the content's own, like the button on Settings · Screens: a caller
// that wants it stretched sets `width` itself (the Routing columns centre it
// instead, so the four footers stay the size of the button rather than the
// column).
Item {
    id: root

    property string text: ""
    signal clicked()

    implicitWidth: contentRow.width + Theme.space6 * 2
    implicitHeight: 40

    Rectangle {
        id: face
        anchors.fill: parent
        radius: Theme.radiusMd
        color: area.pressed ? Qt.darker(Theme.accent, 1.15)
             : area.containsMouse ? Qt.lighter(Theme.accent, 1.1)
             : Theme.accent
        Behavior on color { ColorAnimation { duration: Theme.hoverFade; easing.type: Theme.hoverEasing } }

        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: 8

            // The plus, drawn: two bars centred by anchors cannot sit off-centre
            // the way a stroked path can.
            Item {
                width: 11; height: 11
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    anchors.centerIn: parent
                    width: 11; height: 1.6; radius: 0.8
                    color: Theme.controlInk
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 1.6; height: 11; radius: 0.8
                    color: Theme.controlInk
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.text
                color: Theme.controlInk
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
