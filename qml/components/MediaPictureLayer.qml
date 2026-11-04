import QtQuick
import VGRPresenterUI

// ONE PICTURE BLOCK, DRAWN ONE WAY — by the template editor, the Show Center, the monitor tiles and
// everything else that paints a design with QML. The engine's own renderer reads the very same fields
// (`meta.media`: crop, flip, the colour filters and the opacity; the block style's `mediaFit`), so a
// picture looks the same on the editor canvas, on the preview and on the output.
//
// This used to live inline in the editor only. The shared DesignPreview drew a plain Image that read
// none of it: a template picture set to 40% opacity was dim in the editor and on the output, and full
// strength in the Show Center and on the monitor — "it is not rendering the image correctly".
//
//   MediaPictureLayer { anchors.fill: parent; meta: block.meta; fit: block.style.mediaFit }
Item {
    id: root

    // The block's `meta` ({ src, media: { crop, flipH, flipV, filter: { opacity, blur, … } } }).
    property var meta: ({})
    // "contain" | "cover" | "fill" | "blur" — the block style's mediaFit.
    property string fit: "contain"

    readonly property string src: root.meta ? String(root.meta.src ?? "") : ""
    readonly property string pictureUrl: root.src === ""
        ? "" : "image://mediathumb/500/-1/" + encodeURIComponent(root.src)
    readonly property var media: root.meta && root.meta.media ? root.meta.media : ({})
    readonly property var crop: root.media.crop || ({ top: 0, right: 0, bottom: 0, left: 0 })
    readonly property var filter: root.media.filter || ({})
    readonly property bool flipH: root.media.flipH === true
    readonly property bool flipV: root.media.flipV === true
    readonly property bool cropActive: (root.crop.top ?? 0) + (root.crop.right ?? 0)
                                       + (root.crop.bottom ?? 0) + (root.crop.left ?? 0) > 0
    readonly property bool filterActive: {
        const f = root.filter
        return (f.hueRotate ?? 0) !== 0 || (f.invert ?? 0) !== 0 || (f.blur ?? 0) !== 0
            || (f.grayscale ?? 0) !== 0 || (f.sepia ?? 0) !== 0
            || (f.brightness ?? 1) !== 1 || (f.contrast ?? 1) !== 1
            || (f.saturate ?? 1) !== 1 || (f.opacity ?? 1) !== 1
    }
    readonly property bool canShader: GraphicsInfo.api !== GraphicsInfo.Software

    // What the picture is doing, for the callers that draw a placeholder until it has loaded.
    readonly property alias status: picture.status
    readonly property alias sourceSize: picture.sourceSize
    readonly property bool ready: picture.status === Image.Ready && picture.sourceSize.width > 1

    clip: true

    // The fourth fit: a blurred, dimmed copy fills the box and the sharp picture sits on top of it.
    Image {
        anchors.fill: parent
        visible: root.fit === "blur" && picture.status === Image.Ready
        asynchronous: true
        cache: false
        fillMode: Image.PreserveAspectCrop
        source: root.pictureUrl
        layer.enabled: visible && root.canShader
        layer.effect: SlideFilterEffect {
            filters: ({ blur: 24, brightness: 0.85 })
            stageWidth: StageGrid.w
            stageHeight: StageGrid.h
        }
    }

    Item {
        anchors.fill: parent
        opacity: root.filter.opacity ?? 1
        layer.enabled: root.filterActive && root.canShader
        layer.effect: SlideFilterEffect {
            filters: root.filter
            stageWidth: StageGrid.w
            stageHeight: StageGrid.h
        }

        Image {
            id: picture
            anchors.fill: parent
            asynchronous: true
            cache: false
            fillMode: root.fit === "cover" ? Image.PreserveAspectCrop
                    : root.fit === "fill" ? Image.Stretch
                    : Image.PreserveAspectFit
            sourceClipRect: {
                if (!root.cropActive || picture.sourceSize.width <= 1)
                    return Qt.rect(0, 0, 0, 0)
                const w = picture.sourceSize.width
                const h = picture.sourceSize.height
                const l = (root.crop.left ?? 0) / 100
                const r = (root.crop.right ?? 0) / 100
                const t = (root.crop.top ?? 0) / 100
                const b = (root.crop.bottom ?? 0) / 100
                return Qt.rect(w * l, h * t, w * (1 - l - r), h * (1 - t - b))
            }
            mirror: root.flipH
            transform: Scale {
                origin.x: picture.width / 2
                origin.y: picture.height / 2
                xScale: 1
                yScale: root.flipV ? -1 : 1
            }
            source: root.pictureUrl
            visible: status === Image.Ready && sourceSize.width > 1
        }
    }
}
