import QtQuick
import QtMultimedia
import VGRPresenterUI

// THE ONE WAY THIS APP DRAWS A MEDIA FILE THAT IS ON AIR.
//
// A video file is decoded ONCE (LiveOutputService's player) and its frames go straight to this
// surface's VideoOutput — the same GPU path the Media tab's cards already take, so every consumer
// (the monitor wall, the output windows, the stage) shows the same picture at the file's own rate.
// The old per-consumer `image://mediaplay` request built AND scaled a full QImage per consumer per
// frame on the loader / interface thread, which is what left a tile blank and every tile behind
// the decoder.
//
// WITHOUT A GPU IT STILL WORKS. A VideoOutput needs a graphics API behind the QML scene; when the
// scene is on the software backend this component falls back to the provider path it replaced —
// the file's frames as images, double buffered by LiveFrameImage, so a machine with no GPU shows
// the same picture, just paying the copies that path always paid for. A STILL image on air has no
// stream to mirror and always takes that provider path (one copy).
//
// Debugging: `path` says which of the two this instance draws from, and the switch is logged once
// per surface (trace) — one place to look when a tile is blank.
Item {
    id: root

    // True while the media on air is the picture this surface should show.
    property bool active: false
    // The file's own fill: "contain" | "cover" | "fill".
    property string fit: "contain"
    // The still path's request size (see LiveFrameImage); false keeps the file's own size.
    property bool sizeToItem: true
    // WHICH OUTPUT THIS SURFACE BELONGS TO — its roster row, or -1 when it is not an output (the
    // library's own preview). The file is drawn on an output only once its ROUTE is connected:
    // RoutingService.mediaRouteState is the one rule (a bus wired in for a display sink PLUS the
    // Media layer; the Media layer alone for a REC / STREAM row or a mirror), so a tile whose route
    // is not connected shows no media at all and the take's notice is what explains the black tile.
    // WHICH OUTPUT THIS SURFACE BELONGS TO, BY NAME — not by model index. The tile's index and the
    // board's row are not the same key in every model: matching on the index left a routed tile asking
    // about someone else's row, so the gate said "not routed" forever and a connected output stayed
    // black. An empty name means "not an output" (the library's own preview), which is never gated.
    property string outputName: ""
    readonly property var routeRows: RoutingService.rows
    // THE BOARD'S ANSWERS ARE RE-ASKED ON THIS COUNTER, not on the rows' own reactivity. Depending on
    // RoutingService.rows left this binding holding the answer it got BEFORE the roster published (gate
    // said "media" then), so a routed output stayed black: the gate's own log line printed "ok" for the
    // same output at the same moment this said routeReady=false. A bumped counter cannot be missed.
    // A BINDING HERE WAS NOT RELIABLE, so the gate is ASKED: it held the answer it got before the roster
    // published ("not routed"), and a routed output then drew nothing while the same rule answered "ok"
    // for that output in the very same log line. These calls cannot go stale, and every flip is logged.
    property bool routeReady: false
    function refreshRoute() {
        const name = root.outputName
        const state = name === "" ? "preview" : RoutingService.mediaRouteStateForName(name)
        const ready = name === "" || state === "ok"
        if (ready !== root.routeReady) {
            root.routeReady = ready
            EngineBridge.log("info", "LiveMediaSurface",
                             "gate " + (ready ? "OPEN" : "CLOSED") + ": output="
                             + (name === "" ? "preview" : name) + " state=" + state)
        }
    }
    onOutputNameChanged: root.refreshRoute()
    Connections {
        target: RoutingService
        function onRowsChanged() { root.refreshRoute() }
        function onRoutingChanged() { root.refreshRoute() }
    }

    readonly property bool mediaShown: root.active && root.routeReady && LiveOutputService.mediaOnAir
    readonly property bool videoShown: root.mediaShown && LiveOutputService.mediaIsVideo
    readonly property bool stillShown: root.mediaShown && !LiveOutputService.mediaIsVideo
                                       && !LiveOutputService.mediaIsAudio

    // The QML scene's own backend: the software one cannot paint a VideoOutput, so the provider
    // path draws instead — the same picture, on any machine.
    readonly property bool gpuVideo: GraphicsInfo.api !== GraphicsInfo.Software

    readonly property bool pathGpu: root.videoShown && root.gpuVideo
    readonly property bool pathImages: root.stillShown || (root.videoShown && !root.gpuVideo)
    readonly property string path: root.pathGpu ? "gpu" : (root.pathImages ? "images" : "none")

    readonly property int videoFill: root.fit === "cover" ? VideoOutput.PreserveAspectCrop
                                    : root.fit === "fill" ? VideoOutput.Stretch
                                                          : VideoOutput.PreserveAspectFit
    readonly property int imageFill: root.fit === "cover" ? Image.PreserveAspectCrop
                                    : root.fit === "fill" ? Image.Stretch
                                                          : Image.PreserveAspectFit

    // Attached once per surface: every frame the player decodes is forwarded to this sink on the
    // decoder's thread (QVideoSink::setVideoFrame is thread-safe), so showing it costs the
    // interface thread nothing per frame.
    function attach() {
        // Wired the moment a VIDEO is the picture, whichever path draws it: a sink attached only on the
        // GPU path stayed empty on a software scene, and one attached late left the tile blank until the
        // next take. Attaching is idempotent (the service keeps one entry per sink).
        if (root.active && LiveOutputService.mediaIsVideo)
            LiveOutputService.attachMediaSink(videoOut.videoSink)
    }
    onPathGpuChanged: root.attach()
    onActiveChanged: {
        root.attach()
        // THE DECISION, logged on the event that matters (a file became this surface's picture): a blank
        // surface is then read from this one line instead of guessed at — whether it was the gate, or the
        // path, or the surface never being asked to draw.
        EngineBridge.log("info", "LiveMediaSurface",
                         "object=" + root.objectName + " active=" + root.active
                         + " routeReady=" + root.routeReady + " shown=" + root.mediaShown
                         + " path=" + root.path + " gate="
                         + (root.outputName === "" ? "preview"
                                                   : RoutingService.mediaRouteStateForName(root.outputName))
                         + " output=" + (root.outputName === "" ? "preview" : root.outputName))
    }
    // Diagnosing "the file plays but the tile is black" needs the decision, not a guess: these lines say
    // which path was chosen and whether the gate let it through, at info level so it is in engine.log.
    // The routeReady transition is the one that matters — it is the gate opening on a live take.
    onRouteReadyChanged: EngineBridge.log("info", "LiveMediaSurface",
                                          "object=" + root.objectName + " routeReady=" + root.routeReady
                                          + " output=" + (root.outputName === "" ? "preview" : root.outputName)
                                          + " media=" + LiveOutputService.mediaName)
    onPathChanged: EngineBridge.log("info", "LiveMediaSurface",
                                    "object=" + root.objectName + " path=" + root.path
                                    + " active=" + root.active + " routeReady=" + root.routeReady
                                    + " output=" + (root.outputName === "" ? "preview" : root.outputName)
                                    + " fit=" + root.fit + " media=" + LiveOutputService.mediaName)
    Component.onCompleted: {
        root.refreshRoute()   // the gate's first answer (the board may already be published)
        root.attach()
    }

    VideoOutput {
        id: videoOut
        objectName: "liveMediaVideo"
        anchors.fill: parent
        visible: root.pathGpu
        fillMode: root.videoFill
    }

    LiveFrameImage {
        objectName: "liveMediaFrame"
        anchors.fill: parent
        visible: root.pathImages
        source: root.pathImages ? "image://mediaplay?v=" + LiveOutputService.mediaRev : ""
        fillMode: root.imageFill
        sizeToItem: root.sizeToItem
    }
}
