import QtQuick
import VGRPresenterUI

// WHAT AN OUTPUT'S EMPTY FRAME IS — ONE RULE, shared by the monitor tile (OutputMonitorTile) and the
// output window (OutputWindow). They are the same frame at two sizes, and a tile that says "transparent"
// beside a window that says "black" is the bug this exists to prevent. The two copies had already
// drifted: the window asked only "is a file on air in the app" where the tile asked "does THIS output
// draw it", and both tested the engine's preview buffer for EXISTENCE rather than for content — so
// GO LIVE turned a cleared tile and window from the checkerboard into a black rectangle.
//
// A frame is CLEAR — and this app draws transparency as the checkerboard — when the output is switched
// off, blacked out, or its style background is transparent, and when NOTHING IS COMPOSED ON IT and the
// style paints nothing of its own.
//
// "Nothing composed" is decided by WHAT is composed (a slide, this output's file, a taken input, or an
// engine frame the engine composed FROM that input), never by the engine's buffer merely existing: that
// buffer appears for every output the moment the session goes live, and for one that composes nothing it
// is empty. A slide, a file and an input each have their own marker here, so a frame counts as content
// only through one of them or through `engineComposed`.
//
// A frame that is not clear paints the composition's colour, else the style's own background colour,
// else black — and above that the style's background IMAGE, from the composition when there is a slide
// and from the style roster when there is not.
QtObject {
    id: root

    // ---- the facts a caller supplies, and nothing else ------------------------------------
    property bool enabled: true
    property bool blacked: false
    // This output's style, for the roster read below.
    property string styleId: ""
    // The composed slide: frameTransparent, styleColor, styleImage, styleImagePainted.
    property var slide: ({})
    property bool hasSlidePreview: false
    // THIS output draws the media file — the caller's own route gate is already applied.
    property bool mediaOnAir: false
    property bool inputTaken: false
    // The engine composed this output's picture (a taken input), i.e. the frame has CONTENT.
    property bool engineComposed: false
    // An engine preview frame EXISTS. Not content: see the header, and the style-image layer below,
    // which steps aside for a frame being drawn while the CLEAR decision asks what that frame holds.
    property bool hasFrame: false

    // ---- the output's own style background -------------------------------------------------
    // Read from the style roster by this output's own style id, never from the composition alone: with
    // no slide there IS no composition map (`slideToVariantMap` returns {} for a null slide), which is
    // exactly the blank frame this describes. StyleListModel is not a reactive QML source, so the
    // roster's own change signal re-reads it.
    property int rosterRev: 0
    readonly property var styleBackground: {
        void root.rosterRev
        const at = StyleListModel.rowForId(String(root.styleId))
        return at < 0 ? ({}) : StyleListModel.getStyle(at)
    }
    readonly property string styleBackgroundColor: String(root.styleBackground.backgroundColor ?? "")
    readonly property string styleBackgroundImage: String(root.styleBackground.backgroundImage ?? "")
    readonly property bool styleBackgroundPaints:
        (root.styleBackgroundColor !== "" && root.styleBackgroundColor !== "transparent")
        || root.styleBackground.hasImage === true
    // Held in a property, not declared as a child: a QtObject has NO default property, so a bare
    // `Connections {}` inside one fails to load the whole component ("Cannot assign to non-existent
    // default property") and every caller then reports "Type EmptyFrameState unavailable". Same shape
    // as BusMonitor.qml.
    property Connections rosterConn: Connections {
        target: StyleListModel
        function onRosterChanged() { root.rosterRev++ }
    }

    // ---- the two empty states ---------------------------------------------------------------
    readonly property bool frameClear: !root.enabled
        || root.blacked
        || root.slide.frameTransparent === true
        || (!root.hasSlidePreview && !root.mediaOnAir && !root.inputTaken && !root.engineComposed
            && !root.styleBackgroundPaints)

    // The colour a frame that is not clear paints.
    readonly property string compositionColor: String(root.slide.styleColor ?? "")
    readonly property color frameColor:
        (root.compositionColor !== "" && root.compositionColor !== "transparent")
        ? root.compositionColor
        : ((root.styleBackgroundColor !== "" && root.styleBackgroundColor !== "transparent")
           ? root.styleBackgroundColor : "#000000")

    // ---- the style's background image, the layer above the colour ----------------------------
    readonly property bool styleImagePainted: root.slide.styleImagePainted === true
    // The roster's own image paints when no slide composes one and this output paints nothing else —
    // and not while a frame is being drawn over it (hasFrame), which is the layer order the callers
    // draw in. A SWITCHED-OFF output is excluded: it sends nothing, so its style's picture must not
    // cover the checkerboard that says so.
    readonly property bool rosterStyleImage: root.enabled && !root.hasSlidePreview
                                             && !root.mediaOnAir && !root.inputTaken && !root.hasFrame
                                             && root.styleBackgroundImage !== ""
                                             && root.styleBackground.hasImage === true
    readonly property bool styleImageVisible: !root.blacked
                                              && (root.styleImagePainted || root.rosterStyleImage)
    readonly property string styleImageSource: root.styleImagePainted
        ? "file:///" + String(root.slide.styleImage ?? "")
        : (root.rosterStyleImage ? "file:///" + root.styleBackgroundImage : "")
}
