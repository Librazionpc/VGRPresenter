# Theme, fonts & icons — where they live, and what a refactor needs to touch

## The source of truth: `Theme.qml`

[Theme.qml](Theme.qml) is a `pragma Singleton` — every color, spacing step,
corner radius, and type size in the app is meant to come from here. Change a
value in this one file and every screen/component that references
`Theme.<name>` picks it up automatically.

- **Colors** — brand (`accent`, `accentLight`), semantic (`danger`,
  `success`, `warning`, `info`, each with a `*Light` variant), neutrals
  (`windowBg` → `border`/`borderSubtle`), text (`textPrimary` →
  `textMuted`), and the Settings nav rail's own set
  (`iconMuted` → `toggleOffTrack`).
- **Spacing** — `space1` (4px) through `space6` (24px) plus `space8` (32px).
- **Radius** — `radiusSm`/`radiusMd`/`radiusLg`/`radiusXl`.
- **Type** — `fontFamily` ("Inter") and `textXs` → `textXxl` pixel sizes.

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
theme value, so it isn't a candidate for the config-file treatment.

## The catch: some files can't read `Theme.*` at all

Qt 6.11.1's QML AOT compiler fails to resolve the `Theme` singleton at
deeper nesting levels (confirmed repeatedly this project — not a one-off
fluke). A handful of files sit at that depth and had no choice but to
hardcode literal values instead, each with a comment naming the `Theme.*`
token it has to be kept in sync with by hand, e.g.:

```qml
color: "#232530" // Theme.border
```

**Editing `Theme.qml` does NOT update these** — they're a second copy of
the value, not a reference to it. A real palette refactor has two steps,
not one:

1. Edit `Theme.qml`.
2. Grep for every literal mirror and update each one to match:
   ```
   grep -rn "// Theme\." qml/
   ```
   As of this writing that's 37 spots across 14 files — heaviest in
   `components/DropdownPanel.qml` (20, the whole file), with one or two
   each in `AppMenuBar.qml`, `AppHeader.qml`, `HeaderStatus.qml`,
   `ViewTabs.qml`, `NavRail.qml`, `LabeledSlider.qml`,
   `BackgroundColorModal.qml`, `DraggableCanvasText.qml`,
   `SnapGuideLines.qml`, `SizeStyleCard.qml`, `TextItemPanel.qml`,
   `SlideListItem.qml`, and `EditScreen.qml`.

If a future Qt version fixes the AOT resolution bug, these can all
switch back to plain `Theme.*` references and this step disappears — don't
"fix" it by adding more literals elsewhere in the meantime.

## The other catch: the raw Figma-export screens

`VGRPresenterMainScreen.qml` (~340 raw `"#hex"` colors) and
`EditScreen.qml` (~80) predate `Theme.qml` and were never migrated — they're
literal Figma-to-QML export output, not built against the design system at
all, and **none of their colors are commented as `Theme.*` mirrors** (so
step 2's grep won't find them). A refactor that needs to reach into these
two files is a separate, much bigger pass: mapping each literal to the
closest semantic `Theme.*` token by hand, not a find-and-replace. Flagging
this now so it isn't a surprise mid-refactor — not attempting it as part of
this doc.
