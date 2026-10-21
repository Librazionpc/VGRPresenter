import QtQuick
import QtQuick.Effects
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
// event dies here and never reaches the page. Outside clicks are consumed
// without closing the dialog; a caller can opt in to backdrop dismissal.
Rectangle {
    id: root

    // Keep popups open when the user clicks outside them. Callers can opt in
    // to backdrop dismissal for a specific transient dialog.
    property bool clickToDismiss: false

    // Optional: the item directly behind this scrim to blur (the Settings
    // overlay sets this to whichever of Show/Edit is currently visible — see
    // Main.qml's settingsScrim). null (the default) keeps every OTHER
    // ModalScrim in the app exactly as it always was — a flat dim, no blur,
    // no live GPU cost — a caller has to opt in explicitly.
    property Item blurSource: null

    signal dismissed()

    anchors.fill: parent
    // Dark enough to read as a backdrop even over this app's own near-black panels, not just over a bright screen - 60% was
    // correct but close to invisible against a background already this dark.
    // Transparent instead when blurring: the MultiEffect + its own dim
    // Rectangle below become the backdrop, so this would otherwise paint
    // UNDER a blur that already covers the whole scrim for nothing.
    color: root.blurSource !== null ? "transparent" : "#cc000000"

    // Live blur of whatever's behind this scrim — only built when a caller
    // actually sets blurSource, so every other dialog in the app pays
    // nothing for this.
    MultiEffect {
        anchors.fill: parent
        visible: root.blurSource !== null
        source: root.blurSource
        autoPaddingEnabled: false
        blurEnabled: true
        blurMax: 48
        blur: 1.0
    }
    // A lighter dim over the blur than the flat scrim uses — the blur
    // itself already separates the dialog from the page, so less darkening
    // reads as modal without flattening the blur to near-black.
    Rectangle {
        visible: root.blurSource !== null
        anchors.fill: parent
        color: "#80000000"
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        // Without this, the scrim only ever claimed clicks/wheel — plain
        // mouse MOVEMENT (no button held) fell straight through to
        // whatever sat behind it, so buttons and rows under an open modal
        // kept lighting up on hover as if the modal weren't even there.
        // hoverEnabled makes this MouseArea the hit-test stop for hover
        // too, the same way it already was for clicks.
        hoverEnabled: true
        onClicked: if (root.clickToDismiss) root.dismissed()

        // Accept (the default for an implemented handler) — the empty body
        // is the point: the event stops here instead of propagating behind
        // the overlay.
        onWheel: (wheel) => wheel.accepted = true
    }
}
