import QtQml
import QtQml.Models
import VGRPresenterUI

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