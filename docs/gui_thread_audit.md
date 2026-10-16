7. **The exit is slow AFTER the loop returns — measured, not felt.** Letting the
   show-io scenario quit (`SelfTest.quit()` from its own timer tick) returns the
   event loop within a second — `app event loop returned with code 0` in the log —
   and the PROCESS then takes ~150 s to disappear (measured: 153 s from the
   scenario's verdict line to exit; a second run showed the same shape). The 5 s
   heartbeat stops at the quit, so this is not a stalled interface thread: it is
   the teardown after it — `aboutToQuit`'s bounded joins plus Qt's window/DXGI
   shutdown, the same slow exit `run_canvas_cursor_test.sh` documents (its 30 s
   grace and force-kill exist for exactly this), and every harness here force-kills
   for it. It is why `run_show_io_test.sh` asserts that the LOOP RETURNED and then
   kills instead of waiting the process out. Which handler the 150 s belongs to is
   not isolated — the aboutToQuit work is silent while it joins — so timing each
   handler is the follow-up; a user who closes the app and watches it sit in Task
   Manager for two minutes is worth that. Stated here rather than left in a
   harness comment.
## 7. The published reads: every consumer, and whether it re-asks

§3.13/§3.15 moved the scripture and search reads off the interface thread, which
turned each one into a PUBLISHED read: the entry point answers from what a worker
has already published and a miss queues the read and answers empty. That shape has
one failure mode of its own — a consumer that reads the value once and never asks
again keeps the empty answer forever, because the worker's answer arrives with
nothing watching for it. Every consumer was audited for it (2026-10-16); this is
the record, and the same three shapes the other audits use: **hooked** (a signal
or a revision the binding reads), **latent** (a shape that would stick, with no
live caller), **asymmetric** (work done on a worker, waiting for an answer nobody
delivers).

| read (what announces it) | consumer | verdict |
|---|---|---|
| `ScriptureService.bibles`, `.loading` (`changed`) | ScripturePane's adapter → the pane's `sources` / `currentSource` / `syncSources` (all read `engineRevision`) | hooked |
| `ScriptureService.books(id)` (`changed`) | the pane's `openSource` (assignment) and `visibleBooks` | hooked — `onChanged` → `syncSources` re-asks while the outline is empty (the gap §3.8's note already fixed) |
| `ScriptureService.chapter(...)` (`chapterCached`) | the pane's hover peek; the OPEN chapter goes through `chapterAsync` instead | hooked (`corpusCacheLanded` re-asks the peek); the sync branch is latent — it exists for an adapter without `chapterAsync` and neither shipped adapter is one |
| `ScriptureService.resolve(...)` (`referenceCached`) | the pane's filter jump, its completion popup, `visibleBooks` | hooked (`corpusCacheLanded` re-asks; `visibleBooks` reads `corpusRevision`). One sub-site: the popup's row function does not read `corpusRevision`, so a resolution landing under a POPUP that is already open is only picked up by the next keystroke — self-healing, left as is |
| `ScriptureService.search(...)` (nothing) | the pane's sync `runSearch` fallback | latent, **fixed at the source**: `searchCached(text)` is emitted with the answer, forwarded by the Scripture adapter and re-asked by the pane (declared on The Table's adapter, which never emits it — both tabs search async) |
| `ScriptureService.notes`, `.chapterUserData`, `.note`, `.isHighlighted`, `.highlights` (`userDataChanged`) | the pane's marks binding (reads `userRevision`) and its notes drawer | hooked (the handler bumps the revision and re-asks the drawer when it is open) |
| `ScriptureService.preview`, `.slides`, `.templates`, `.reference` | ShowCenter's design preview, the pane's preview/rows | not published reads — engine read-throughs and session state, so they cannot "stick" on an answer; `preview` reads the engine on the pick by design (§3.15) |
| `SearchService.bibles` (`bibleChanged`) | the rank diagnostic in Main.qml | **was a trap**: `ImportService` re-emitted `bibleChanged` from outside without asking for a re-walk, and the getter only re-walks an EMPTY cache — so a Bible installed into a non-empty list never appeared in the published list. **Fixed**: `SearchService::refreshBibles()` (+ the two ImportService sites call it) — the walk publishes the ids and `bibleChanged` lands with them |
| `SearchService.bibleLoading`, `.bibleImporting`, `.bibleProgress` | QuickSearchDialog, ScripturePane's adapter | hooked (property dependencies) |
| `SearchService.resolveWords(...)` (nothing) | **`SearchService::readSearch`, on a worker** — the locked search's typo correction | **was a live trap**: the worker called the PUBLISHED lookup, which on a cold cache queues its read and returns `{}` — a hook no worker has — so the first locked search of a misspelled word silently lost the correction the block exists to apply (and the second identical search found it). **Fixed**: warm cache first, `readResolveWords` otherwise; a `wordsCached(text)` notify completes the sync entry's contract |
| `SearchService.fullVerse(...)` (`verseCached`) | QuickSearchDialog's hover preview | **was a trap in the contract**: the service's own comment said "the dialog re-asks" and the dialog had no `verseCached` handler at all. **Fixed**: the handler bumps `verseRevision`, the entry's `fullText` is a binding on it, and the binding falls back to the text the row already carries so a hover cannot blank while the read is in flight. Measured: a verse row in the dialog already carries its own verse text (`chars=79 seeded=79`), so the published answer was never VISIBLE there — latent, not live (see §4's note on the check that could not run) |
| `SearchService.search(...)` (nothing) | Main.qml's rank diagnostic (now `searchAsync`) | latent, **fixed at the source**: `searchCached(text)` is emitted with the answer; no shipped consumer needs it |
| `SearchService.searchSettings` | the Settings title bar | not an engine read — the service's own list |

