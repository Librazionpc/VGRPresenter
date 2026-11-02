import QtQuick
import VGRPresenterUI

SpeedometerGauge {
    id: root

    property int pct: 0
    property string reason: ""
    // The engine's own "background work is being held back" flag, which can be
    // set by a battery / on-air mode even when the throttle figure is 0.
    property bool throttled: false

    value: root.pct
    minValue: 0
    maxValue: 100
    unit: "%"
    caption: root.throttled ? (root.reason === "" || root.reason === "None"
                               ? qsTr("Throttling")
                               : qsTr("Throttling · %1").arg(root.reason))
                            : qsTr("Free running")
    dialColor: root.pct >= 50 ? Theme.danger
             : root.pct >= 5 ? Theme.warning
                             : Theme.success
}