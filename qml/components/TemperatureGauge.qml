import QtQuick
import VGRPresenterUI

// Package temperature as a dial: 0-100 °C, green under 70, amber to 85 (where
// the engine's thermal manager starts easing off) and red above it — the same
// line the config advice calls "already warm". A reading the platform cannot
// expose stays at 0 with a muted dial and says so, rather than inventing a
// temperature.
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
