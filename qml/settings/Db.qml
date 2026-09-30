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
}
