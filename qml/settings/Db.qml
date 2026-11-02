pragma Singleton
import QtQuick

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
    function channelState(gain) {
        if (isChannelOff(gain))
            return "off"
        return isChannelTrimmed(gain) ? "trimmed" : "unity"
    }

    // ---- ...and the same three states IN WORDS -----------------------------
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