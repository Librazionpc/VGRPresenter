# VGRPresenter — Product update

# VGRPresenter 0.1.3

## What's new

- **Single click puts the next one on air.** Once a verse, paragraph or slide is on air, a single click on another one in the same list puts it on air too — no double-click each time. Ctrl and Shift clicks still just select. Turn it off in Settings · General · Preferences ("Single click puts the next on air").
- **Cursor frequency.** A new +/− setting in the reading options sets how many lines one arrow press moves (1–20), next to the reading speed.
- **Clean reinstall in the installer.** If the app behaves differently on one computer than another after an update, tick "Clean reinstall" on the installer's options page (or run Setup with `/FRESH`): it erases the old app files, settings, libraries and caches — including the ones an older version left behind — and installs from scratch. Recordings are always kept. The uninstaller now also offers to erase your settings and data for a completely clean removal.
- **A quiet, operator-friendly app by default.** The installed app now keeps its log to warnings and errors only, no longer runs its developer diagnostics in the background, and no longer shows technical engine or interface warnings as pop-up notices — those go to the log file for support. Notices written for you (a recovered show, an output that is not sending, a missing device) still appear.
- **Recording works the same on every system.** Video and audio recordings are now written by one shared recorder on Windows, macOS and Linux, so a recording made on one machine is the same kind of file on any other (MP4, MKV, MOV, WebM, M4A, MP3, FLAC and WAV, where the system can encode them). Linux and macOS also gain video recording and output level meters. (The Linux and macOS builds of these parts have not been run on those systems yet.)
- **One shared platform layer for Windows, macOS and Linux.** Files, paths, clipboard, notifications, network, environment, input, dialogs and video sources now run on the same code on every system instead of separate per-system versions, so a fix lands everywhere at once.
- **Shared GPU renderer (Windows, macOS, Linux).** A single GPU drawing path replaces the Windows-only one; it is opt-in while it is finished.
- **Each system picks its own default fonts**, chosen in the app, so text looks right on every system without a font that is missing on it.
- **Slowdowns can be traced when needed.** Developers can switch on a stall trace that records where the interface froze; it is off in the installed app so it costs nothing.

## Fixes

- **Recordings no longer come out corrupted on Windows.** Recorded files could not be played back; they now save as proper, playable files with picture and sound, and a recording is no longer cut short when you stop it while the encoder is still catching up.
- **The verse number / reference on Main Output and the Lower Third now follows the scroll.** As you scroll forward or back through a passage, the reference changes with it on every output.
- **Scrolling Scripture and The Table is smoother.** Long passages glide more evenly, with far fewer small hitches, and the audience output keeps a steadier frame rate.
- **The Lower Third (and every other output size) moves exactly like Main Output.** Wider or smaller outputs no longer re-flow their lines on each press or show short cut-off lines at the top, going forward or back. Each output keeps its own line breaks and moves by whole lines, and the real NDI output matches the on-screen tiles.
- **No more brief wrong line breaks right after a pick.** For about half a second after choosing a passage, the outputs could be handed the wrong line layout.
- **Words fit correctly from the first click.** A single click, a double-click, Enter or ‹ › now all show the preview at the same size as what goes on air; the first double-click after opening the app no longer shows tiny text that corrects itself later.
- **A double-click no longer picks twice.**
- **Faster picks.** Choosing a verse or paragraph is quicker — including the very first one after opening the app and picks from the Bible, which no longer stall the app afterwards. Hidden panels and cards no longer lay out text or work out a font size during a pick, and each output's picture is composed once and shared instead of once per window.
- **Camera and HDMI sources no longer make the app act strangely or lag**, and NDI no longer redoes work when nothing has changed.
- **The Shows list appears right away** (from a saved index) instead of staying empty while the library is scanned.
- **A show's media row now shows a live thumbnail**, and a media file or The Table is no longer reported as "Not connected this time" when it is not a camera.
- **Removed the false "no tap was started" warning** and a binding-loop error on the Audio & Video board.
- **Audio level dots stay faintly visible at low volume** instead of disappearing.
- **The Scripture and Table previews stop fighting over their text size**, which used to make the app re-measure again and again.
- **Outputs are tested and configured on the Routing screen**, the one place the old Settings · Outputs page was merged into.

## Windows downloads

- **VGRPresenter-Setup-0.1.3.exe** (recommended installer).
- **VGRPresenter-win64.zip** (portable version; unzip it and open the app).

Requires Windows 10 or newer.
