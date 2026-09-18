import QtQuick
import VGRPresenterUI

// The dim backdrop behind every modal in this app — one shared component
// instead of eleven hand-rolled scrim Rectangles, so the backdrop's event
// behavior can't drift between dialogs.
//
// Its second job is the one that matters: CONSUME WHEEL EVENTS. Qt
// propagates unaccepted wheel events to the items *behind* the overlay, so
// with a plain scrim, scrolling a modal's Flickable that has hit its end
// (or has nothing to scroll) leaks the scroll into whatever Flickable sits
// in the page underneath — the settings page visibly scrolling behind the
// open dialog. This scrim accepts the wheel at the overlay layer, so the
// event dies here and never reaches the page. Clicking the backdrop still
// dismisses, exactly as the per-dialog MouseAreas always did.
Rectangle {
    id: root

    // Whether a click on the backdrop itself dismisses the modal (every
    // current consumer wants this; kept as a property so a future
    // click-trapping-only scrim doesn't need a fork).
    property bool clickToDismiss: true

    signal dismissed()

    anchors.fill: parent
    color: "#99000000"

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onClicked: if (root.clickToDismiss) root.dismissed()

        // Accept (the default for an implemented handler) — the empty body
        // is the point: the event stops here instead of propagating behind
        // the overlay.
        onWheel: (wheel) => wheel.accepted = true
    }
}
