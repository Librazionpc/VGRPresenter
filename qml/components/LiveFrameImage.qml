import QtQuick

// A LIVE FRAME THAT NEVER LOADS ON THE INTERFACE THREAD AND NEVER BLANKS.
//
// Every tile re-requests its picture on every frame revision (`image://livepreview?v=<rev>`). With
// `asynchronous: false` each request ran the provider ON the interface thread — a full-frame copy, a scan and
// a scale per tile per revision — which is what made the whole app slow even with no load. An asynchronous
// Image moves that to the image loader thread, but a plain async Image drops its old picture while the next one
// loads (the flicker the sync load was chosen to avoid). So: TWO Images. The new revision loads into the one
// that is hidden; when it is Ready it swaps to the front and the other becomes the next loading buffer. The
// picture on screen is always a finished frame.
//
// The provider is asked for the item's size (in device pixels, rounded up to a multiple of 64 so a resize does
// not reload on every pixel), so a small tile is not handed a 1920x1080 image to upload.
Item {
    id: root

    property url source
    property int fillMode: Image.Stretch
    // Ask the provider for the item's size instead of the frame's own. Off for a consumer that needs full size.
    property bool sizeToItem: true

    // The self-tests read what a plain Image would answer: the picture on screen.
    readonly property int status: front.status
    readonly property size sourceSize: front.sourceSize
    readonly property real paintedWidth: front.paintedWidth
    readonly property real paintedHeight: front.paintedHeight

    property bool aFront: true
    readonly property Image front: aFront ? imgA : imgB

    readonly property size requestSize: {
        if (!root.sizeToItem || root.width <= 0 || root.height <= 0)
            return Qt.size(0, 0)
        const dpr = Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
        const round64 = (v) => Math.max(64, Math.ceil(v * dpr / 64) * 64)
        return Qt.size(round64(root.width), round64(root.height))
    }

    // THE PROVIDER ANSWERS "NO FRAME YET" WITH A 1x1 TRANSPARENT PLACEHOLDER. A slide that goes on air asks
    // for its frame before the engine has drawn it; for a still slide nothing ever changes the source
    // again, so that placeholder was kept on screen forever (a tile that went black on GO LIVE — the
    // slide had a picture in it, which is the one case the tile shows the engine's frame for). A
    // placeholder never takes the front; the same frame is asked for again until a real one comes.
    property int retries: 0
    readonly property int maxRetries: 60
    function isPlaceholder(img) { return img.implicitWidth <= 1 && img.implicitHeight <= 1 }
    function swapIfReady(img) {
        // Only the buffer that is loading (the hidden one) may take the front.
        if (img.status !== Image.Ready || img === root.front)
            return
        if (root.isPlaceholder(img)) {
            if (root.retries < root.maxRetries)
                retryTimer.restart()
            return
        }
        root.retries = 0
        retryTimer.stop()
        root.aFront = (img === imgA)
    }
    Timer {
        id: retryTimer
        interval: 150
        repeat: false
        onTriggered: {
            const back = root.aFront ? imgB : imgA
            const s = String(root.source)
            ++root.retries
            back.source = s + (s.indexOf("?") >= 0 ? "&" : "?") + "r=" + root.retries
        }
    }

    onSourceChanged: {
        root.retries = 0
        retryTimer.stop()
        if (String(root.source) === "") {            // nothing to show: drop both, so no stale frame lingers
            imgA.source = ""
            imgB.source = ""
            return
        }
        const back = root.aFront ? imgB : imgA
        back.source = root.source
    }

    Image {
        id: imgA
        anchors.fill: parent
        visible: root.aFront
        asynchronous: true
        cache: false            // every revision IS a new frame
        fillMode: root.fillMode
        sourceSize: root.requestSize
        onStatusChanged: root.swapIfReady(imgA)
    }
    Image {
        id: imgB
        anchors.fill: parent
        visible: !root.aFront
        asynchronous: true
        cache: false
        fillMode: root.fillMode
        sourceSize: root.requestSize
        onStatusChanged: root.swapIfReady(imgB)
    }
}
