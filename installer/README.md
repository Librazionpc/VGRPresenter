# Installer seed data

`tools/make_installer.sh` builds `dist/VGRPresenter-Setup-<version>.exe` — the
one file users download and run. Everything the app needs is already inside;
the **seed data** folders here are for giving users a pre-loaded start (your
show files, sermon library, Bibles, settings), so nobody starts from an empty
app.

The installer copies these files onto the user's machine, but **only files
that don't already exist** — an existing user's data is never overwritten, and
uninstall never deletes user data.

## Where your data lives on THIS machine

| What | On this machine | Goes into |
| --- | --- | --- |
| Settings, engine data, show library | `%AppData%\VGR\VGRPresenter\` | `seed-data/roaming/` |
| User data dir (sermon library `the_table.json`, converted texts, Bibles…) | `%LocalAppData%\bps\` | `seed-data/local/` |

## How to add seed data

1. Back up the folders above (your responsibility — they stay on this machine).
2. Copy what new users should start with into the matching folder here:
   - `installer/seed-data/roaming/` → mirrors `%AppData%\VGR\VGRPresenter\`
   - `installer/seed-data/local/` → mirrors `%LocalAppData%\bps\`
3. Keep the same subfolder structure (e.g. `seed-data/local/the_table.json`,
   `seed-data/local/bibles/<file>.json`).
4. Rebuild the installer — `tools/make_installer.sh` picks it up automatically.

## Privacy note

`installer/seed-data/` is **git-ignored** — anything you drop here stays on
this machine and is only ever baked into locally-built installers. It never
reaches GitHub.
