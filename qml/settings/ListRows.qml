pragma Singleton
import QtQuick
import VGRPresenterUI

// THE striping rule for every list in the app — one pair of colours, decided
// here and nowhere else.
//
// The action dialog's three lists read it through StripedListPanel; the
// virtualized lists (the shows table, The Table / Scripture's verse list, the
// projects tree) cannot host that panel's Column, so they read the same rule
// directly and stripe their own rows. That is the point of this singleton: a
// second hand-picked tint somewhere else is how two lists start to disagree by
// two percent of white.
QtObject {
    id: rows

    // THE two tuning knobs. The tint is the theme's own ink laid over a list's
    // ground, so its DIRECTION flips with the theme on its own (white-ish in
    // dark, dark-ish in light); only its STRENGTH needs a per-theme value,
    // because a light ground reacts far more to the same alpha than a
    // near-black one does. Matched by measurement, in CIE L* (perceptual
    // lightness) — both land on the same ~2.9 step, so neither theme's list
    // reads stronger than the other's:
    //   dark  ink #e2e8f0 @ 0.028 over #15161d surface -> L* +2.95
    //   dark  the same over #191b24 card               -> L* +2.82
    //   light ink #16181f @ 0.036 over #ffffff surface -> L* -2.88
    // Re-measure both if either value moves.
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

    // For a row that paints its OWN ground (a selected row, a depth shade, the
    // hovered row of a view that animates colour itself): the tint to lay over
    // it, "transparent" on the rows that must stay plain.
    function stripeOverlay(index) {
        return index % 2 === 1 ? stripe : "transparent"
    }
}
