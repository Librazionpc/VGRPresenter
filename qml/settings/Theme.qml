pragma Singleton
import QtQuick

QtObject {
    id: theme

    // ---- Theme (Settings > General > Appearance > Theme) ----
    // The ENGINE owns the choice ("appearance.theme": "dark" | "light"). This
    // singleton only turns it into token values, so every token below is a LIVE
    // binding: picking Light in Settings recolours every surface that reads a
    // Theme token, with no restart. Until the engine is up the choice reads
    // undefined and the app stays dark (its default).
    //
    // Every surface follows the choice now, including the app chrome (title
    // bar, tab strip, menu bar, dropdowns): those files used to carry their
    // own dark hex literals because this singleton was registered as an
    // ORDINARY type rather than a singleton (see QT_QML_SINGLETON_TYPE in
    // CMakeLists.txt), so `Theme.*` resolved to undefined and they isolated
    // themselves. With the registration fixed they read tokens like any
    // other surface.
    readonly property string themeName: {
        const v = SettingsService.values["appearance.theme"]
        return v === undefined || v === null ? "dark" : String(v)
    }
    readonly property bool dark: themeName !== "light"

    // ---- Brand ----
    // The accent is the user's choice (Settings > General > Accent color): the ENGINE keeps it and SettingsService hands the colours over,
    // so every Theme.accent / accentLight / accentSoft in the interface follows the choice. (Purple until the engine is up.)
    readonly property color accent: SettingsService.accent
    readonly property color accentLight: SettingsService.accentLight
    readonly property color accentSoft: Qt.alpha(SettingsService.accent, 0.15)   // the tint behind a selected chip or row

    // ---- Semantic ----
    // Deliberately the SAME in both themes: these are fixed status hues, and
    // every one of them is drawn on its own tinted or neutral chip rather than
    // directly on the page ground. (The on-air output windows do not read Theme
    // at all, so a light app theme can never lighten the stage output.)
    // A FAULT RED HAS TO READ AS RED. Both of these are a PURE red hue — green
    // and blue at the same value, which is what keeps the colour on the red axis
    // instead of drifting toward coral/orange as they differ. The pair was
    // #ff4d3d / #ff6b61, whose green sat well above its blue: every Delete
    // button and every error line in the app (they all read these two tokens,
    // nowhere is a fault red spelled out) came out orange, which is not what a
    // destructive action should look like. Change the HUE here — not in the
    // callers — and the whole fault family follows: all the delete buttons in
    // the edit and confirmation dialogs, the failed steps, the error text and
    // the low-disk warnings.
    readonly property color danger: "#ff2b2b"
    readonly property color dangerLight: "#ff6b6b"
    // GO LIVE is red in BOTH themes (a live switch reads as red the way a
    // recording dot does), so these are fixed semantic hues, not neutrals.
    readonly property color liveBg: "#b03630"        // GO LIVE pill, at rest
    readonly property color liveBgOnAir: "#7a2a24"   // GO LIVE pill, on air
    //
    // RED MEANS ONE OF TWO THINGS HERE, AND THEY ARE NOT INTERCHANGEABLE:
    //   A FAULT — an error, a destructive action, a failed step, low disk — is
    //   `danger` as a fill, dot or border, and `dangerLight` as ink drawn on a
    //   dark ground.
    //   A LIVE STATE — recording, a running flow, an on-air output or media,
    //   the GO LIVE pill itself — is NOT a fault. Its SURFACE is `liveBg` while
    //   it is live and `liveBgOnAir` once it is actually on air; the INK on that
    //   surface is `dangerLight`, which is what AppHeader's GO LIVE pill has
    //   always used.
    // Ten files used to carry their own hand-picked version of these reds (six
    // near-copies of danger/dangerLight — #ff8d7f, #ff9a90, #ff8a7e, #ff8a80,
    // #ffb4ab, #ff5d5d — plus #c8321e, #85261f and #e05a4e copying the live
    // pair), which is how one "recording" state could read red three ways on
    // three screens. The near-black "armed" surface washes that sweep left
    // alone (a surface tint is not the live red) have a block of their own now —
    // see "Armed (staged) surfaces" further down.
    readonly property color success: "#4ade80"
    readonly property color successLight: "#7ee2a8"
    readonly property color warning: "#f5c26b"
    // The amber family was missing the one member every other family has: the
    // ink drawn ON a dark ground (dangerLight, successLight, infoLight). It
    // carries two roles because both read as the same tone — a condition the
    // operator has to act on (dropped frames, free space under 20%) and the flow
    // runner's STAGED dot/label, the step that is armed but not yet live (live is
    // the red above, never this amber). It was hand-picked as "#ffb454" four
    // times, across FunctionsPane and RecordingScreen.
    readonly property color warningLight: "#ffb454"
    // The two STATE golds. Neither is `accent`: the accent is the user's own
    // colour choice and moves when they change it, while these two say something
    // about the data on screen and must not move with it.
    //   highlightInk a SEARCH HIT: the matched words in a result row, and the
    //          spell-fix hint line under the box. It is interpolated into the
    //          RichText markup Qt's StyledText actually honours (a colored
    //          <font>, not a background span — the background was silently
    //          dropped), and its value is a CONTRACT, not a taste: the pixel
    //          self-test (tools/selftest/gold_pixels.mjs,
    //          run_pane_highlight_test.sh) counts exactly this gold, and
    //          Main.qml strips the same literal back out of a rendered row.
    //          Changing it here means checking those three readers.
    //   markedInk    the operator's OWN mark: a verse they highlighted (its verse
    //          number and its note button), a note's reference line, a favorited
    //          template's star. A mark a person placed — which is why it is not
    //          `warning`, even though the two golds sit a shade apart.
    readonly property color highlightInk: "#ffd54a"
    readonly property color markedInk: "#f5c542"
    readonly property color info: "#4da6ff"
    readonly property color infoLight: "#8ecbff"
    // The LEVEL-METER scale — the gradient every meter in the app draws (the
    // pro-audio dialogs' channel strips, LevelTrack, OutputMonitorTile): quiet
    // cyan → loud green → hot amber → clip red. A DEVICE scale, so like the hues
    // above it is the same in both themes (it is meant to read as a hardware
    // meter, and it is drawn on a strip of its own). It lives HERE because those
    // four values used to be copied into all three files, which each carried a
    // comment apologising for it ("three copies, three ways to drift").
    readonly property color meterQuiet: "#00c8c8"   // signal present, low level
    readonly property color meterLoud: "#00ff32"    // healthy speech/music
    readonly property color meterHot: "#ffc800"     // near the top of the scale
    readonly property color meterClip: "#c80000"    // at/over 0 dBFS

    // ---- Channel state (one vocabulary for off / trimmed / at unity) ----
    // WHICH state a channel is in is Db.channelState (a gain of 0 is off, 1 is
    // unity, between is a trim); this is only how each one LOOKS, named here so
    // the audio dialogs' gain knob, the Media pane's cards and L/R pills, and
    // the monitor tiles cannot each invent an answer of their own — they used to,
    // and the result was an amber ring in one place, a red slash in another, and
    // no way anywhere to see that a channel had been trimmed.
    //   OFF     the channel passes nothing. INACTIVE chrome, never a warning:
    //           an off channel is not a fault (danger stays for faults), which
    //           is why it is the muted tone and not a red.
    //   TRIMMED the operator moved this channel's trim off unity — the warning
    //           tone, because a deliberate attenuation is easy to forget.
    //   UNITY   untouched: the normal state.
    readonly property color channelOff: textMuted
    readonly property color channelTrimmed: warning
    readonly property color channelUnity: success

    // The two calls a surface needs with a channel's gain in hand — Theme does
    // NOT reach into Db itself (a singleton calling a sibling singleton for a
    // decision is the linter's "Member not found on type Db" ambiguity, and it
    // keeps the two vocabularies separable):
    //   readonly property string state: Db.channelState(gain)
    //   border.color: Theme.channelStateColor(state)
    function channelStateColor(state) {
        return state === "off" ? channelOff
             : state === "trimmed" ? channelTrimmed
             : channelUnity
    }

    // ---- Armed (staged) surfaces ----
    // A control that is STAGED for a state wears a near-black wash of the hue it
    // is about to become, with that hue's ink on top. The wash is a dim ground,
    // never the live or fault hue itself: the real red belongs to the thing that
    // is actually on air, and the real blue to the action once it has been taken.
    // The red sweep walked past these on purpose (a surface TINT is not the live
    // red), which left them hand-picked per file — the flow runner's Run button,
    // Stop button and state pills, the recording screen's live pill and Start
    // button, a taken media key in a monitor tile — until no two screens agreed.
    //   armedReadyBg  the blue wash of an action that is ready to be taken: a
    //          flow's Run button, a media key already taken in a monitor tile.
    //          `armedReadyInk` is that pair's ink, on all three of its sites.
    //   armedStopBg   the warm wash of a control armed to STOP: that same Run
    //          button while its flow is the one running (it reads "Stop" there),
    //          the flow runner's own Stop button, the recording screen's Start
    //          button once it is recording.
    //   armedLiveBg   the warm wash behind a state pill that IS live: the flow
    //          runner's RUNNING/PAUSED pill, the recording screen's RECORDING
    //          pill. Its ink is `dangerLight`, the live ink — the same pair the
    //          GO LIVE pill is built from.
    // The grounds left beside them are single-use, so they stay literals until a
    // second screen wants them: #3a1410 a failed step's pill, #173326 a completed
    // one's.
    readonly property color armedReadyBg: "#1d2a4d"
    readonly property color armedReadyInk: "#8fb4ff"
    readonly property color armedStopBg: "#241a19"
    readonly property color armedLiveBg: "#2a1210"

    // ---- Neutrals ----
    // The dark values are the Figma-to-Qt export's own, pulled directly
    // (VGRPresenter_Main_Screen.qml) rather than approximated by hand. The light
    // counterparts keep the same relationships - ground darkest, cards above it,
    // insets recessed - inverted for a light ground.
    readonly property color windowBg: dark ? "#0f1015" : "#f4f5f8"   // app root background
    readonly property color panelBg: dark ? "#12131a" : "#ffffff"    // app-header / side columns
    readonly property color rowBg: dark ? "#16171e" : "#ffffff"      // list rows, cards, hero bg
    readonly property color surface: dark ? "#15161d" : "#ffffff"    // Settings modal surface
    readonly property color card: dark ? "#191b24" : "#ffffff"       // Settings card background
    readonly property color inset: dark ? "#1a1c26" : "#f1f2f6"      // input/inset fields
    readonly property color chip: dark ? "#232733" : "#e8eaf0"       // neutral tag chips
    readonly property color border: dark ? "#232530" : "#e2e5ec"     // borders, dividers, hover bg
    readonly property color borderSubtle: dark ? "#2c3140" : "#d7dbe4"
    // ---- App chrome (title bar, tab strip, menu bar, dropdowns) ----
    // These close the last gap Light mode had: the chrome files at one point
    // hardcoded their own dark shades, so picking Light recoloured the settings
    // screens and left the header/menus dark. Their families live here now.
    readonly property color hoverBg: dark ? "#1c1d26" : "#eceef3"     // pill / menu-row hover wash
    readonly property color activeBg: dark ? "#1e1f29" : "#e9ebf1"    // active tab pill
    readonly property color iconChrome: dark ? "#b4bccb" : "#5a6272"  // header icon at rest

    readonly property color textPrimary: dark ? "#e2e8f0" : "#16181f"
    readonly property color textSecondary: dark ? "#8a94a6" : "#5b6472"
    readonly property color textMuted: dark ? "#5c6475" : "#7b8496"

    // ---- Nav rail (Settings) ----
    readonly property color iconMuted: dark ? "#8b93a7" : "#6b7484"      // unselected nav/glyph icon
    readonly property color navLabelMuted: dark ? "#c7cdd8" : "#3a4150"  // unselected nav item label; also the app's mid-emphasis label tone (a control's own body/knob)
    readonly property color navChipBg: dark ? "#1b1d27" : "#eceef3"      // unselected icon-chip background
    readonly property color railDivider: dark ? "#23252f" : "#e2e5ec"    // nav rail edge line
    readonly property color footerSupport: dark ? "#6f7788" : "#6b7484"  // nav rail "Support" text
    readonly property color footerVersion: dark ? "#4a5060" : "#8b93a7"  // nav rail version text
    readonly property color toggleOffTrack: dark ? "#2a2f3a" : "#d3d7e0" // ToggleSwitch off-state track; also a slider's unfilled rail

    // ---- Control surfaces ----
    // The raised neutral controls and the ink that sits ON a filled one. These
    // began as literals inside ProAudioForm.qml, which made the audio dialogs
    // the last dark island: with Light chosen, the dialog card went white and
    // every strip inside it stayed dark. The dark values are the originals, so
    // the dark look is unchanged; the light ones keep the same relationships
    // (a raised control reads as a light grey on white, not a washed-out hole).
    readonly property color controlBg: dark ? "#3a3f55" : "#e8eaf0"      // raised neutral pill/button body
    readonly property color controlBorder: dark ? "#4a4f66" : "#cfd5e2"  // its border, and an unchecked box's
    // NOT named `onAccent`: any `onXxx` name is read by QML as a signal handler
    // (`Cannot assign a value to a signal`), which failed the build's QML load.
    readonly property color controlInk: "#ffffff"                        // ink on a filled control (a primary button, an accent-filled checkbox, a meter marker) — white in BOTH themes

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
    // Windows' own system UI font - always installed, unlike a webfont ("Inter", this app's previous choice) that has to be
    // installed separately and silently falls back to something else when it isn't. FreeShow's own app chrome (not its output/
    // stage display, which does ship a font) takes the same approach: the OS's native font, not a bundled one.
    readonly property string fontFamily: "Segoe UI"
    readonly property int textXs: 10
    readonly property int textSm: 13
    // ---- The SETTINGS SUBSCRIPT ----
    // The line under a settings row that explains what the choice does (the
    // engine's own description), and a dial's caption under its value. These
    // were textXs (10), which is a FINE-PRINT size: on Settings · General the
    // sentence explaining a switch read as a footnote nobody would read. 12 is
    // the smallest size that still reads as text, and it stays clearly under the
    // row's own label (textSm, 13). It is NOT a replacement for textXs — a
    // badge, a chip label and a unit suffix are still 10px by design.
    readonly property int textSub: 12
    readonly property int textMd: 15
    readonly property int textLg: 17
    readonly property int textXl: 23
    readonly property int textXxl: 25
}
