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
    required property string styleId
    required property bool active
    required property bool isEnabled
    // The model object itself. Required-property delegates do NOT receive
    // the implicit `model` context object — reading `model.styleBackground`
    // without declaring it hits the typeof guard below and silently returns
    // the unstyled fallback forever (the "style bg never shows" bug).
    // Declared, it becomes a real dependency: StyleBackgroundRole
    // dataChanged re-evaluates styleBg and the pane repaints.
    required property var model

    // The distributed frame (image://livepreview re-fetched on every frameRev
    // bump). An output wearing a STYLE reads ITS OWN gated buffer
    // (frameBuffer role → its per-output pass under its own content rules);
    // an unstyled output mirrors the shared PREVIEW feed (single-pipeline
    // software renderer: the frame the preview output received IS what the
    // real outputs got). frameRev pulses on every main-pass frame; the
    // per-output passes run right after it in the same loop iteration, so
    // one rev serves both.
    readonly property string ownBuffer: (root.model !== null && root.model !== undefined)
                                        ? String(root.model.frameBuffer ?? "") : ""
    readonly property bool hasFrame: LiveOutputService.live && LiveOutputService.frameRev > 0
    readonly property url frameSource: {
        if (!hasFrame)
            return ""
        return ownBuffer !== ""
            ? "image://livepreview/" + ownBuffer + "?v=" + LiveOutputService.frameRev
            : "image://livepreview?v=" + LiveOutputService.frameRev
    }

    // The on-air slide AS DESIGN BLOCKS — the same data the preview pane's
    // DesignPreview draws, straight from the runtime's current slide (no
    // re-resolving the on-air title through a service, so every content kind
    // works: scripture, The Table, shows).
    readonly property var onAirSlide: LiveOutputService.onAirSlide
    readonly property bool hasSlidePreview: onAirSlide.valid === true
                                            && onAirSlide.blocks
                                            && onAirSlide.blocks.length > 0

    // ---- Taken input layer (the Media pane's click-to-preview) ----------
    // The service owns the PAL tap; this tile composites the feed as one
    // more IMAGE layer (the same way the style's PNG/JPG background rides
    // above the base colour), UNDER the on-air content — camera-behind-lyrics.
    readonly property bool inputTaken: LiveOutputService.inputLabel !== ""
    // Pointed at the provider THE WHOLE TIME the input is taken — NOT only
    // once frames are live: inputLive flips on the Image's own Ready status,
    // so gating the source on it deadlocked the warm-up (empty URL → no
    // request → no decode → no confirmation → the ring spun forever). The
    // provider answers a not-yet-flowing tap with a 1×1 transparent frame,
    // which reads as "not Ready enough" (sourceSize 1) and keeps the
    // placeholder up until real pixels arrive.
    readonly property url inputSource: inputTaken
        ? "image://videopreview/" + encodeURIComponent(LiveOutputService.inputLabel)
          + "?n=" + LiveOutputService.inputRev : ""

    // First-frame detection lives in the SERVICE (its pump polls the
    // provider's decode cache) — the previous tile-side confirm round-trip
    // (Image status → confirmInputFrame() → inputRev bump → THIS URL) was a
    // binding loop; QML only reads here now.

    // This output's own style background ({ color, image, hasImage }) —
    // "for THAT output": the tile the output wears paints ITS style's look,
    // not the active output's. Delivered as the StyleBackground ROLE (per
    // OUTPUT, so a re-assignment on another output can't repaint this tile)
    // — a Q_INVOKABLE is opaque to the QML engine and would capture its
    // value once at creation and never move again.
    //
    // The role's dataChanged pulses are what repaint the tile, but a style
    // background IMAGE that appears on disk WITHOUT a model mutation (a pick
    // in the Styles dialog before Save, styles hydrating after boot) needs a
    // nudge: on activeStyleChanged the model re-emits dataChanged for the
    // whole roster, so bumping this counter re-reads the role — with a
    // fresh QFile::exists() inside — and the pane repaints.
    property int stylePulse: 0
    Connections {
        target: OutputListModel
        function onActiveStyleChanged() { root.stylePulse++ }
    }
    readonly property var styleBg: {
        void root.stylePulse
        // model is a DECLARED required property (above), so this read is a
        // real role dependency — no silent-undefined branch. The guard stays
        // for the transient early-construction window only.
        const bg = (root.model !== null && root.model !== undefined)
            ? root.model.styleBackground : null
        return (bg && typeof bg === "object") ? bg
            : { color: "transparent", image: "", hasImage: false }
    }

    // Does the style paint a REAL background — a solid colour or an image?
    // Everything transparent reads as "no background" (DesignPreview.isClear's
    // convention: "transparent", "" and a 0-alpha colour alike), so a
    // layout-only template style (transparent bg, no image) keeps the
    // checkerboard — the output IS transparent on air. Qt.color can throw on
    // a malformed string; the try keeps the binding alive rather than
    // killing the pane's whole background chain.
    function isClearColor(c) {
        if (c === undefined || c === null || c === "" || c === "transparent")
            return true
        try { return Qt.color(c).a === 0 } catch (e) { return false }
    }
    readonly property bool styled: !isClearColor(root.styleBg.color)
                                   || root.styleBg.hasImage === true

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
    // with the distributed frame taking over for media content. The style's
    // own background (colour + image) paints UNDER everything — off air a
    // styled output shows ITS look, not the transparency checkerboard.
    Rectangle {
        id: previewPane
        x: 6
        y: 6
        width: parent.width - 12
        height: width * 9 / 16
        clip: true
        radius: 4
        // The style's colour (the engine's OutputStyleSpec): the pane's base.
        color: root.styleBg.color !== "" ? root.styleBg.color : "transparent"
        // The style's background IMAGE above the colour, under the content —
        // the engine paints it behind every frame; the block-only preview
        // cannot, so the tile does (cover-fit, like SceneBuilder's CoverRect).
        Image {
            anchors.fill: parent
            visible: root.styleBg.hasImage
            source: root.styleBg.hasImage ? "file:///" + root.styleBg.image : ""
            fillMode: Image.PreserveAspectCrop
        }

        // The TAKEN INPUT's live frames — the camera/screen feed as one more
        // image layer, under the on-air content (camera-behind-lyrics), above
        // the style background. Fill the pane: feeds are 16:9-shaped like the
        // pane itself. Placeholder glyphs from the shared pane's art keep the
        // warm-up seconds honest.
        Image {
            id: inputImage
            anchors.fill: parent
            visible: root.inputTaken
            source: root.inputSource
            fillMode: Image.PreserveAspectCrop
            cache: false   // every rev IS a new frame
            asynchronous: false
        }

        // Warm-up placeholder while an input is taken but no frame has
        // decoded yet (the shared pane's camera ring art, inline).
        Item {
            anchors.centerIn: parent
            visible: root.inputTaken && !LiveOutputService.inputLive
            width: 48; height: 48

            Rectangle {
                anchors.centerIn: parent
                width: 34; height: 34; radius: 17
                color: "transparent"
                border.width: 2.4
                border.color: "#6c5ce7"
            }
            Rectangle {
                anchors.centerIn: parent
                width: 10; height: 10; radius: 5
                color: "#6c5ce7"
            }
        }

        // Rendered on-air slide: the engine's own blocks through the shared
        // renderer, with its own checkerboard OFF — a clear background here
        // means "the style's colour/image underneath shows through", and the
        // tile paints that (the pane below).
        DesignPreview {
            anchors.fill: parent
            visible: root.hasSlidePreview && !root.framePriority
            // Checkers only when NOTHING paints a background here: an
            // unstyled output's clear slide shows the transparency
            // convention, a styled output's clear slide shows the STYLE's
            // colour/image through (the pane under this item).
            showCheckerboard: !root.styled
            // The style's own image rides the block render (mirrors the
            // engine: the style's colour OR the slide's composed colour,
            // then the style image, then content — a clear slide background
            // leaves the style's colour underneath, exactly as on air).
            backgroundImage: root.styleBg.hasImage ? root.styleBg.image : ""
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

        // Transparency checkerboard — ONLY for an unstyled output with
        // nothing on air AND no taken input (the input layer IS content:
        // over an unstyled output the checker painted ON TOP of the feed
        // and the picture read broken/dimmed). A styled output shows its
        // colour/image instead ("black bg + image" looked unstyled before).
        Checkerboard {
            anchors.fill: parent
            visible: !root.hasFrame && !root.hasSlidePreview && !root.styled
                     && !root.inputTaken
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

        // Taken-input marker: purple dot + label, so the layer's presence
        // reads even when the feed is hidden behind on-air content.
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            visible: root.inputTaken

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: "#6c5ce7"
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, 110)
                elide: Text.ElideRight
                color: "#9aa0b5"
                font.family: "Segoe UI"
                font.pixelSize: 10
                text: LiveOutputService.inputLabel
            }
        }
    }
}
