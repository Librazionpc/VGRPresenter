pragma Singleton
import QtQuick

// THE one linear-amplitude → meter-fill mapping (0..1), on the -60..0 dBFS
// scale every level meter here draws (gradient zones, ticks, needles all
// assume it). Raw linear amplitude pins a fill in its leftmost few percent
// for real audio — meters read in dB, so this is the single conversion the
// meters use (was copy-pasted in LevelTrack, OutputMonitorTile and
// ProAudioForm; three copies were three ways to drift).
//
// `linear` is the WASAPI tap's own domain: 0..1 (or 0..100, see pct()).
//
// It also owns the app's CHANNEL-STATE vocabulary (off / trimmed / at unity):
// a channel's gain is the single source of truth for what that channel is doing,
// and the decision belongs in one place — see the section at the foot of this
// file. The LOOK of each state is Theme's (Theme.channelStateColor).
Item {
    // ≈ -66 dBFS floor → silence (a hair of noise stays zero).
    readonly property real floorLinear: 0.0005

    // 0..1 linear amplitude → 0..1 fill fraction.
    function dbPct(linear) {
        if (linear <= floorLinear)
            return 0
        const db = 20 * Math.log10(linear)
        return Math.max(0, Math.min(1, (db + 60) / 60))
    }

    // 0..100 linear percentage (LevelTrack's input) → 0..1 fill.
    function pct(linearPct) {
        return dbPct(linearPct / 100)
    }

    // ---- The channel-state vocabulary: off / trimmed / at unity -------------
    // A channel's GAIN is the app's single source of truth for its state:
    //   0      = OFF. That is the convention, not an accident — the L/R pills
    //            WRITE 0 to close a side, so every meter reads 0 as off, and a
    //            channel that comes back on is seeded from its remembered trim
    //            rather than from 0 (see ProAudioForm.trimFor).
    //   1      = UNITY, untouched.
    //   between = TRIMMED, an attenuation the operator made deliberately.
    //
    // The decision lives here ONCE because six surfaces used to test
    // `<= 0.0005` for themselves — the gain knob, three cards' pills, the
    // monitor tiles' gain gate, the settings dialogs — and NOT ONE of them could
    // see the trimmed state at all: a side half-closed from the gain knob looked
    // identical to a side at unity everywhere else. Now they all ask this.
    //
    // A gain that is missing or not a number is NOT "off": no information about
    // a trim is not the same as a closed channel, so it reads as untouched.
    readonly property real unityTolerance: 0.005

    function isChannelOff(gain) {
        const g = Number(gain)
        return isFinite(g) && g <= floorLinear
    }
    function isChannelTrimmed(gain) {
        const g = Number(gain)
        return isFinite(g) && g > floorLinear && Math.abs(g - 1) >= unityTolerance
    }
    function isChannelAtUnity(gain) {
        const g = Number(gain)
        return !isFinite(g) || Math.abs(g - 1) < unityTolerance
    }
    // "off" | "trimmed" | "unity" — the name a surface reports, and the key
    // Theme maps to a colour. One call for a surface that has the gain in hand:
    //   border.color: Theme.channelStateColor(gain)
    function channelState(gain) {
        if (isChannelOff(gain))
            return "off"
        return isChannelTrimmed(gain) ? "trimmed" : "unity"
    }

    // ---- ...and the same three states IN WORDS -----------------------------
    // A colour is not a word. Off, trimmed and at unity were readable only as
    // tones (the pill's slash and border, the knob's ring), so a tip that names
    // the state has to be spelled somewhere — and spelled ONCE, or a tooltip on
    // a pill and one on the gain knob could describe the same channel
    // differently. These are the app's three channel-state words, built from the
    // same decisions above.
    //
    // The trim's own dB rides along with it, because "Trimmed" alone does not
    // say by how much — the one number an operator wants off a trimmed side.
    // "Off" carries none: gain 0 is the CLOSED convention, not a level (−∞ dB),
    // and "Unity" is 0 dB by definition.
    function channelStateWord(gain) {
        if (isChannelOff(gain))
            return qsTr("Off")
        return isChannelTrimmed(gain) ? qsTr("Trimmed") : qsTr("Unity")
    }

    // 0..1 linear gain → its dB, to one decimal under 10 dB (a trim is set in
    // fractions of a dB) and whole dB above it, where the fraction is noise.
    // Unity is exactly "0 dB"; a closed channel has no level to print.
    function gainDbText(gain) {
        const g = Number(gain)
        // EXACTLY the decision the state word above is built on, never a second
        // threshold: a side the vocabulary calls "Unity" reads "0 dB", and a side
        // it calls "Trimmed" always prints its own fraction (a -0.03 dB trim is
        // still a trim the operator made, not unity). Unity comes FIRST because
        // it is also the vocabulary's reading for a gain that is missing or not
        // a number — that is "no trim to report", never a closed channel, and
        // printing it as -inf dB made the tip read "Unity · -\u221E dB".
        if (isChannelAtUnity(g))
            return qsTr("0 dB")
        if (!isFinite(g) || g <= floorLinear)
            return qsTr("-\u221E dB")        // same spelling as ProAudioForm's own readouts
        const db = 20 * Math.log10(g)
        return (db < 0 ? "-" : "+") + (Math.abs(db) < 10 ? Math.abs(db).toFixed(1)
                                                          : String(Math.round(Math.abs(db))))
               + " dB"
    }

    // The state and its value in ONE phrase — "Off", "Unity · 0 dB",
    // "Trimmed · -6.0 dB" — which is what a tooltip names.
    function channelStateText(gain) {
        const word = channelStateWord(gain)
        if (isChannelOff(gain))
            return word
        return word + " · " + gainDbText(gain)
    }
}
