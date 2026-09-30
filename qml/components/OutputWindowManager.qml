import QtQml
import QtQml.Models
import VGRPresenterUI

// Owns one OutputWindow (qml/components/OutputWindow.qml) per row in
// OutputListModel — GO LIVE's real destination on a physical/HDMI screen,
// separate from MonitorWall.qml/OutputMonitorTile.qml's small in-app
// preview tiles. Instantiator (not Repeater): its delegates don't need a
// visual parent item, which is exactly what a top-level Window delegate
// needs — this is its documented use case.
//
// Every row gets a delegate (cheap: an OutputWindow with visible: false is
// just a hidden native window, no rendering cost); each instance decides
// its OWN visibility from outputEnabled/outputScreenName, so a row that
// isn't enabled or isn't bound to a screen never shows anything. This
// mirrors MonitorWall.qml's own model: ALL enabled+bound outputs show
// simultaneously while live, not just the one open in Settings/Edit.
Instantiator {
    id: root
    model: OutputListModel

    delegate: OutputWindow {
        id: win
        required property int index
        required property bool isEnabled
        required property string screenName
        required property string frameBuffer
        required property bool stayOnTop
        required property bool fullscreenOutput
        required property bool active

        outputIndex: win.index
        outputEnabled: win.isEnabled
        outputScreenName: win.screenName
        ownBuffer: win.frameBuffer
        outputStayOnTop: win.stayOnTop
        outputFullscreen: win.fullscreenOutput
        outputActive: win.active
    }
}
