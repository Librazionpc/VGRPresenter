import QtQuick
import VGRPresenterUI

// Settings · Resource health — the live-health modal (Smart Config's "Live
// health" action). One screen's worth of honesty about the machine the engine
// is running on: two dials (how hard it is throttling, how warm it is), the
// live CPU / GPU / memory readings, and what the adaptive runtime is doing
// about all of it.
//
// Everything comes from TelemetryService.health / .engine, which sample the
// platform and the runtime on the poll timer — so this modal is LIVE (it keeps
// moving while open) and reads "Not measured" wherever the machine cannot
// answer rather than inventing a number. Nothing here computes health itself.
ModalCard {
    id: root

    title: qsTr("Resource health")
    subtitle: qsTr("What this machine is doing right now while VGR runs.")
    cardWidth: 600
    showSave: false
    cancelText: qsTr("Close")
    onCancelled: root.close()

    readonly property var health: TelemetryService.health
    readonly property var engine: TelemetryService.engine
    readonly property bool hasGpu: health.gpuPct !== undefined && health.gpuPct >= 0
    readonly property bool hasTemp: health.tempC !== undefined && health.tempC >= 0
    readonly property bool cpuMeasured: health.cpuPct !== undefined && health.cpuPct >= 0
    readonly property bool memMeasured: health.memUsedPct !== undefined && health.memUsedPct >= 0

    // The two dials, side by side.
    Row {
        id: gauges
        width: parent.width
        spacing: Theme.space4

        ThrottleGauge {
            width: (gauges.width - gauges.spacing) / 2
            pct: root.health.throttlePct === undefined ? 0 : root.health.throttlePct
            reason: root.health.reason === undefined ? "" : root.health.reason
            throttled: root.health.throttling === true
        }

        TemperatureGauge {
            width: (gauges.width - gauges.spacing) / 2
            celsius: root.health.tempC === undefined ? -1 : root.health.tempC
        }
    }

    Text {
        width: parent.width
        text: {
            if (root.health.throttling !== true)
                return qsTr("The engine is free to use the machine. Nothing is holding it back.")
            const reason = root.health.reason
            if (reason === "Thermal")
                return qsTr("The CPU is running below its nominal clock, so the engine is easing off to keep it cool.")
            if (reason === "Battery")
                return qsTr("Running on battery — background work is paused so the show lasts.")
            if (reason === "Presentation")
                return qsTr("On air — background work is paused so nothing competes with the live output.")
            return qsTr("The engine is easing off background work right now.")
        }
        color: Theme.textSecondary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
        wrapMode: Text.WordWrap
    }

    // ---- Live readings ----
    LabeledMeter {
        width: parent.width
        label: qsTr("CPU")
        pct: root.cpuMeasured ? root.health.cpuPct : 0
        barColor: Theme.accent
        readout: root.cpuMeasured ? "" : qsTr("Not measured")
    }

    LabeledMeter {
        width: parent.width
        label: root.hasGpu && root.health.gpuName !== "" ? qsTr("GPU · %1").arg(root.health.gpuName) : qsTr("GPU")
        pct: root.hasGpu ? root.health.gpuPct : 0
        barColor: Theme.accent
        readout: root.hasGpu ? "" : qsTr("Not measured")
    }

    LabeledMeter {
        width: parent.width
        label: qsTr("Memory")
        pct: root.memMeasured ? root.health.memUsedPct : 0
        barColor: Theme.accent
        readout: root.memMeasured ? "" : qsTr("Not measured")
    }

    Text {
        visible: !root.hasGpu
        width: parent.width
        text: qsTr("GPU load is not measured on this machine's driver.")
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
        wrapMode: Text.WordWrap
    }

    // ---- What the engine decided to do about it ----
    Rectangle {
        width: parent.width
        height: engineCol.height + Theme.space3 * 2
        radius: Theme.radiusMd
        color: Theme.inset

        Column {
            id: engineCol
            x: Theme.space3
            y: Theme.space3
            width: parent.width - Theme.space3 * 2
            spacing: 4

            Text {
                text: qsTr("Engine management")
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                text: qsTr("%1 mode · %2 layer · %3 quality")
                    .arg(root.engine.mode === undefined ? "—" : root.engine.mode)
                    .arg(root.engine.layer === undefined ? "—" : root.engine.layer)
                    .arg(root.engine.quality === undefined ? "—" : root.engine.quality)
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: qsTr("%1 worker threads · GPU cap %2% · CPU cap %3%")
                    .arg(root.engine.workers === undefined ? "—" : root.engine.workers)
                    .arg(root.engine.gpuCapPct === undefined ? "—" : root.engine.gpuCapPct)
                    .arg(root.engine.cpuCapPct === undefined ? "—" : root.engine.cpuCapPct)
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: qsTr("Memory budget %1 MB").arg(root.engine.memoryBudgetMb === undefined ? "—" : root.engine.memoryBudgetMb)
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.textXs
                wrapMode: Text.WordWrap
            }
        }
    }

    Text {
        width: parent.width
        text: qsTr("Throttling compares the CPU's real clock with its nominal one and watches the package temperature; anything the machine cannot report is shown as not measured rather than as zero.")
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textXs
        wrapMode: Text.WordWrap
    }
}
