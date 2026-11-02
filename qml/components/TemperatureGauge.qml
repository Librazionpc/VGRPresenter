import QtQuick
import VGRPresenterUI

SpeedometerGauge {
    id: root

    property int celsius: -1
    readonly property bool measured: root.celsius >= 0

    value: root.measured ? root.celsius : 0
    minValue: 0
    maxValue: 100
    unit: "°C"
    caption: root.measured ? qsTr("Package temp") : qsTr("Not measured")
    dialColor: !root.measured ? Theme.textMuted
             : root.celsius >= 85 ? Theme.danger
             : root.celsius >= 70 ? Theme.warning
                                  : Theme.success
}