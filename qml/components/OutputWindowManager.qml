import QtQml
import QtQml.Models
import VGRPresenterUI

Instantiator {
    id: root
    model: OutputListModel

    // SELF-TEST ONLY (see OutputWindow.selfTestForceVisible): show every output window so a scenario can
    // grab them. Nothing in the app sets this.
    property bool selfTestForceVisible: false

    delegate: OutputWindow {
        id: win
        required property int index
        // One name per output, so a grab can address THIS output's window (SelfTest.grabWindow).
        objectName: "outputWindow_" + win.index
        selfTestForceVisible: root.selfTestForceVisible
        required property bool isEnabled
        required property string screenName
        required property string frameBuffer
        required property bool stayOnTop
        required property bool onAirOnly
        required property bool active
        required property string res

        outputIndex: win.index
        outputEnabled: win.isEnabled
        outputScreenName: win.screenName
        ownBuffer: win.frameBuffer
        outputStayOnTop: win.stayOnTop
        outputRes: win.res
        onAirOnly: win.onAirOnly
        outputActive: win.active
    }
}