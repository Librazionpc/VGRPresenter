# Installer seed data

`tools/make_installer.sh` builds `dist/VGRPresenter-Setup-<version>.exe` — the
one file users download and run. Everything the app needs is already inside;
the **seed data** folders here are for giving users a pre-loaded start (your
show files, sermon library, Bibles, settings), so nobody starts from an empty
app.

The installer copies these files onto the user's machine, but **only files
that don't already exist** — an existing user's data is never overwritten, and
uninstall never deletes user data.

## Two files are generated, not copied

`tools/make_installer.sh` refreshes these on every build, so they always describe
the build rather than somebody's appdata:

| Generated file | From |
| --- | --- |
| `seed-data/local/templates.json`, `seed-data/local/overlays.json` | the engine's own shipped catalogs (`bps_cli --dump-catalogs`) — font-free, with the current catalog revision |
| `seed-data/local/settings.json`, `seed-data/roaming/enginedata/kernel.json` | this machine's live settings and the engine's configuration collections, with this PC's absolute paths stripped (`tools/make_seed_config.mjs`) |

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
| Settings, engine data, show library | `%AppData%\VGR\VGRPresenter\` | `seed-data/roaming/` |
| User data dir (sermon library `the_table.json`, converted texts, Bibles…) | `%LocalAppData%\bps\` | `seed-data/local/` |

## How to add seed data

1. Back up the folders above (your responsibility — they stay on this machine).
2. Copy what new users should start with into the matching folder here. Do NOT
   hand-copy `templates.json`, `overlays.json`, `settings.json` or the engine's
   `kernel.json` — the build regenerates those (see above); anything you put
there is replaced.
   - `installer/seed-data/roaming/` → mirrors `%AppData%\VGR\VGRPresenter\`
   - `installer/seed-data/local/` → mirrors `%LocalAppData%\bps\`
3. Keep the same subfolder structure (e.g. `seed-data/local/the_table.json`,
   `seed-data/local/bibles/<file>.json`).
4. Rebuild the installer — `tools/make_installer.sh` picks it up automatically.

## Privacy note

`installer/seed-data/` is **git-ignored** — anything you drop here stays on
this machine and is only ever baked into locally-built installers. It never
reaches GitHub.
