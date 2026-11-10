import QtQuick
import QtMultimedia
import VGRPresenterUI

// THE ONE WAY THIS APP DRAWS A CAMERA OR SCREEN THAT IS TAKEN TO AN OUTPUT.
//
// The tap's own newest frame is handed to this surface's VideoOutput by ONE worker thread
// (LiveOutputService.startInputFeed) as it arrives — a straight copy into a QVideoFrame, painted on the
// GPU. The output window and every monitor tile show the same picture at the camera's own rate, and the
// interface thread does nothing per frame. The old way — `image://videopreview`, a JPEG the tap
// re-encodes ~15 times a second and each consumer decodes again on the interface thread — is what made
// the HDMI picture lag.
//
// WITHOUT A GPU IT STILL WORKS: a QML scene on the software backend cannot paint a VideoOutput, so
// `pathGpu` is false there and the parent shows its own image path (the JPEG provider) instead — the
// same picture, paying the cost that path always paid.
//
// A parent uses it like this:
//     LiveInputSurface { id: inputSurface; anchors.fill: parent; active: root.inputTaken }
//     <its image path>  { visible: root.inputTaken && !inputSurface.pathGpu
//                         source:  inputSurface.pathGpu ? "" : root.inputSource }
Item {
    id: root

    // True while a taken input is the picture this surface should show.
    property bool active: false
    // Cameras fill the output (cropping to its aspect), like the image path always did.
    property int fillMode: VideoOutput.PreserveAspectCrop

    readonly property bool gpuVideo: GraphicsInfo.api !== GraphicsInfo.Software
    // Camera and screen are fed by the worker thread; an NDI input is fed as its frames arrive.
    readonly property bool feedKind: LiveOutputService.inputKind === "camera"
                                     || LiveOutputService.inputKind === "screen"
                                     || LiveOutputService.inputKind === "ndi"
    // THE ONE QUESTION THE PARENT ASKS: is the GPU surface the picture? (else it draws the image path)
    readonly property bool pathGpu: root.active && root.gpuVideo && root.feedKind

    // Attached once per surface: the service keeps one entry per sink and forwards every frame to it.
    function attach() {
        if (root.pathGpu)
            LiveOutputService.attachInputSink(videoOut.videoSink)
    }
    onPathGpuChanged: root.attach()
    Component.onCompleted: root.attach()

    VideoOutput {
        id: videoOut
        objectName: "liveInputVideo"
        anchors.fill: parent
        visible: root.pathGpu
        fillMode: root.fillMode
    }
}
