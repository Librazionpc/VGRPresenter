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

- **Search finds the sentence you typed.** Searching a phrase such as "I preach what Paul preached" used to list any paragraph that had the words somewhere — the sermon that came first won. Paragraphs where the words sit together, in the order you typed them, now come first, and the percentage on each result shows how close the words are (the phrase itself 100, nearly 92, merely present 75) instead of 100 for everything.
- **The same song is no longer added to Shows again and again.** Importing a song you already have used to create "Name (2)", "Name (3)"… and a library could list over a thousand songs twice. An import now skips a song that is already there with the same words, and **Remove duplicate shows** (Settings · General · Libraries) keeps one copy of each and moves the extra copies to the library's deleted-shows folder, so nothing is lost. A second show with the same name is now numbered the way a file system numbers a copy — "Name (2)", "Name (3)"… — in its name as well as its file, so the copies can be told apart in a list (Duplicate, and an import of a different song that shares a name).
- **The Scripture search box fills the whole reference.** Typing the start of a book ("matt", "1joh") now fills "Matthew 1:1" with the colon, ready for you to type the chapter and verse. A start that fits two books ("jud") is still left for you to finish.
- **The app uses much less memory when idle.** The sermon library no longer keeps three extra copies of its text for search (about 330 MB less), and it opens one sermon at a time instead of building a copy of the whole library first. Each Bible verse also no longer carries search data nothing used (about 35 MB less), the Bible's search index is stored far more compactly (about 40 MB less), and searching The Table no longer keeps a second, lowercase copy of every sermon in memory (about 80 MB less after the first search). The sermon text itself is now kept in a file the system pages in and out as needed instead of in private memory (about 145 MB less when idle); the file is temporary and removed when the app closes.
- **Recordings no longer come out corrupted on Windows.** Recorded files could not be played back; they now save as proper, playable files with picture and sound, and a recording is no longer cut short when you stop it while the encoder is still catching up.
- **The verse number / reference on Main Output and the Lower Third now follows the scroll.** As you scroll forward or back through a passage, the reference changes with it on every output.
- **Scrolling Scripture and The Table is smoother.** Long passages glide more evenly, with far fewer small hitches, and the audience output keeps a steadier frame rate.
- **The Lower Third (and every other output size) moves exactly like Main Output.** Wider or smaller outputs no longer re-flow their lines on each press or show short cut-off lines at the top, going forward or back. Each output keeps its own line breaks and moves by whole lines, and the real NDI output matches the on-screen tiles.
- **No more brief wrong line breaks right after a pick.** For about half a second after choosing a passage, the outputs could be handed the wrong line layout.
- **Words fit correctly from the first click.** A single click, a double-click, Enter or ‹ › now all show the preview at the same size as what goes on air; the first double-click after opening the app no longer shows tiny text that corrects itself later.
- **A double-click no longer picks twice.**
- **Faster picks.** Choosing a verse or paragraph is quicker — including the very first one after opening the app and picks from the Bible, which no longer stall the app afterwards. Hidden panels and cards no longer lay out text or work out a font size during a pick, and each output's picture is composed once and shared instead of once per window.
- **The app stays fast on a busy computer and after hours of use.** A long-run load test (a press every 0.7 s, a new pick every 14 s, mode switches and blackouts, with the CPU heavily loaded) found and removed the causes of the slow-downs: a reading passage no longer re-cuts its text on every press in a long sermon, the same slide data is no longer converted back and forth for every screen, and a still NDI picture is sent at a lower rate instead of 60 times a second. In the same test the worst interface freeze fell from about 4 seconds to about 1 second, and stalls and slow presses roughly halved.
- **The reference label follows the scroll without slowing it.** "Genesis 1:5" now changes on every screen without rebuilding each screen's picture; that rebuild cost about 90 ms per paragraph while scrolling.
- **Following the scroll no longer searches the whole chapter on every tick.** The app remembers which paragraph the reading is in and only looks again when it moves to another one.
- **A still picture is composed once per change.** A slide that is not a scrolling passage used to be rebuilt once for each of a screen's handlers for a single event; it is now built once.
- **Saved slide data is converted once, not on every redraw.** The text of a slide is no longer packed and unpacked each time a screen composes it.
- **A load test now ships with the project.** `tools/selftest/run_soak_test.sh` simulates a busy service (a press every 0.7 s, new picks, mode switches, blackouts, a loaded CPU) and reports memory, handles, threads and interface freezes; with `BPS_PERF_PROBE=1` it also names which handlers cost the most. For developers only.
- **No more multi-second freeze while an NDI output is on and you change an output's style or flip Slide ↔ Scroll.** A long soak caught the app rebuilding a style's whole layout for every output several times a second, and re-fitting every page of a long passage just to write a diagnostic line when switching from Scroll to Slide. The first is now remembered until a style changes, the second only runs for developers. Switching modes on a long sermon went from about 2–3 seconds to under 1 second, and searching The Table no longer makes the rest of the app wait for the search.
- **No more crash when closing the app while NDI is sending.** A late request could start the NDI sender again while the app was quitting, which made Windows report a crash.
- **Two outputs on a mirrored projector no longer draw over each other.** When Windows reports one projector as two identical screens, only the first output opens its window and a notice says why.
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
