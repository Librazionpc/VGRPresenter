# Theme, fonts & icons — where they live, and what a refactor needs to touch

## The source of truth: `Theme.qml`

[Theme.qml](Theme.qml) is a `pragma Singleton` — every color, spacing step,
corner radius, and type size in the app is meant to come from here. Change a
value in this one file and every screen/component that references
`Theme.<name>` picks it up automatically.

- **Colors** — brand (`accent`, `accentLight`), semantic (`danger`,
  `dangerLight`, `success`, `warning`, `info`, each with a `*Light`
  variant, plus the fixed-red `liveBg`/`liveBgOnAir` GO LIVE pill),
  neutrals (`windowBg` → `border`/`borderSubtle`), app chrome
  (`hoverBg`, `activeBg`, `iconChrome`), text (`textPrimary` →
  `textMuted`), and the Settings nav rail's own set
  (`iconMuted` → `toggleOffTrack`). Every neutral is a live
  `dark ? <dark> : <light>` binding, so the whole token set follows the
  Appearance > Theme choice.
- **Spacing** — `space1` (4px) through `space6` (24px) plus `space8` (32px).
- **Radius** — `radiusSm`/`radiusMd`/`radiusLg`/`radiusXl`.
- **Type** — `fontFamily` ("Segoe UI" — Windows' own system UI font, always present, the same reasoning FreeShow's own app UI uses its OS's native font instead of shipping one) and `textXs` → `textXxl` pixel sizes.

Every screen and shared component built this session (`OutputsScreen`,
`StylesScreen`, `AudioVideoScreen`, `ModalCard`, `Pill`, `SelectableChip`,
`SettingsToggle`, `LabeledSlider`, `LevelDial`, all of it) reads straight
from `Theme.*`. Editing this file alone is enough for those.

## Icons: `IconGlyph.qml`

Icon *shapes* live in [components/IconGlyph.qml](components/IconGlyph.qml)
as Lucide SVG path data, one `Component` per icon name, picked by a
`switch` on `IconGlyph.name`. There's no separate icon color token — every
icon is monochrome and takes its color from whoever uses it
(`IconGlyph { name: "close"; color: Theme.textMuted }`), so icon *coloring*
already refactors for free along with the rest of the palette. Only the
icon *shapes themselves* (adding a new one, swapping which SVG a name maps
to) require editing `IconGlyph.qml` directly — that's asset content, not a
theme value, so it isn't a candidate for the config-file treatment.## The chrome reads tokens now (the old "AOT depth" story was wrong)

Earlier docs (and comments in the chrome files themselves) claimed Qt
6.11.1's QML AOT compiler "cannot resolve the `Theme` singleton at deeper
nesting levels," so the app chrome — title bar, tab strip, menu bar,
dropdowns — hardcoded its own dark hex with `// Theme.<token>` mirror
comments. **That diagnosis was wrong.** The real cause was that
`qt_add_qml_module` does not infer `pragma Singleton` from the file, so the
generated `qmldir` registered `Theme` as an *ordinary type*; `Theme.border`
then read a static property off a type object and came back `undefined`.
Depth only *looked* correlated because the shallow screens happened to be
the ones that imported the module explicitly.

That is fixed at its root in `CMakeLists.txt`:

```cmake
set_source_files_properties("…/qml/settings/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE true)
```

The generated `qmldir` now lists `singleton Theme 1.0 …`, and the chrome
files ([AppHeader.qml](../screens/main/AppHeader.qml),
[ViewTabs.qml](../screens/main/ViewTabs.qml),
[AppMenuBar.qml](../screens/main/AppMenuBar.qml),
[HeaderStatus.qml](../screens/main/HeaderStatus.qml),
[WindowControls.qml](../screens/main/WindowControls.qml),
[DropdownPanel.qml](../components/DropdownPanel.qml)) read `Theme.*`
directly. A Light choice recolours them with the rest of the app.

The Edit screen and its components are migrated: `EditScreen.qml`,
`BackgroundColorModal.qml`, `TextItemPanel.qml`, `MediaLibraryPane.qml`,
`MonitorWall.qml`, and `SizeStyleCard.qml` read `Theme.*` for every piece of
chrome. The migration rule that mattered there was chrome-vs-content:
backgrounds, borders, hover washes, text tones, accents and semantic status
hues became tokens, while genuinely per-item CONTENT — the colour/gradient
palette swatches, an item's own chosen background/border/text colour, the
camera/clock artwork, the canvas stage ground — kept its literal values
(those are data the user picks, not app chrome, and must not be recoloured
by the app theme).

Outside those, smaller counts of raw hex remain in a handful of components
(e.g. `LabeledSlider.qml`'s `#aeb6c8` change-chip label, the per-kind source
modals) — the same chrome-vs-content rule applies, and they need no nesting
workaround. (`NavRail.qml` is already fully tokenised.)

## The other catch: the raw Figma-export screen

`VGRPresenterMainScreen.qml` (~340 raw `"#hex"` colors) predates
`Theme.qml` and was never migrated — it's literal Figma-to-QML export
output, not built against the design system at all, and **none of its
colors are commented as `Theme.*` mirrors** (so step 2's grep won't find
them). A refactor that needs to reach into it is a separate, much bigger
pass: mapping each literal to the closest semantic `Theme.*` token by hand,
not a find-and-replace. Flagging this now so it isn't a surprise
mid-refactor — not attempting it as part of this doc.
