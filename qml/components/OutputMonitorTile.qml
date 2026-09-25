import QtQuick
import VGRPresenterUI

// One output-monitor tile — the card in the Show screen's wall and the Edit
// screen's ITEMS tab, driven by OutputListModel in both places. The tile
// shows TRUTH: the on-air slide's design blocks (the SAME { blocks, background }
// shape the ReferencePane preview renders, served by LiveOutputService.onAirSlide)
// drawn through DesignPreview, OR the distributed frame when the content is
// media-like (a camera/media block draws only as a placeholder tile, so the
// real pixels win), OR the checkerboard "nothing" texture when off air.
//
// SCALABLE: the tile fills whatever width its wall assigns; internals are
// anchored; the preview pane holds a true 16:9. Height derives from width.
Rectangle {
    id: root

    // OutputListModel roles
    required property string name
    required property string badge
    required property bool active
    required property bool isEnabled

    // The distributed frame (image://livepreview re-fetched on every frameRev
    // bump). One provider serves the PREVIEW output's last frame — every tile
    // mirrors it (single-pipeline software renderer: the frame the preview
    // output received IS what the real outputs got).
    readonly property bool hasFrame: LiveOutputService.live && LiveOutputService.frameRev > 0
    readonly property url frameSource: hasFrame ? "image://livepreview?v=" + LiveOutputService.frameRev : ""

    // The on-air slide AS DESIGN BLOCKS — the same data the preview pane's
    // DesignPreview draws, straight from the runtime's current slide (no
    // re-resolving the on-air title through a service, so every content kind
    // works: scripture, The Table, shows).
    readonly property var onAirSlide: LiveOutputService.onAirSlide
    readonly property bool hasSlidePreview: onAirSlide.valid === true
                                            && onAirSlide.blocks
                                            && onAirSlide.blocks.length > 0

    // Pixel-kind decision: when the on-air slide carries media-like blocks
    // (camera/media/audio/image), DesignPreview could only draw the source's
    // NAME as a placeholder tile — the distributed FRAME is the honest picture
    // for those, so it wins. Text/shape/clock/timer slides draw true in
    // DesignPreview and take the crisp block rendering (the frame can lag a
    // slide change; the blocks cannot).
    readonly property bool framePriority: {
        if (!hasFrame || !hasSlidePreview)
            return false
        const blocks = onAirSlide.blocks
        for (let i = 0; i < blocks.length; ++i) {
            const kind = String(blocks[i] && blocks[i].kind ? blocks[i].kind : "")
            if (kind === "camera" || kind === "media" || kind === "audio" || kind === "image")
                return true
        }
        return false
    }

    // Caller sets width (or anchors); height follows as pane + footer.
    implicitWidth: 182
    implicitHeight: 6 + previewPane.height + 6 + 16 + 6
    radius: 8
    // Live = danger-red border; inactive = visible slate border so an off
    // tile reads as "inactive", not just black.
    border.color: root.active && LiveOutputService.live ? "#85261f" : "#2b2e3d"
    border.width: 1
    color: "#16171e"
    // Disabled screens dim everywhere — same model, same state.
    opacity: root.isEnabled ? 1 : 0.45
    Behavior on opacity { NumberAnimation { duration: 120 } }

    // 16:9 preview pane — always inset 6px, always the right aspect. On air:
    // the on-air slide drawn block-true (DesignPreview — the SAME renderer the
    // ReferencePane preview uses, so it "renders perfectly" by construction),
    // with the distributed frame taking over for media content. Off air: the
    // shared transparency checkerboard.
    Rectangle {
        id: previewPane
        x: 6
        y: 6
        width: parent.width - 12
        height: width * 9 / 16
        clip: true
        radius: 4
        color: "transparent"

        // Rendered on-air slide: the engine's own blocks through the shared
        // renderer. A transparent slide background shows DesignPreview's
        // checkerboard (the Edit canvas's convention) — same as the pane.
        DesignPreview {
            anchors.fill: parent
            visible: root.hasSlidePreview && !root.framePriority
            blocks: root.onAirSlide.blocks ?? []
            background: root.onAirSlide.background ?? "transparent"
        }

        // The distributed frame: wins when the on-air slide is media content
        // (a block render would be a name-on-a-tile placeholder).
        Image {
            anchors.fill: parent
            visible: root.framePriority || (root.hasFrame && !root.hasSlidePreview)
            source: root.frameSource
            fillMode: Image.Stretch
            asynchronous: false
            cache: false
            // The provider hands back an ARGB32 of the exact requested size;
            // stretch keeps the mapping 1:1 with the pane.
        }

        // Show checkerboard when nothing else to show
        Checkerboard {
            anchors.fill: parent
            visible: !root.hasFrame && !root.hasSlidePreview
            tileSize: 9
            shadeA: "#3a3c48"
            shadeB: "#25262f"
        }

        // (The LIVE pill was removed by request — the red border alone marks
        // the active output.)
    }

    // Footer: output name — anchored to the pane's bottom, full width.
    Item {
        x: 6
        y: previewPane.y + previewPane.height + 6
        width: parent.width - 12
        height: 16

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: "#e2e8f0"
            font.family: "Segoe UI"
            font.pixelSize: 13
            font.weight: Font.Medium
            text: root.name
        }
    }
}
