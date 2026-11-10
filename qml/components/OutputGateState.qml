import QtQuick
import VGRPresenterUI

// THE TWO GATES EVERY OUTPUT PICTURE PASSES — ONE RULE, shared by the monitor tile (OutputMonitorTile)
// and the output window (OutputWindow), exactly as EmptyFrameState holds what an empty frame is.
//
// A file reaches an output only when BOTH hold: the output is switched on, and the board's own M switch
// says its media layer is on (RoutingService.mediaRouteStateForName — an OUTPUT needs only the media
// layer; a BUS is what needs a source AND the layer). Overlays are the O switch, and a name the board
// does not know hides nothing.
//
// The two copies this replaces had drifted. The tile treated an empty name as "gate open" while the
// window treated it as closed; the window asked the OVERLAY gate BY ROW, and the board's rows are not the
// output indexes (the buses take the first rows), so each window asked about a bus; and the window's
// overlay answer was never re-asked when the board republished, so flipping O could leave a window
// painting what the tile had already hidden.
//
// The answers are ASKED, not bound over `RoutingService.rows`: such a binding keeps the answer it got
// before the roster published (see LiveMediaSurface.refreshRoute), which is how a genuinely routed output
// could read as unrouted forever. `refresh()` runs on every change that can move an answer.
QtObject {
    id: root

    // ---- the facts the surface knows about itself ---------------------------------------------
    property string outputName: ""
    property bool outputEnabled: true
    property bool blacked: false
    property bool inputTaken: false

    // ---- the gate's answers -------------------------------------------------------------------
    property bool mediaOpen: false
    // An output nobody can name is not one whose overlays we can switch off: hide nothing.
    property bool overlaysOn: true

    function refresh() {
        const name = String(root.outputName)
        // A SWITCHED-OFF OUTPUT RECEIVES NOTHING: the Media layer is only half the answer, the row being
        // enabled is the other half (RoutingService answers "ok" for a disabled row on purpose, so the
        // take's notice never names an output the operator switched off — the gate checks it here).
        const media = root.outputEnabled && name !== ""
                      && RoutingService.mediaRouteStateForName(name) === "ok"
        const overlays = name === "" || RoutingService.outputShowsOverlaysForName(name)
        if (media !== root.mediaOpen)
            root.mediaOpen = media
        if (overlays !== root.overlaysOn)
            root.overlaysOn = overlays
    }
    onOutputNameChanged: root.refresh()
    onOutputEnabledChanged: root.refresh()
    Component.onCompleted: root.refresh()

    // Held in a property, not declared as a child: a QtObject has NO default property, so a bare
    // `Connections {}` inside one fails to load the whole component. Same shape as EmptyFrameState.
    property Connections routingConn: Connections {
        target: RoutingService
        function onRowsChanged() { root.refresh() }
        function onRoutingChanged() { root.refresh() }
    }

    // ---- what the surfaces actually gate on: a file on air, THIS output's, nothing over it -----
    readonly property bool mediaOnAir: !root.blacked && LiveOutputService.mediaOnAir
                                       && !root.inputTaken && root.mediaOpen
}
