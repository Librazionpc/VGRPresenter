# Installer seed data

`tools/make_installer.sh` builds `dist/VGRPresenter-Setup-<version>.exe` — the
one file users download and run. Everything the app needs is already inside;
the **seed data** folders here are for giving users a pre-loaded start (your
show files, sermon library, Bibles, settings), so nobody starts from an empty
app.

The installer copies these files onto the user's machine, but **only files
that don't already exist** — an existing user's data is never overwritten, and
uninstall never deletes user data.

## Several files are generated, not hand-copied

`tools/make_installer.sh` refreshes these on EVERY build, so they always describe
the build (and the building machine's content) rather than a stale manual copy:

| Generated file | From |
| --- | --- |
| `seed-data/local/overlays.json` | the engine's own shipped catalog (`bps_cli --dump-catalogs`) — font-free, with the current catalog revision |
| `seed-data/local/settings.json`, `seed-data/roaming/enginedata/kernel.json` | this machine's live settings and the engine's configuration collections, with this PC's absolute paths stripped (`tools/make_seed_config.mjs`) |
| `seed-data/shows/*` | this machine's Shows library (`Documents\VGR Presenter\Shows`, minus `.deleted`) |
| `seed-data/roaming/enginedata/bibles.json` | this machine's Bibles (not `bibles.userdata.json` — that's personal marks/notes, never starter content) |
| `seed-data/local/the_table.json` + `the_table_texts/` | this machine's The Table sermon library (not the `.bak*` variants, `cache/`, `backups/` or `recovery/` — this machine's own safety nets, not content) |
| `seed-data/local/templates.json` | this machine's OWN live templates, written LAST so it overrides `bps_cli`'s empty catalog dump — see the note below |

A fresh install used to open Scripture/The Table EMPTY, because nothing ever
copied the library content itself — only the catalogs and settings above. The
rows added for Shows/Bibles/The Table close that gap: whichever machine
builds a release is now the source of the content that ships with it.

**Templates are different from overlays**: the engine ships NO built-in
templates at all by design (`DesignCatalogs.hpp`: "ships nothing: the user's
own layouts and categories only") — `bps_cli --dump-catalogs` always writes an
empty `templates.json`. The ONLY way a fresh install gets any templates is
bundling the operator's own live file, which is what the last row above does
— and by explicit choice this carries whatever font names those templates
reference (the "never hand-copy" warning below is about the BUILT-IN catalogs
specifically; a live custom template genuinely has no shipped equivalent to
fall back to). A font missing on a recipient's machine just falls back to a
default font — it does not break anything. If you ever build a template
around a real custom/downloaded font (not a Windows system font like Segoe
UI), that font's own FILE would need separate, license-aware handling this
script does not attempt.

The same build writes two baselines into `dist/VGRPresenter/assets/`, and they
are what an INSTALLED app actually reads — every install replaces them:

- `settings.shipped.json` — the app takes a value from it only for a setting its
  store has no value of its own, so an update arrives configured while every
  choice a user already made is kept.
- `kernel.shipped.json` — the engine's configured styles, outputs and profiles.
  An install that already has a `kernel.json` merges it ONCE per content
  fingerprint: entries the build ships are refreshed in place or appended, the
  user's own entries are never touched, and nothing is deleted. (A per-machine
  production graph is not merged — a fresh install still gets it from the seed.)

That is why the installer never has to delete `settings.json` or `kernel.json` to
make a fix visible.

Catalogs are never hand-copied: a hand-copied catalog is how the shipped
templates once ended up naming the fonts of the machine they were copied from.

## Where your data lives on THIS machine

| What | On this machine | Goes into |
| --- | --- | --- |
| Settings, Bibles, engine data | `%AppData%\VGR\VGRPresenter\` | `seed-data/roaming/` |
| User data dir (sermon library `the_table.json`, converted texts, catalogs) | `%LocalAppData%\bps\` | `seed-data/local/` |
| Show library (the song/sermon categories a Shows screen lists) | `Documents\VGR Presenter\Shows\` | `seed-data/shows/` — its OWN path, not roaming or local (ShowService's default lives in Documents) |

## How to add seed data

Shows, Bibles, The Table and Templates are now bundled AUTOMATICALLY by
`tools/make_installer.sh` (see the generated-files table above) — you do not
need to copy those by hand, and anything you put in their spots is replaced
on the next build. This section is for anything else you want new users to
start with.

1. Back up the folders above (your responsibility — they stay on this machine).
2. Copy what new users should start with into the matching folder here. Do NOT
   hand-copy `templates.json`, `overlays.json`, `settings.json`, the engine's
   `kernel.json`, `shows/*`, `bibles.json` or `the_table.json`/`the_table_texts/`
   — the build overwrites all of those from this machine's own content on every
   run (see above); anything you put there yourself is replaced.
   - `installer/seed-data/roaming/` → mirrors `%AppData%\VGR\VGRPresenter\`
   - `installer/seed-data/local/` → mirrors `%LocalAppData%\bps\`
   - `installer/seed-data/shows/` → mirrors `Documents\VGR Presenter\Shows\`
3. Keep the same subfolder structure.
4. Rebuild the installer — `tools/make_installer.sh` picks it up automatically.

## Privacy note

`installer/seed-data/` is **git-ignored** — anything you drop here stays on
this machine and is only ever baked into locally-built installers. It never
reaches GitHub.
