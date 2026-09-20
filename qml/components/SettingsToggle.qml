import QtQuick
import VGRPresenterUI

// The settings pill toggle — the on/off control every preferences row uses
// (General screen rows, and any future settings section). One shared
// control instead of a hand-rolled track/knob per row, matching the
// reference exports' toggle exactly: purple track + white knob when on,
// dark inset track + white knob when off, knob glides between the two.
//
// Controlled-component convention, same as LabeledSlider: `checked` is
// OWNED BY THE CONSUMER — this control never self-assigns it (a self-flip
// destroys the consumer's `checked` binding on first click and, with a
// consumer that also flips in onToggled, double-applies the change — the
// "toggle behaves erratically" bug class). It only REPORTS via toggled();
// every consumer flips its own state there.
Rectangle {
    id: root

    property bool checked: false
    signal toggled()

    implicitWidth: 36
    implicitHeight: 20
    radius: height / 2
    color: checked ? Theme.accent : Theme.toggleOffTrack
    border.width: 1
    border.color: checked ? Theme.accent : Theme.borderSubtle
    Behavior on color { ColorAnimation { duration: 120 } }

    Rectangle {
        id: knob
        width: 14
        height: 14
        radius: 7
        anchors.verticalCenter: parent.verticalCenter
        color: "#ffffff"
        x: root.checked ? root.width - width - 3 : 3
        Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutQuad } }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled()   // consumer flips `checked` — see header
    }
}
