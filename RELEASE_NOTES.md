# VGRPresenter 0.1.1

## What's new

- **Added The Table, Bible and Only Believe Songs** as content you can bring on screen.
- **Continuous reading scroll for Scripture and The Table.** Alt+Down / Alt+Up now glides smoothly through a passage line by line, instead of cutting slide by slide — the words move the way a teleprompter does, across every output at once.
- **A note when a new output goes on the wire, and a warning when it cannot.** An output added during a session gets a notice the first time it actually starts sending; a row that stays dark gets a warning that says why.
- **Fill on Settings · Outputs is a real, saved setting** (Contain / Cover / Fill), applied from startup instead of silently falling back to a default.

## Improvements

- **Every output now reports its own live state, not just the one with a sender.** A Settings · Outputs row used to show a running line only for NDI and SDI outputs — an HDMI row said "Inactive" and nothing else. Every row now reports its own state: live and sending, warming up, or ready to go live.
- **A blackout is reported on every output**, not only the NDI feed, and clears again the moment the blackout ends.
- **An NDI output created mid-session starts sending on its own** instead of staying dark until the app is restarted.
- **NDI reports name the output they are actually about**, instead of always naming the same one regardless of which output is behind.
- **More style controls on the Edit screen.** Padding, Corner Radius, Border Width, Letter spacing, Word spacing, Line height and Line spacing can now be dragged on a slider instead of typed by hand, and the text border's Line / Dotted / Dashed style has its own dropdown next to Width.

## Fixes

- **Scrolling passages (Scripture, The Table) no longer change size while reading.** The type used to shrink partway through a longer passage and stay smaller for the rest of it; it now stays at the size the template defines for the whole passage, every time.
- **Picking a new verse or paragraph now reliably updates the screen.** Selecting a different item in the list, or stepping with the arrow buttons, used to sometimes leave the display stuck on whatever was shown before — it now always jumps straight to the new selection.
- **Removed a visual glitch that could appear when selecting content**, seen as the screen briefly flickering or resetting itself instead of flowing smoothly to the next line.
- **Camera and video sources no longer lag the app on CPU-only machines.**
- Settings · Outputs' Fill choice is now saved and remembered across restarts instead of being lost.
- Settings · Outputs no longer leaves every non-NDI row silent about its own feed.
- The blackout no longer goes unmentioned on outputs that are not the NDI feed.
- An NDI output added while live no longer holds its frames until the app is restarted.
- A stalled output's recovery now resets that output's own sender correctly.

## Windows downloads

- **VGRPresenter-Setup-0.1.1.exe** (recommended installer).
- **VGRPresenter-win64.zip** (portable version; unzip it and open the app).

Requires Windows 10 or newer.
