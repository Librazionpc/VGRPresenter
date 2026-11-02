pragma Singleton
import QtQuick
import VGRPresenterUI

QtObject {
    id: rows

    readonly property real darkStripeAlpha: 0.028
    readonly property real lightStripeAlpha: 0.036
    readonly property real stripeAlpha: Theme.dark ? darkStripeAlpha : lightStripeAlpha
    readonly property color stripe: Qt.alpha(Theme.textPrimary, stripeAlpha)
    // The wash a row takes while the pointer is on it (the same one
    // StripedListPanel's rows use).
    readonly property color hover: Theme.hoverBg

    // A row's resting colour: hover wins, then the alternating tint, then the
    // list's own ground shows through.
    function rowColor(index, hovered) {
        if (hovered)
            return hover
        return stripeOverlay(index)
    }

    function stripeOverlay(index) {
        return index % 2 === 1 ? stripe : "transparent"
    }
}