One hole was on the PRODUCER side and is fixed with it: `ScriptureService`'s
`dropCorpusCaches()` (an import, a reinstall, a template edit) emptied the caches
while reads started before it were still in flight, and the pane's re-read — the
drop announces itself — could find the cache refilled by that older answer and
queue nothing, making a pre-import outline the last word for the session. Every
corpus read now carries the generation it was read in; a landing from an older
generation is dropped and its read re-queued (`corpusGeneration_` in the header).


## 8. The exit path, measured — and the ~150 s that does not reproduce

The quit path was the last unaudited window in this document, and item 7 above is
its open question: the event loop returns in a second, and the process was
measured taking ~150 s more to disappear, with nothing in the log saying which
step owned the wait. It is now instrumented and measured, and the answer is that
**no handler owns it** — at all:

* every `aboutToQuit` step is timed into `engine.log` (`Exit` /
  `aboutToQuit: <step> took <n> ms`, `src/main.cpp`'s `timeExitStep`), and the
  six steps plus the two post-loop ones total **69–87 ms** across three runs
  (largest: `TheTableService.shutdownIndexing` 56–69 ms, `shutdownNdiWorker`
  7–14 ms; every join is already bounded — `EngineWorker`'s at 2 s);
* the tail is split by two markers, because "the stack teardown" is two different
  things: `main: leaving at +0 ms` is the last line before the stack unwinds, and
  `qAddPostRoutine`'s `QCoreApplication teardown reached at +570 ms` fires after
  the `QQmlApplicationEngine` and every window are gone. So the QML/window
  teardown is **~570 ms** and Qt's own statics are the remainder;
* the process is gone **1 s** after the loop returns, in every run of the new
  harness — with an unsaved show AND an output live, which is the case the ~150 s
  reports were about.

That last number is the finding worth stating plainly: the delay the audit
recorded came from `run_show_io_test.sh`/`run_canvas_cursor_test.sh`, and it does
**not** reproduce under the quit path itself. The candidates that could have held
the process are ruled out on the evidence rather than by argument: the gdb
hang-watch never attached (its log shows `app is no longer running — nothing to
attach to`), and the crash store shows no crash record from any of these runs
(a crash in static teardown would have been held by the Windows error reporter
for minutes, and would also skip the post-routine marker — a distinction the
instrumentation can now make, and one the original measurement could not).

`tools/selftest/run_quit_test.sh` (VGR_QUIT_TEST=1) is the harness: a saved show
made dirty again, `goLive()`, the window closed through the real `onClosing`
guard, the dialog answered with a real click on its Save (the click landed —
`saveButton=true`, probed after the dialog's 130 ms fade, which is the one place
`itemVisible` reads true), the queued save's verdict closing the window, the loop
returning, and then the teardown measured. It reads the step timings from
`engine.log` and the two tail markers from the console log, because after
`EngineBridge.shutdown` the engine's logger is gone and the same calls reach
stdout only — reading one file for both is what made its first fixed run report a
marker that "never landed" while the line sat in the other file. It prints
`QUIT: PASS guard=refused save=ok quit=yes teardown=<s>s handlers=<n>steps` and a
`NOTE` when the teardown exceeds 10 s, so a recurrence is attributable from its
own output rather than from a stopwatch at the end.

## 9. The wrong-thread teardown Qt reported on every quit

`QObject::killTimer: Timers cannot be stopped from another thread` followed by
`QObject::~QObject: Timers cannot be stopped from another thread`, once per quit,
after any live session (in `engine.log` at 01:33:36.779, 01:37:17.047 and
01:40:13.138). Both lines come from the same event: a QObject with a timer being
destroyed in a thread that does not own it.

The object is the NDI feed worker. `LiveOutputService::ensureNdiWorker()` creates
`ndiWorker_` and moves it to the feed thread; `setNdiPlan()` then creates the
send clock as its CHILD, on that thread (`ndiSendTimer_ = new QTimer(ndiWorker_)`
inside a call queued to the worker). So the worker owns a QTimer whose thread is
the feed thread — and `shutdownNdiWorker()` deleted the worker from the interface
thread (`delete ndiWorker_;` after the join), taking the timer down with it. Qt
says exactly that, and the shape is worth naming: the delete looked careful
(joined first, plain context object) and the bug was an ownership fact two calls
away from it.

The fix is the ownership rule itself: the worker is destroyed in the thread that
owns it. `ensureNdiWorker()` now hangs the destruction off `finished()` — which is
emitted IN the feed thread as its loop ends — and `shutdownNdiWorker()` deletes
nothing at all, it only drops the raw pointers with the objects they belonged to.
`armNdiSenderForSession()`'s duplicate copy of the thread/worker construction is
gone, so the ownership rule exists once, at the only place a worker is made. The
same rule was applied to `EngineWorker`'s plain context object (no timer today,
which is why it was silent — and why the rule belongs there before one is added).

**The first attempt failed, and the measurement is the interesting part.** It
queued the timer's teardown to the feed thread before telling that loop to quit —
which reads like the careful thing. It never ran: `quit()` ends the loop and a
posted call is not reliably drained on the way out (the run that proved it quit
2 s after going live, and no teardown line appeared at all). So the destruction
rides `finished()` instead, and it carries an evidence line
(`NDI worker teardown on the feed thread`) because a fix whose only proof is an
ABSENT warning cannot be checked later — the queued version looked fixed by that
measure and had simply never executed.

`tools/selftest/run_quit_test.sh` now asserts both halves on every run: the
warning pair must not appear in either log (console or `engine.log`, they carry
different halves of the exit path), and the teardown line must be there.
Verified: the pair appears in `engine.log` for the three earlier runs and for
none of the runs after the fix.
