import QtQuick

// The edit canvas's copy of a slide's filter set — the same seven values the
// engine applies to the composed frame on air (rendering::SlideFilterValues,
// see Effects.hpp's ApplyFilters), so that what the canvas shows and what the
// HDMI/NDI output shows are one picture instead of two.
//
// Used as a layered item's effect:
//
//   Item {
//       layer.enabled: <filters are not identity>
//       layer.effect: SlideFilterEffect { filters: slideStore.current.filters }
//   }
//
// The shader itself is qml/shaders/slidefilter.frag, compiled by qsb at build
// time into qrc:/shaders/slidefilter.frag.qsb (see CMakeLists.txt). Qt 6 no
// longer accepts GLSL source strings here, and this is the same alternative it
// asks for. The layer's texture arrives as the shader's `source` sampler — that
// binding is Qt's own contract for layered effects, so it is not declared here.
//
// The QML properties below ARE the shader's uniforms: same names, one each, and
// the shader's OP ORDER matches ApplyFilters line for line. An identity filter
// set never reaches this file at all — the host item only enables its layer
// while a value differs from the default — which is what keeps an ordinary
// unfiltered canvas paying nothing.
ShaderEffect {
    id: root

    fragmentShader: "qrc:/shaders/slidefilter.frag.qsb"

    // The slide's filter map ({ hueRotate, invert, blur, grayscale, brightness,
    // contrast, saturate }), read value-by-value so a replaced map re-evaluates
    // every uniform.
    property var filters: ({})

    // The canvas's own size in STAGE pixels (StageGrid.w/h). Blur is specified
    // in stage pixels by the panel while the layer texture's UV space is 0–1, so
    // the two have to be divided — otherwise "blur 8" would mean eight pixels of
    // the layer's device resolution instead of eight of the slide's.
    property real stageWidth: 1920
    property real stageHeight: 1080

    property real hueRotate: root.filters ? (root.filters.hueRotate ?? 0) : 0
    property real invertAmount: root.filters ? (root.filters.invert ?? 0) : 0
    property real blurPx: root.filters ? (root.filters.blur ?? 0) : 0
    property real grayAmount: root.filters ? (root.filters.grayscale ?? 0) : 0
    // A MEDIA item's own filters carry sepia (FreeShow's media filter list); a
    // SLIDE's filter set has no sepia value, so this stays 0 there and the
    // shader's op is skipped — one shader, both callers.
    property real sepiaAmount: root.filters ? (root.filters.sepia ?? 0) : 0
    property real brightnessValue: root.filters ? (root.filters.brightness ?? 1) : 1
    property real contrastValue: root.filters ? (root.filters.contrast ?? 1) : 1
    property real saturationValue: root.filters ? (root.filters.saturate ?? 1) : 1

    property real stageW: root.stageWidth
    property real stageH: root.stageHeight
}
