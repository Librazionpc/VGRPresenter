import QtQuick
import VGRPresenterUI

// Small glyph drawn from primitives, matching the nav-rail icon set.
// kind: "sliders" | "spark" | "monitor" | "layoutTemplate" | "recordDot" | "grid"
Item {
    id: root
    property string kind: "sliders"
    property color color: Theme.iconMuted
    implicitWidth: 16
    implicitHeight: 16

    // sliders (General)
    Item {
        visible: root.kind === "sliders"
        anchors.fill: parent
        Rectangle { x: 1; y: 3; width: 14; height: 2; radius: 1; color: root.color }
        Rectangle { x: 8; y: 1; width: 2; height: 2; radius: 1; color: root.color }
        Rectangle { x: 1; y: 11; width: 14; height: 2; radius: 1; color: root.color }
        Rectangle { x: 4; y: 9; width: 2; height: 2; radius: 1; color: root.color }
    }

    // spark (Smart Config)
    Item {
        visible: root.kind === "spark"
        anchors.fill: parent
        Rectangle {
            width: 8; height: 8
            anchors.centerIn: parent
            color: root.color
            rotation: 45
        }
        Rectangle { x: 1; y: 1; width: 3; height: 3; radius: 1.5; color: root.color }
        Rectangle { x: 12; y: 12; width: 3; height: 3; radius: 1.5; color: root.color }
    }

    // monitor (Outputs / Audio & Video)
    Item {
        visible: root.kind === "monitor"
        anchors.fill: parent
        Rectangle { x: 1; y: 2; width: 14; height: 9; radius: 1.5; color: root.color }
        Rectangle { x: 6; y: 11; width: 4; height: 2; color: root.color }
        Rectangle { x: 3; y: 13; width: 10; height: 1.5; radius: 0.75; color: root.color }
    }

    // layoutTemplate (Styles) — Lucide's layout-template: a top band over
    // two lower cells, drawn as filled primitives like the other glyphs.
    Item {
        visible: root.kind === "layoutTemplate"
        anchors.fill: parent
        Rectangle { x: 1; y: 2; width: 14; height: 5; radius: 1; color: root.color }
        Rectangle { x: 1; y: 9; width: 6; height: 5; radius: 1; color: root.color }
        Rectangle { x: 9; y: 9; width: 6; height: 5; radius: 1; color: root.color }
    }

    // recordDot (Recording)
    Item {
        visible: root.kind === "recordDot"
        anchors.fill: parent
        Rectangle {
            anchors.centerIn: parent
            width: 10; height: 10; radius: 5
            color: root.color
        }
    }

    // grid (Plugins)
    Item {
        visible: root.kind === "grid"
        anchors.fill: parent
        Rectangle { x: 1; y: 1; width: 6; height: 6; radius: 1.5; color: root.color }
        Rectangle { x: 9; y: 1; width: 6; height: 6; radius: 1.5; color: root.color }
        Rectangle { x: 1; y: 9; width: 6; height: 6; radius: 1.5; color: root.color }
        Rectangle { x: 9; y: 9; width: 6; height: 6; radius: 1.5; color: root.color }
    }
}
