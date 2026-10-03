import QtQuick
import VGRPresenterUI

// One labeled usage meter — label left, percentage right, rounded bar
// underneath filling to the percentage. Shared by the Settings screens that
// show resource usage (General's Resource profile row, Smart Config's
// Resource budgets, the resource-health modal); extracted so the meter visual
// can't drift between them. Hand `label`, `pct`, and a width; the height is
// intrinsic.
//
// CAP-VS-USAGE: pass `capPct` (0-100) as well and the bar becomes two layers —
// a soft "allowance" band reaching the cap, the bright fill showing what is
// actually used now, and a tick marking the cap — with the readout reading
// `used% / cap%`. That is what Smart Config's budgets want: a live number
// against the share the profile allows. Leave it at -1 (the default) for a
// plain single-value meter, which is what every other caller passes.
Item {
    id: root

    property string label: ""
    property int pct: 0
    // -1 = a plain usage meter (the historic shape); >= 0 draws the allowance
    // band + cap tick and prints "pct% / cap%".
    property int capPct: -1
    readonly property bool capped: root.capPct >= 0
    // Accent by default; a health-style meter (e.g. free disk space) can
    // pass Theme.success instead. With a cap it is the USAGE fill's colour;
    // the allowance band stays a quiet wash so the two never read as one.
    property color barColor: Theme.accent
    // Replaces the right-hand percentage when the caller has something better
    // to say — e.g. "Not measured · cap 80%" where the platform cannot report
    // a usage figure at all. Empty keeps the numeric readout.
    property string readout: ""

    implicitWidth: 200
    implicitHeight: 30

    Text {
        anchors.top: parent.top
        text: root.label
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
        width: parent.width - capValue.width - 12
    }
    Text {
        id: capValue
        anchors.top: parent.top
        anchors.right: parent.right
        text: root.readout !== ""
              ? root.readout
              : (root.capped ? root.pct + "% / " + root.capPct + "%" : root.pct + "%")
        color: Theme.textPrimary
        font.family: Theme.fontFamily
        font.pixelSize: Theme.textSm
    }
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 6
        radius: 3
        color: Theme.inset

        // The allowance the cap represents — a quiet wash behind the usage.
        Rectangle {
            visible: root.capped
            width: root.capped ? parent.width * Math.min(100, Math.max(0, root.capPct)) / 100 : 0
            height: parent.height
            radius: 3
            color: Qt.alpha(Theme.accent, 0.22)
        }

        // What is actually used now.
        Rectangle {
            width: parent.width * Math.min(100, Math.max(0, root.pct)) / 100
            height: parent.height
            radius: 3
            color: root.barColor
            Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutQuad } }
        }

        // The cap line itself, so the bright fill reads against its limit.
        Rectangle {
            visible: root.capped
            x: root.capped
               ? Math.max(0, Math.min(parent.width - width,
                                      parent.width * Math.min(100, Math.max(0, root.capPct)) / 100 - width / 2))
               : 0
            width: 2
            height: parent.height
            radius: 1
            color: Theme.textMuted
        }
    }
}
