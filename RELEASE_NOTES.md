# VGRPresenter — Product update

## New in the upcoming update

- **Single click puts the next one on air.** Once a verse, paragraph or slide is on air, a single click on another one in the same list puts it on air too — no double-click each time. Ctrl and Shift clicks still just select. Turn it off in Settings · General · Preferences ("Single click puts the next on air").
- **Cursor frequency.** A new +/− setting in the reading options sets how many lines one arrow press moves (1–20), next to the reading speed.
- **One shared platform layer for Windows, macOS and Linux.** Files, paths, clipboard, notifications, network, environment, input, dialogs, video sources and recording now run on the same Qt code on every system instead of separate per-system versions, so a fix lands everywhere at once. Linux and macOS also gain video recording and output level meters. (The Linux and macOS builds of these parts have not been run on those systems yet.)
- **Shared GPU renderer (Windows, macOS, Linux).** A single GPU drawing path replaces the Windows-only one; it is opt-in while it is finished.
- **Each system picks its own default fonts**, chosen in the app, so text looks right on every system without a font that is missing on it.
- **Stalls are always traced.** If the interface freezes for more than about a tenth of a second, the log now records where, on every system, so slowdowns can be found without a special build.

# VGRPresenter 0.1.2

## What's new

- **Added The Table, Bible and Shows** as content you can bring on screen.
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
- UI Fixes

## Fixed in the upcoming update

- **Scrolling Scripture and The Table is smoother.** Long passages glide more evenly, with far fewer small hitches.
- **The Lower Third (and every other output size) moves exactly like Main Output.** Wider or smaller outputs no longer re-flow their lines on each press or show short cut-off lines at the top, going forward or back. Each output keeps its own line breaks and moves by whole lines.
- **No more brief wrong line breaks right after a pick.** The outputs used to be handed the wrong line layout for about half a second after choosing a passage.
- **Words fit correctly from the first click.** A single click used to show the preview text tiny until a double-click; the preview now uses the same size as what goes on air, whether you click, press Enter, or use ‹ ›.
- **A double-click no longer picks twice.**
- **Faster picks.** Choosing a verse or paragraph is noticeably quicker: hidden panels and cards no longer do text layout work during a pick.
- **Camera and HDMI sources no longer make the app act strangely or lag**, and NDI no longer redoes work when nothing changed.
- **The Shows list appears right away** (from a saved index) instead of staying empty while the library is scanned.
- **A show's media row now shows a live thumbnail**, and a media file or The Table is no longer reported as "Not connected this time" when it is not a camera.
- **Removed the false "no tap was started" warning** and a binding-loop error on the Audio & Video board.
- **Audio level dots stay faintly visible at low volume** instead of disappearing.
- **The reference-pane previews stop fighting over their text size**, which used to re-measure again and again.
- **Settings → General** gets a switch for single-click-on-air (above).

## Windows downloads

- **VGRPresenter-Setup-0.1.2.exe** (recommended installer).
- **VGRPresenter-win64.zip** (portable version; unzip it and open the app).

Requires Windows 10 or newer.
