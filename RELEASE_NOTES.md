# VGRPresenter 0.1.0

## Improvements

- **Every output now reports its own live state, not just the one with a sender.** A Settings · Outputs row used to show a running line only for NDI and SDI outputs — an HDMI row said "Inactive" and nothing else, so the single row that had something to say looked like the only output the app was watching. Every row now reports: "HDMI · live · 1204 frames delivered", "warming up · waiting for the first frame", or "ready (go live to send)", with its dot coloured by that state.
- **A blackout is reported on every output.** While the stage is blacked out, every enabled row says "· blackout up" — not only the NDI feed — and every row drops it again the moment the blackout ends.
- **An NDI output created mid-session starts sending on its own.** It no longer stays dark until the app is restarted: the feed re-reads the roster every tenth of a second, so a row added, enabled, renamed or switched off while live reaches the wire within a poll.
- **NDI reports name the output they are about.** Rate drops, recoveries, sustained drift and stalls are measured per output, so the message points at the output that is actually behind instead of always naming "New Screen 2".
- Added The Table, Bible and Only Believe Songs

## What's new

- **A note when a new output goes on the wire, and a warning when it cannot.** An output added during a session gets one "NDI output 'X' is live — 'VGRPresenter - X' is on the network now" note the first time its sender carries a frame. A row that stays dark gets one warning that names the actual wait: its frame buffer has not been created yet, its buffer is held off, no frame has been rendered for it yet, or its sender has not opened.
- **The engine measures every output, not just the senders.** Frames delivered and the time since the last present are read for each output's own buffer, which is what lets an HDMI row say the same things as an NDI row instead of guessing.
- **Fill on Settings · Outputs is a real setting.** "Default media fit" is a declared choice (Contain / Cover / Fill) with a saved default, applied at startup before any take can fall back to the built-in one.
- **A settings key that does not exist can no longer ship.** The build now checks every key the interface reads or writes against the engine's own settings list, so a missing one fails the build instead of appearing as a red toast the first time the control is touched.

## Fixes

- Settings · Outputs' Fill no longer shows "there is no setting 'outputs.defaultMediaFit'" — the pick is saved and remembered across restarts instead of being refused and lost.
- Settings · Outputs no longer leaves every non-NDI row silent about its own feed while the NDI row reported frames.
- The blackout no longer goes unmentioned on the outputs that are not the NDI feed.
- An NDI output added while live no longer holds its frames until the app is restarted, and a row that cannot send says why instead of staying quietly dark.
- A stalled output's recovery resets that output's own sender, instead of an object the feed never sends through.

## Windows downloads

- **VGRPresenter-Setup-0.1.0.exe** (recommended installer).
- **VGRPresenter-win64.zip** (portable version; unzip it and open the app).

Requires Windows 10 or newer.
