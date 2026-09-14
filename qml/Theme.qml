pragma Singleton
import QtQuick

QtObject {
    id: theme

    // ---- Brand ----
    readonly property color accent: "#6C5CE7"
    readonly property color accentLight: "#9b8ff5"

    // ---- Semantic ----
    readonly property color danger: "#ff4d3d"
    readonly property color dangerLight: "#ff6b61"
    readonly property color success: "#4ade80"
    readonly property color successLight: "#7ee2a8"
    readonly property color warning: "#f5c26b"
    readonly property color info: "#4da6ff"

    // ---- Neutrals ----
    // Every value below is pulled directly from the Figma-to-Qt export
    // (VGRPresenter_Main_Screen.qml) rather than approximated by hand, so
    // there's exactly one place to read/import a color from instead of
    // re-typing hex values at each call site.
    readonly property color windowBg: "#0f1015"   // app root background
    readonly property color panelBg: "#12131a"    // app-header / side columns
    readonly property color rowBg: "#16171e"      // list rows, cards, hero bg
    readonly property color surface: "#15161d"    // Settings modal surface
    readonly property color card: "#191b24"       // Settings card background
    readonly property color inset: "#1a1c26"      // input/inset fields
    readonly property color chip: "#232733"       // neutral tag chips
    readonly property color border: "#232530"     // borders, dividers, hover bg
    readonly property color borderSubtle: "#2c3140"

    readonly property color textPrimary: "#e2e8f0"
    readonly property color textSecondary: "#8a94a6"
    readonly property color textMuted: "#5c6475"

    // ---- Nav rail (Settings) ----
    readonly property color iconMuted: "#8b93a7"      // unselected nav/glyph icon
    readonly property color navLabelMuted: "#c7cdd8"  // unselected nav item label
    readonly property color navChipBg: "#1b1d27"      // unselected icon-chip background
    readonly property color railDivider: "#23252f"    // nav rail edge line
    readonly property color footerSupport: "#6f7788"  // nav rail "Support" text
    readonly property color footerVersion: "#4a5060"  // nav rail version text
    readonly property color toggleOffTrack: "#2a2f3a" // ToggleSwitch off-state track

    // ---- Spacing scale ----
    readonly property int space1: 4
    readonly property int space2: 8
    readonly property int space3: 12
    readonly property int space4: 16
    readonly property int space5: 20
    readonly property int space6: 24
    readonly property int space8: 32

    // ---- Radius scale ----
    readonly property int radiusSm: 6
    readonly property int radiusMd: 8
    readonly property int radiusLg: 10
    readonly property int radiusXl: 16

    // ---- Typography ----
    readonly property string fontFamily: "Inter"
    readonly property int textXs: 9
    readonly property int textSm: 11
    readonly property int textMd: 13
    readonly property int textLg: 15
    readonly property int textXl: 20
    readonly property int textXxl: 22
}
