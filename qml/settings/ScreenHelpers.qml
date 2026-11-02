pragma Singleton
import QtQuick
import VGRPresenterUI

// Helpers that two or more screens need verbatim. Each of these existed as a copy in
// every screen that wanted it; the copies drift, so they live here once.
QtObject {
    // A template tag's layout class, decided by its largest-area TEXT block:
    // "lower" when that block sits in the bottom half and is at most half tall.
    function layoutClassOf(key) {
        if (typeof key !== "string" || key === "")
            return ""
        if (key.indexOf("tpl-") !== 0)
            return key === "lowerThird" ? "lower" : ""
        const blocks = TemplateLibraryService.design(key).blocks ?? []
        let best = null
        let bestArea = 0
        for (let i = 0; i < blocks.length; ++i) {
            const b = blocks[i]
            if (b.kind !== undefined && b.kind !== "text")
                continue
            const area = Number(b.width) * Number(b.height)
            if (area > bestArea) {
                bestArea = area
                best = b
            }
        }
        if (!best)
            return ""
        return (Number(best.y) >= StageGrid.h * 0.5 && Number(best.height) <= StageGrid.h * 0.5)
               ? "lower" : "full"
    }

    // Reconcile the stream metres with `wanted` and return the set now held.
    function syncVideoMeters(wanted, held) {
        const has = (list, t) => list.some(x => x.label === t.label && x.kind === t.kind)
        for (const t of held)
            if (!has(wanted, t)) EngineBridge.stopStreamMeter(t.label)
        for (const t of wanted)
            if (!has(held, t)) EngineBridge.startStreamMeter(t.label, t.kind, t.label)
        return wanted
    }
}
