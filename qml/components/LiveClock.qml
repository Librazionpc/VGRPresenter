import QtQuick

// Shared ticking clock — one component every clock/timer-driven visual (the
// Clock/Timer "+" menu pickers' live previews, and their canvas items in
// EditScreen.qml) imports and reuses, instead of each maintaining its own
// second-by-second Timer plus its own HH:MM:SS/12-vs-24-hour formatting
// code. Named LiveClock rather than "Timer" (as originally asked for)
// specifically so it doesn't shadow QtQuick's own built-in Timer element in
// any file that imports both — this component wraps exactly one of those
// internally.
//
// formatClock/timerSeconds are pure functions taking `now` as an explicit
// argument rather than reading `root.now` internally — a consumer's binding
// (e.g. `text: LiveClock.formatClock(ticker.now, ...)`) reads `ticker.now`
// itself at the call site, which is a plain, guaranteed-reactive QML
// property read. Reading it *inside* the function instead worked in
// principle (QML is supposed to track property reads through a function
// call), but wasn't actually updating the Clock picker's live preview in
// practice — this shape removes any doubt about it by keeping the reactive
// read and the pure computation clearly separate.
Item {
    id: root

    property date now: new Date()
    // Exposed so a consumer can freeze the tick (e.g. a picker that closed,
    // or a paused timer) without tearing the component down.
    property bool running: true

    Timer {
        interval: 1000
        running: root.running
        repeat: true
        triggeredOnStart: true
        onTriggered: root.now = new Date()
    }

    function pad(n) {
        return n < 10 ? "0" + n : String(n)
    }

    // Formats `now` as a clock string — "11:50:47 PM" (12-hour) or
    // "23:50:47" (24-hour), each with or without the seconds field.
    function formatClock(now, hour12, showSeconds) {
        let h = now.getHours()
        const m = root.pad(now.getMinutes())
        const s = root.pad(now.getSeconds())
        let suffix = ""
        if (hour12) {
            suffix = h >= 12 ? " PM" : " AM"
            h = h % 12
            if (h === 0)
                h = 12
        }
        return showSeconds ? (root.pad(h) + ":" + m + ":" + s + suffix)
                            : (root.pad(h) + ":" + m + suffix)
    }

    // Seconds remaining/elapsed for a Timer canvas item, given its mode
    // ("countdown" | "countup" | "timeofday"), its configured duration (for
    // "timeofday", interpreted as seconds-since-midnight instead), and the
    // moment it was started — the same math a picker's live preview and the
    // canvas visual both need, kept in one place instead of duplicated.
    function timerSeconds(now, mode, durationSeconds, startedAt) {
        const elapsed = Math.floor((now.getTime() - startedAt) / 1000)
        if (mode === "countup")
            return Math.max(0, elapsed)
        if (mode === "timeofday") {
            const midnight = new Date(now.getFullYear(), now.getMonth(), now.getDate())
            const nowSecs = Math.floor((now.getTime() - midnight.getTime()) / 1000)
            let diff = durationSeconds - nowSecs
            if (diff < 0)
                diff += 86400
            return diff
        }
        // "countdown"
        return Math.max(0, durationSeconds - elapsed)
    }

    // Formats a seconds count as HH:MM:SS.
    function formatDuration(totalSeconds) {
        const s = Math.max(0, Math.floor(totalSeconds))
        const hh = Math.floor(s / 3600)
        const mm = Math.floor((s % 3600) / 60)
        const ss = s % 60
        return root.pad(hh) + ":" + root.pad(mm) + ":" + root.pad(ss)
    }
}
