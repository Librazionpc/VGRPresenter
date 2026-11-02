import QtQuick

ShaderEffect {
    id: root

    fragmentShader: "qrc:/shaders/slidefilter.frag.qsb"

    property var filters: ({})

    property real stageWidth: 1920
    property real stageHeight: 1080

    property real hueRotate: root.filters ? (root.filters.hueRotate ?? 0) : 0
    property real invertAmount: root.filters ? (root.filters.invert ?? 0) : 0
    property real blurPx: root.filters ? (root.filters.blur ?? 0) : 0
    property real grayAmount: root.filters ? (root.filters.grayscale ?? 0) : 0
    property real sepiaAmount: root.filters ? (root.filters.sepia ?? 0) : 0
    property real brightnessValue: root.filters ? (root.filters.brightness ?? 1) : 1
    property real contrastValue: root.filters ? (root.filters.contrast ?? 1) : 1
    property real saturationValue: root.filters ? (root.filters.saturate ?? 1) : 1

    property real stageW: root.stageWidth
    property real stageH: root.stageHeight
}