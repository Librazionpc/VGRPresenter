import QtQuick

// A "+", drawn from two centered bars instead of the SVG-path glyph IconGlyph's "plus" uses - that
// path renders visibly off-center (confirmed on screen: the cross sits up-and-left of its circle's
// true center, in the Projects panel's FAB and everywhere else "plus" was used), the same class of
// bug this app already fixed once before for the timer's own +/- buttons by drawing them this way.
// Two Rectangles centered by anchors can't drift off-center, so every "+" in the app draws through
// this component now instead.
//
//   PlusGlyph { size: 22; thickness: 2.4; color: "#ffffff" }
//   PlusGlyph { size: 14; thickness: 1.6; anchors.verticalCenter: parent.verticalCenter }   // inline beside a label
//   PlusGlyph { rotation: open ? 135 : 0; Behavior on rotation { NumberAnimation { duration: 200 } } }   // "+" -> "x"
Item {
    id: root

    property real size: 14
    property real thickness: Math.max(1.2, size * 0.12)
    property color color: "#ffffff"

    implicitWidth: size
    implicitHeight: size

    Rectangle {
        anchors.centerIn: parent
        width: root.size
        height: root.thickness
        radius: root.thickness / 2
        color: root.color
    }
    Rectangle {
        anchors.centerIn: parent
        width: root.thickness
        height: root.size
        radius: root.thickness / 2
        color: root.color
    }
}
