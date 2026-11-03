#!/usr/bin/env node
// check_gui_thread_blocks.mjs — GUI-thread blocking-call guard.
//
// The question this answers is not "is there a lock held across a PAL call"
// (that is tools/check_lock_scope.mjs) but "can a *QML binding or signal
// handler* reach a call that parks the interface thread". The interface thread
// is the one thread the user sees: every millisecond it spends inside a PAL
// read, a filesystem walk, a device enumeration or a contended lock is a
// millisecond of frozen window, dropped click and un-redrawn frame — and unlike
// a worker thread, it has no backlog to make up the time later.
//
// The app's own hang dumps prove the class (AppData/Roaming/VGR/VGRPresenter/
// crashes/hang-*-gdb.txt, written by CrashHandler): the interface thread has
// been captured parked in, among others,
//
//   TelemetryService::poll -> WindowsPlatform::Sample -> GetIfTable2 (kernel)
//   EngineBridge::pumpMeterSnapshot -> WindowsAudio::InputLevels (mutex)
//   SettingsService::setValue -> AppSettings::Set -> ApplyToEngine ->
//       AdaptiveRuntime::ApplyOptimization (mutex)
//   EngineBridge::write -> Logger::Log
//
// Every one of those is a Q_INVOKABLE or a bound property reachable from QML.
// That is the shape this guard flags: a QML-exposed function whose body can
// reach a blocking primitive, off-thread hop or not.
//
// HOW IT DECIDES
//
//  1. Entry points come from the QML-facing layer's headers: `Q_INVOKABLE`,
//     `Q_PROPERTY(... READ x ...)` getters, `QML_ELEMENT` classes' public
//     slots. A binding or handler can call these synchronously, so each one is
//     a place the interface thread can be parked.
//  2. Bodies are extracted by brace matching (a heuristic parser, not a
//     compiler), for definitions in the .cpp files AND inline in the headers.
//  3. A body is "blocking" if it contains a blocking primitive, or calls
//     another function that does — the closure is transitive within the file,
//     because the interesting chain is always a private helper (sampleHealth,
//     pumpMeterSnapshot, ApplyToEngine) that the entry point looks innocent
//     next to.
//  4. An entry point is EXEMPT when it hops off the interface thread before the
//     blocking work: `Qt::QueuedConnection` / `QThread::create` / a worker
//     `->start()` earlier in the body. That is the fix pattern the codebase
//     already uses (TelemetryService's sampler, the search/indexer workers), so
//     the guard rewards it instead of flagging it.
//
// Waivers: `gui-ok: <reason>` on the entry point's definition line, reason
// mandatory. Same staleness rules as the lock guard — a waiver whose line no
// longer trips a rule, or whose entry point is no longer reachable from QML, is
// itself a finding.
//
// THE BASELINE (`tools/gui_thread_baseline.json`) is the third state, for HARD
// sites the project KNOWS about and has not fixed yet: they report as `N known
// (baselined)` and do not fail the build, while any NEW hard finding still does.
// It exists because a truthful repair of this sweep must not be paid for with a
// permanently red build — teaching it that a nested helper type cannot take the
// enclosing class's name over made 18 `EngineBridge` sites visible at once, and
// the ones nobody had time to fix are recorded there with their plan (see the
// file's own `note`, and docs/gui_thread_audit.md §6.5) rather than deleted.
// `--update-baseline` rewrites the list from the current findings — which
// includes the SOFT inventory, so prefer editing `keys` by hand and keep the
// soft list in the audit report where it belongs.
//
// Usage:
//   node tools/check_gui_thread_blocks.mjs            # default scope: qml/services, qml/models
//   node tools/check_gui_thread_blocks.mjs --all      # plus the soft inventory (locks/singletons)
//   node tools/check_gui_thread_blocks.mjs --list     # compact file:line list, still exits 1
//   node tools/check_gui_thread_blocks.mjs --verbose  # print the blocking chain per finding
//   node tools/check_gui_thread_blocks.mjs --qml      # also list every sync call site
//
// The same run also sweeps qml/ for a SYNCHRONOUS call where the service already
// offers an async sibling (`search` vs `searchAsync`, `chapter` vs
// `chapterAsync`): the worker and the token guard exist, and only the call site
// did not use them. Waive one with `// sync-ok: <reason>`.
//   node tools/check_gui_thread_blocks.mjs --update-baseline
//
// Exit: 0 = clean (or only baselined sites), 1 = new findings or stale waivers.

import { spawnSync } from "node:child_process";
import { readFileSync, readdirSync, statSync, writeFileSync, existsSync } from "node:fs";
import { join, relative, sep, basename, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = process.cwd();
// The script's own directory, so the fixtures are found (and the self-test's
// child process is spawned) no matter which directory the guard is invoked
// from — CMake runs it from the build tree, a developer from the repo root.
const SELF = fileURLToPath(import.meta.url);
const SCRIPT_DIR = dirname(SELF);

// ---- What counts as blocking ------------------------------------------------
// Two severities, because they are two different claims and only one of them
// can gate a build without crying wolf.
//
// HARD = the interface thread parks for an *unbounded* time in the operating
// system, a device or another process. Every hard pattern here has frozen this
// app's interface thread in a recorded hang dump (see the header), or is the
// same call one step away. These fail the check.
//
// SOFT = a call that *can* contend: a cross-engine singleton (which takes its
// own locks) or a lock acquire on the interface thread. Those froze the app too
// — but only while some other thread held the lock, which is not visible in the
// call site, and most such sites are uncontended in practice. They are printed
// as an inventory (the audit list) and do not fail the build on their own.
const HARD = [
  // ---- The platform layer: unbounded OS waits ------------------------------
  [/\bbps::platform::/, "PAL call"],
  [/\bPlatformAccessor\b/, "PAL accessor"],
  [/\b(WindowsPlatform|WindowsAudio|WindowsVideo|WindowsDisplay)\b/, "Windows PAL"],
  [/\b(WaitForSingleObject|WaitForMultipleObjects|MsgWaitForMultipleObjects)\s*\(/, "Win32 wait"],
  [/\b(SleepEx|Sleep|msleep|usleep|std::this_thread::sleep_for)\s*\(/, "sleep"],
  [/\b(ReadFile|WriteFile|DeviceIoControl|GetIfTable2?|GetSystemTimes|Thread32Next|NsiAllocateAndGetTable)\s*\(/, "OS enumeration"],
  [/\b(CoInitializeEx|CoCreateInstance|IAudioClient|IMMDeviceEnumerator|IMFActivate)\b/, "COM/device init"],
  [/\bNDIlib_\w+/, "NDI SDK"],
  [/\b(DwmFlush|StretchBlt|BitBlt)\s*\(/, "GDI/DWM present"],
  [/\bQProcess\b|\bwaitForFinished\s*\(|\bwaitForStarted\s*\(/, "process wait"],
  [/Qt::BlockingQueuedConnection/, "blocking queued call"],
];

const SOFT = [
  // ---- The engine: singletons that take locks and touch devices ------------
  [/\b(BroadcastEngine|DisplayEngine|MediaEngine|RenderEngine|ProductionEngine|RecordingEngine|FlowEngine|SceneCompositionEngine|DatabaseManager|BibleEngine|SongEngine|SearchEngine|ProjectLibrary|DesignLibrary|WorkspaceManager|TheTableLibrary|MediaLibrary)::Instance\s*\(/, "cross-engine singleton"],
  [/\b(AdaptiveRuntime|ResourceManager|Telemetry)::Instance\s*\(/, "cross-engine singleton"],
  // The bridge, judged on what each of its calls actually costs — the recorded
  // hang (`EngineBridge::write -> Logger::Log`) predates the logger's async
  // pump, and the two shapes the QML-facing layer uses are both bounded:
  //
  //   * `EngineBridge::write` / `log` forwards to `bps::Logger::Instance().Log()`,
  //     which only converts the record and ENQUEUES it — a `deque` push under a
  //     short mutex, dropping when full (core/logging/Logger.cpp:195) — while
  //     every sink write (file, console, JSON, IPC) happens on the logger's own
  //     thread in `Pump()`. Nothing behind it is a device, a disk or another
  //     process, so its cost is the app's own, which is what the audit allows.
  //   * `EngineBridge::instance().booted()` / `isBooted()` is
  //     `Kernel::Instance().State() == KernelState::Running`, and `Kernel::State()`
  //     is `state_.load()` (core/kernel/Kernel.hpp:84) — an atomic read.
  //   * The four INPUT READERS — `inputSourceReachable`, `inputSourceAlive`,
  //     `previewFailure`, `inputDelivery` — are the state a card's light and
  //     slash are drawn from, evaluated from QML bindings at the pump's rate.
  //     Each used to reach the PAL or the disk on the caller's thread; none does
  //     now, and the exemptions are the PROOF of the work, not a substitute for
  //     it:
  //       - `previewFailure` asked the video PAL why a tap died, and
  //         `WindowsVideo::PreviewFailure` took the tap table's mutex — the same
  //         one a capture worker holds while it opens, closes or publishes a
  //         frame. It now `try_lock`s that table and, when a worker holds it,
  //         repeats the last answer read for that tap, keyed by a table
  //         GENERATION bumped on every tap add/erase (the audio meter's reader
  //         contract, §3.1 of docs/gui_thread_audit.md). No wait, and no stale
  //         answer: an answer from another generation is dropped.
  //       - `inputSourceReachable` asked the filesystem whether a "media" label
  //         (a file path) still exists — on a share that has gone away, the whole
  //         SMB timeout, per card, per binding evaluation. That question is now
  //         asked once on the engine worker and PUBLISHED
  //         (`EngineBridge::mediaPathReachable`), re-asked on a TTL so a file
  //         deleted mid-session still flips.
  //       - everything else in the four is in-memory: roster scans over the
  //         cached device lists, the tap-ownership/delivery tables the GUI thread
  //         itself owns, and `lightAsks_` bookkeeping feeding a device sweep that
  //         was ALREADY queued to a worker (`inputSourceAlive` never enumerates
  //         inline).
  //
  //   * The three TAP CLOSES — `stopVideoPreview`, `stopScreenPreview`,
  //     `stopNdiPreview` — keep their bookkeeping on the caller's thread (the
  //     owner/id tables are the bridge's own GUI-thread state, and the entry
  //     points read them to decide what to do next) and push only the device
  //     close to the engine worker: a camera tap's stop joins its drain thread,
  //     a screen tap's releases DIBs a worker may be painting into, and an NDI
  //     receiver's disconnect goes out through the BroadcastEngine to the vendor
  //     SDK. Nothing on the caller's thread waits for any of it, and the posts
  //     are FIFO, so a close can never overtake the open of the same tap.
  //
  // The rest of the bridge's DEVICE work hops now as well (audit §6.5, which
  // cleared the last twelve baselined entry points): the meter tap open/close and
  // the id claims (`resolveAndStartMeterOffThread`, `stopInputMeterOffThread`,
  // `stopAllInputMetersOffThread`, `claimStreamTap` -> `openStreamTapOffThread` /
  // `closeStreamTapOffThread`), the device sweep's SDI provider probe
  // (`probeSdiAsync`, `probeNdiAsync`'s shape, on the enumeration pool) and the
  // NDI firewall pair (`probeNdiFirewallOffThread`, for the two entry points that
  // both spawn a hidden `netsh` child — the request's verdict comes back as the
  // signal `ndiFirewallAccessResult`, the retry timer's as
  // `ndiFirewallRuleChecked`, because a child process cannot answer a return
  // value without parking the caller).
  //
  //   * THE THREE TAP OPENS — `startVideoPreview`, `startScreenPreview`,
  //     `startNdiPreview` — carry the ACCEPTED-then-corrected contract §3.11
  //     built: the body resolves the roster label against the CACHED list, reads
  //     and writes this thread's own owner/id/mode tables, and QUEUES the device
  //     call (`openTapOffThread` / the closed tap's own `queueTapOpen`); a screen
  //     label that cannot resolve queues an enumeration rather than walking
  //     hardware. The one PAL touch left is `PlatformAccessor::Installed()`, a
  //     static flag read — the same bound `inputSourceReachable` is waived for.
  //   * `resolvedPreviewDeviceId` is `previewIds_.value(label)` (an inline map
  //     read on this thread's own table) and `refreshNdiConnections` is a
  //     check-and-set plus a `QThreadPool` post — the SDK query that used to sit
  //     inline happens on the pool, and this thread sees a queued answer.
  //   * `ndiAvailable`, `ndiState`, `ndiStatus` are the NDI probe's PUBLISHED
  //     verdict (plain member reads, filled by `probeNdiAsync`'s marshalled
  //     answer) — the same "no answer yet" state `unknown` distinguishes, which
  //     is why the live path can call them from a 10 Hz poll.
  //
  // What the label below still catches is the SINGLETON / logging read-through —
  // `EngineBridge::instance().<member>` outside the list of published-state
  // readers, plus engine fetches that are in-memory — which is the inventory, not
  // a wait.
  [/\bEngineBridge::instance\s*\(\s*\)\s*\.\s*(?!booted|isBooted|inputSourceReachable|inputSourceAlive|previewFailure|inputDelivery|stopVideoPreview|stopScreenPreview|stopNdiPreview|startVideoPreview|startScreenPreview|startNdiPreview|resolvedPreviewDeviceId|refreshNdiConnections|ndiAvailable|ndiState|ndiStatus)\w+/, "engine bridge / logger"],
  // ---- Locks on the interface thread ---------------------------------------
  [/\bstd::(lock_guard|unique_lock|scoped_lock|shared_lock)\s*</, "lock acquire"],
  [/\.lock\s*\(\s*\)|->lock\s*\(\s*\)/, "lock acquire"],
  [/\bstd::condition_variable\b|\bQWaitCondition\b/, "thread wait"],
  // ---- Filesystem & data loading -------------------------------------------
  [/\bstd::filesystem::/, "filesystem"],
  // A file READ/WRITE, or a question ASKED OF the filesystem — not the class
  // NAME. `QDir::fromNativeSeparators("C:/x")` and
  // `QFileInfo(path).fileName()` are string arithmetic that touches nothing, and
  // matching the bare class named three ShowService entry points as "QFile I/O"
  // on those two spellings alone — while the file work they really do
  // (`doc->Save(vgrPath(path))`, `peek.Open(vgrPath(path))`) sat one level below
  // the scan, which is why the audit had to state it in prose (§6.4).
  [/\bQFile\b|\bQSaveFile\b|\bQTextStream\b|\bQSettings\b|\bQDir\s*\(|\bQDir\b[^;]*\.(exists|entryList|entryInfoList|mkpath|mkdir|rmdir|removeRecursively|drives)\s*\(|\bQFileInfo\s*\([^;]*\.(size|exists|lastModified|created|isFile|isDir|isReadable|isWritable|permissions|ownerId|birthTime)\s*\(|\.(entryList|entryInfoList|mkpath|mkdir|rmdir|removeRecursively)\s*\(|\b(QFile|QFileInfo|QDir|QSaveFile)::(exists|remove|copy|rename|link|size|lastModified|isFile|isDir|isReadable|isWritable|drives|mkpath|mkdir|rmdir)\s*\(/, "QFile I/O"],
  // …and the same work when it goes through the ENGINE's document API, which is
  // where ShowService's synchronous save/load actually happens: a path is
  // resolved (`vgrPath`) and handed to `Save`/`SaveCopy`/`Open`. Named so the
  // deliberate synchronous case is flagged at the line that does the work
  // instead of being inferred from path arithmetic somewhere above it.
  [/\bvgrPath\s*\(|\b\w*Document\s*(?:->|\.)\s*(?:Save|SaveCopy|Open|Load)\s*\(/, "document I/O"],
  [/\bQStandardPaths::/, "path lookup"],
  // A LOAD takes an argument that names bytes. The bare `QImage(` this rule
  // used to carry also matched `QImage()` — an empty construction, which
  // decodes nothing and allocates nothing — and that one spelling kept
  // `LiveOutputService::clearMedia` in the inventory on its own: the line that
  // reset the held frame (`lastMediaFrame_ = QImage();`) was the only "data
  // load" in the function. The rule now requires a non-empty first argument.
  [/\bQImage\s*\(\s*[^)\s]|\bQPixmap\s*\(\s*[^)\s]|loadFromData\s*\(|\.readAll\s*\(|\.readLine\s*\(/, "data load"],
];

const BLOCKING = [...HARD.map(([re, l]) => [re, l, "hard"]), ...SOFT.map(([re, l]) => [re, l, "soft"])];

// The hop that makes an entry point safe: work is handed to another thread
// before anything blocking runs.
//
// A named worker-runner counts as a hop (`runOffThread`, `submitOffThread`,
// `queueWorker`): the blocking call sitting inside its lambda is not running on
// this thread, and the guard must see that rather than flag the entry point that
// hands the work over. This is exactly how the codebase's fixes are written —
// see SettingsService::runOffThread — so recognising the shape keeps the guard
// honest about the fixed sites instead of demanding a waiver for them.
// The `->start()` in this list is the ONLY alternative that needs a receiver:
// it means "a worker thread was started here", and it used to be written as a
// bare `->start\s*\(\s*\)` — which also matches `pump_->start()`, a QTimer, or
// any engine's `Start()`-shaped method. That made the exemption FALSE for a
// whole class of entry points: `RecordingService::startRecording` was counted as
// "already off-thread" because it starts its 1 Hz status pump, while the same
// body calls RecordingEngine on the interface thread (measured while this file
// was being made to tell the truth about that service). The receiver now has to
// name a thread or a worker, so the hop it claims is really a hop.
const OFF_THREAD_HOP =
  /Qt::QueuedConnection|QThread::create\s*\(|QThreadPool|QtConcurrent|\b\w*(?:[Tt]hread|[Ww]orker)\w*\s*(?:->|\.)\s*start\s*\(\s*\)|\bstd::thread\b|invokeMethod\s*\([^)]*QueuedConnection|postEvent\s*\(|\b\w*(?:OffThread|AsyncWork|Worker)\w*\s*\(/;

// ---- Self-test (the fixture verdicts these rules must produce) ---------------
// Run before every scan, the lock guard's shape (see AGENTS.md): the files in
// tools/gui_thread_fixtures are small shapes whose verdict is KNOWN — one per
// rule this guard applies, including the ones that must stay QUIET. A rule that
// stops firing fails here instead of quietly approving everything, and a rule that
// starts firing on a helper that reads the engine on a WORKER fails here too.
// Machine-readable output makes the comparison exact rather than a grep.
function runSelfTest() {
  const dir = join(SCRIPT_DIR, "gui_thread_fixtures");
  const expectFile = join(dir, "expect.json");
  if (!existsSync(expectFile)) return;   // a checkout without the fixtures: nothing to prove
  const expect = JSON.parse(readFileSync(expectFile, "utf8"));
  const r = spawnSync(process.execPath, [SELF, dir, "--verdicts"], { encoding: "utf8" });
  const lines = `${r.stdout || ""}`.split(/\r?\n/);
  const pick = (tag, skip) => lines.filter((l) => l.startsWith(`${tag} `)).map((l) => l.slice(skip).trim());
  const verdicts = pick("VERDICT", 8);
  const stale = pick("STALE", 6);
  const bare = pick("BARE", 5);
  const exempt = Number((/exempt=(\d+)/.exec(lines.join(" ")) || [])[1] || 0);
  const same = (a, b) => JSON.stringify([...a].sort()) === JSON.stringify([...b].sort());
  const problems = [];
  if (!same(verdicts, expect.verdicts || []))
    problems.push(`findings ${JSON.stringify(verdicts.sort())} != expected ${JSON.stringify(expect.verdicts || [])}`);
  if (!same(stale, expect.stale || []))
    problems.push(`stale waivers ${JSON.stringify(stale.sort())} != expected ${JSON.stringify(expect.stale || [])}`);
  if (!same(bare, expect.bare || []))
    problems.push(`bare waivers ${JSON.stringify(bare.sort())} != expected ${JSON.stringify(expect.bare || [])}`);
  if (exempt !== (expect.exempt || 0))
    problems.push(`off-thread entry points ${exempt} != expected ${expect.exempt || 0}`);
  if (problems.length) {
    console.log(`gui_thread_selftest: FAIL ${problems.join("; ")}`);
    process.exit(1);
  }
  console.log(`gui_thread_selftest: PASS findings=${verdicts.length} stale=${stale.length} `
              + `bare=${bare.length} off-thread=${exempt}`);
}

// ---- Source files ----------------------------------------------------------
function walk(dir, out) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (name === "include") { walk(p, out); continue; }
    const st = statSync(p);
    if (st.isDirectory()) walk(p, out);
    else if (/\.(cpp|cc|hpp|h)$/.test(name)) out.push(p);
  }
  return out;
}

function walkQml(dir, out) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) walkQml(p, out);
    else if (/\.qml$/.test(name)) out.push(p);
  }
  return out;
}

function strip(text) {
  // Remove comments and literal bodies so brace matching and name matching
  // cannot be fooled by a brace inside a string.
  let out = "";
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === "/" && text[i + 1] === "/") { while (i < text.length && text[i] !== "\n") i++; out += "\n"; continue; }
    if (c === "/" && text[i + 1] === "*") { i += 2; while (i < text.length && !(text[i] === "*" && text[i + 1] === "/")) { if (text[i] === "\n") out += "\n"; i++; } i++; continue; }
    if (c === '"' || c === "'") { const q = c; i++; while (i < text.length && text[i] !== q) { if (text[i] === "\\") i++; i++; } out += q + q; continue; }
    out += c;
  }
  return out;
}

// A function definition head: `Class::method(args) {` or `Type name(args) {`.
// Heuristic: the text before the opening brace at depth 0 must look like a
// declarator and not like a lambda (`[...](`), an initializer (`=`) or a
// control statement.
const HEAD_RE =
  /(?:^|[\s\*&>])(?:([A-Za-z_]\w*)\s*::\s*)?(~?[A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?$/;
const CONTROL = /^(if|for|while|switch|return|catch|do|else|sizeof|new|delete|static_assert)$/;

// Extract {name, qualifier, startLine, endLine, body, headLineText} for every
// top-level definition in the file.
function bodies(code) {
  const clean = strip(code);
  const out = [];
  const n = clean.length;
  let depth = 0;
  let stmtStart = 0;      // start of the current top-level statement
  let line = 1;
  // LINE NUMBERS COME FROM `clean`, NOT `code`: strip() removes comment and
  // literal BODIES, so a clean-index is smaller than the code-index it came
  // from and counting newlines in `code` up to it reports a line too early —
  // far too early in a file with a big header comment (`definition:
  // EngineBridge.cpp:1139` for a body that really sat at 2100+). Both strings
  // carry the same number of lines (strip emits one newline per newline it
  // consumes), so counting in `clean` is the exact answer.
  const lineOf = (idx) => { let l = 1; for (let i = 0; i < idx; i++) if (clean[i] === "\n") l++; return l; };
  for (let i = 0; i < n; i++) {
    const c = clean[i];
    if (c === "{") {
      if (depth === 0) {
        const head = clean.slice(stmtStart, i).trim();
        const headLine = head.split("\n").pop().trim();
        const m = HEAD_RE.exec(head.replace(/\s+/g, " ").trim());
        const lastChar = head[head.length - 1];
        const isLambda = /[[\]]\s*\([^)]*\)\s*$/.test(head) || head.includes("](");
        if (m && !CONTROL.test(m[2]) && lastChar !== "=" && !isLambda) {
          // Find the matching close brace.
          let d = 1, j = i + 1;
          while (j < n && d > 0) { if (clean[j] === "{") d++; else if (clean[j] === "}") d--; j++; }
          out.push({
            cls: m[1] || null,
            name: m[2],
            startLine: lineOf(stmtStart),
            endLine: lineOf(j),
            body: clean.slice(i + 1, j - 1),
            // The line the BODY starts on, so a hit inside it can be reported
            // where it really is. `body` starts at the brace, so a body that
            // opens mid-line gives bodyStartLine == the brace's own line — the
            // same arithmetic the lock guard uses to name its offending line.
            bodyStartLine: lineOf(i + 1),
            head,
          });
          i = j - 1;
          line = lineOf(j);
          stmtStart = j;
          continue;
        }
      }
      depth++;
    } else if (c === "}") {
      if (depth > 0) depth--;
      if (depth === 0) stmtStart = i + 1;
    } else if (c === ";" && depth === 0) {
      stmtStart = i + 1;
    }
  }
  return out;
}

// `Q_INVOKABLE` and property getters, per class, from the QML-facing headers.
function entryPoints(headerText) {
  const clean = strip(headerText);
  const found = new Map();   // key: Class::name -> {cls,name,kind,line}
  // CRLF matters here, and it is not cosmetic: the Q_PROPERTY pattern below ends
  // in `(.*)$`, and `.` cannot match a `\r` — so in a file with Windows line
  // endings every property read was silently skipped (ScriptureService's `bibles`
  // and `loading` were never judged while SearchService's LF header was).
  // Splitting on `\r?\n` keeps the 1-based numbering the waivers are pinned to.
  const lines = clean.split(/\r?\n/);
  let cls = null, depth = 0;
  for (let idx = 0; idx < lines.length; idx++) {
    const raw = lines[idx];
    const mClass = /^\s*(?:class|struct)\s+([A-Za-z_]\w*)\b/.exec(raw);
    // ONLY the OUTERMOST class names the members below it. A nested helper type
    // (EngineBridge's VideoPreviewProvider lives inside the class) used to take
    // the name over and then reset \`cls\` at its own \`};\`, which put the
    // enclosing class's whole Q_INVOKABLE surface -- the tap opens, the preview
    // providers, the meters, 300+ lines of it -- beyond this sweep entirely.
    if (mClass && idx + 1 < lines.length && !/;/.test(raw) && cls === null) { cls = mClass[1]; depth = 0; }
    const add = (name, kind) => {
      if (!name || !cls) return;
      const key = `${cls}::${name}`;
      if (!found.has(key)) found.set(key, { cls, name, kind, line: idx + 1 });
    };
    for (const m of raw.matchAll(/Q_INVOKABLE\s+[^;=(]*?\b(~?\w+)\s*\(/g)) add(m[1], "Q_INVOKABLE");
    for (const m of raw.matchAll(/Q_PROPERTY\s*\((.*)$/g)) {
      // Q_PROPERTY(Type name READ getter [WRITE ...] [NOTIFY ...]) — may wrap.
      let spec = raw.slice(raw.indexOf("Q_PROPERTY("));
      let k = idx;
      while (!spec.includes(")") && k + 1 < lines.length) spec += " " + lines[++k].trim();
      const read = /\bREAD\s+(\w+)/.exec(spec);
      if (read) add(read[1], "property READ");
    }
    for (let i = 0; i < raw.length; i++) { if (raw[i] === "{") depth++; else if (raw[i] === "}") depth = Math.max(0, depth - 1); }
    if (depth === 0 && /};\s*$/.test(raw)) cls = null;
  }
  return found;
}

// ---- The second sweep: a sync call where the service offers an async one ----
// The C++ sweep above finds entry points that can park the interface thread.
// This one finds the places that call them when the SAME operation is already
// available off-thread — `search` next to `searchAsync`, `chapter` next to
// `chapterAsync`, `resolveWords` next to `resolveWordsAsync`. Those are the
// cheapest fixes in the codebase: the worker, the token guard and the "newest
// request wins" contract already exist, and the call site simply did not use
// them yet. A per-keystroke search box that runs the corpus scan on the
// interface thread is exactly this shape, and it is invisible to the C++ scan
// because the service it calls is async-capable and only the QML chose not to.
function asyncPairs(files) {
  const pairs = new Map();   // Class -> Set(base name)
  for (const file of files) {
    if (!/\.(hpp|h)$/.test(file)) continue;
    for (const [key, ep] of entryPoints(readFileSync(file, "utf8"))) {
      const m = /^(.*)Async$/.exec(ep.name);
      if (!m) continue;
      if (!pairs.has(ep.cls)) pairs.set(ep.cls, new Set());
      pairs.get(ep.cls).add(m[1]);
    }
  }
  return pairs;
}

function qmlSyncFindings(qmlFiles, pairs) {
  const out = [];
  for (const file of qmlFiles) {
    const lines = readFileSync(file, "utf8").split(/\r?\n/);
    lines.forEach((raw, idx) => {
      // A `sync-ok: <reason>` comment waives the line, same contract as the
      // other guards.
      const w = /sync-ok:(.*)$/.exec(raw);
      const reason = w ? w[1].replace(/\*\/\s*$/, "").trim() : null;
      const code = strip(raw);
      // Adapter DEFINITIONS (`readonly property var search: (...) =>
      // Service.search(...)`) are deliberately both-sided: the pane picks
      // async when the adapter offers it, so the definition itself is not a
      // call site. The call sites are what this checks.
      const isDefinition = /property\s+var\s+\w+\s*:.*=>/.test(code) || /function\s*\w*\s*\(.*=>\s*\w+\./.test(code);
      for (const m of code.matchAll(/\b(\w+)\s*\.\s*(\w+)\s*\(/g)) {
        const cls = m[1], name = m[2];
        if (!pairs.has(cls) || !pairs.get(cls).has(name)) continue;
        if (isDefinition) continue;
        if (reason !== null && reason.length > 0) continue;
        out.push({
          file, line: idx + 1, key: `${cls}.${name}`, text: raw.trim(),
          missingReason: reason !== null && reason.length === 0,
        });
      }
    });
  }
  return out;
}

// ---- The third sweep: a SYNCHRONOUS image-provider load -----------------------
// The two sweeps above only see what QML CALLS (an invokable, a getter). A QQuickImageProvider is not
// called by anything the C++ scan walks: QML hands it a URL, and with `asynchronous: false` Qt runs
// requestImage() ON THE INTERFACE THREAD. The live-preview tiles did exactly that — a full-frame copy, a
// scan and a scale per tile per revision — and the guard reported "0 new HARD" the whole time. So: any
// `Image { asynchronous: false }` whose source is an `image://` URL (written inline, or held in a property
// that builds one) is a finding. Fix: load it asynchronously (LiveFrameImage keeps a live frame from blanking),
// or waive the block with `// gui-ok: <why this provider is cheap or interface-thread-owned>`.
function qmlSyncImageFindings(qmlFiles) {
  const out = [];
  for (const file of qmlFiles) {
    const lines = readFileSync(file, "utf8").split(/\r?\n/);
    const imageProps = new Set();   // properties whose definition builds an image:// URL
    lines.forEach((line, i) => {
      if (!/image:\/\//.test(line)) return;
      for (let j = i; j >= Math.max(0, i - 10); --j) {
        const m = /property\s+\w+\s+(\w+)\s*:/.exec(lines[j]);
        if (m) { imageProps.add(m[1]); break; }
      }
    });
    for (let i = 0; i < lines.length; ++i) {
      if (!/^\s*Image\s*\{/.test(lines[i])) continue;
      let depth = 0, end = i;
      for (let j = i; j < lines.length; ++j) {
        for (const ch of strip(lines[j])) { if (ch === "{") depth++; else if (ch === "}") depth--; }
        if (depth <= 0) { end = j; break; }
      }
      const block = lines.slice(i, end + 1).join("\n");
      if (!/asynchronous\s*:\s*false/.test(block)) continue;
      const src = lines.slice(i, end + 1).find((l) => /^\s*source\s*:/.test(l)) || "";
      const viaProvider = /image:\/\//.test(src)
        || [...imageProps].some((p) => new RegExp(`\\b${p}\\b`).test(src));
      if (!viaProvider) continue;
      const waiver = /gui-ok:(.*)$/m.exec(block);
      if (waiver && waiver[1].trim().length > 0) continue;
      out.push({ file, line: i + 1, text: src.trim(), bare: !!waiver });
    }
  }
  return out;
}

const argv = process.argv.slice(2);
const listMode = argv.includes("--list");
const qmlMode = argv.includes("--qml");
const allMode = argv.includes("--all");
const verbose = argv.includes("--verbose");
const updateBaseline = argv.includes("--update-baseline");
// Machine-readable output, used by the self-test below (and by anything else that
// wants this guard's verdicts as data instead of prose).
const verdictMode = argv.includes("--verdicts");
const args = argv.filter((a) => !a.startsWith("--"));

// The fixture self-test runs on every ordinary scan, but NOT from inside the
// child it spawns (`--verdicts` is that child) and not when the baseline is
// being rewritten, which is about the real tree. It has to sit after the argv
// parse: `verdictMode` is a `const` below this point in the module, so a call
// above it threw `Cannot access 'verdictMode' before initialization` — the
// guard failed to start at all rather than scanning (caught by running it).
if (!verdictMode && !updateBaseline) runSelfTest();

// Default scope: the two directories whose functions QML can call directly.
const targets = args.length ? args : ["qml/services", "qml/models"];

const files = [];
for (const t of targets) {
  const st = statSync(t);
  if (st.isDirectory()) walk(t, files);
  else files.push(t);
}

// ---- Index the source -------------------------------------------------------
const index = new Map();   // class -> { file, defs: Map(name -> def) }
const allDefs = [];        // every definition, for name-based linking
for (const file of files) {
  const text = readFileSync(file, "utf8");
  const defs = bodies(text);
  for (const d of defs) {
    d.file = file;
    allDefs.push(d);
    if (d.cls) {
      if (!index.has(d.cls)) index.set(d.cls, { file, defs: new Map() });
      index.get(d.cls).defs.set(d.name, d);
    }
  }
}

// Blocking verdict per definition, computed as a fixpoint so a chain of private
// helpers resolves to the primitive at the end of it.
// ---- The OFF-THREAD SPAN ----------------------------------------------------
// The primitives that really hand work to ANOTHER thread. Deliberately NOT the
// whole `OFF_THREAD_HOP` list: `QMetaObject::invokeMethod(this, …, QueuedConnection)`
// is a call BACK to this object's own thread, so code inside its lambda is not
// off-thread work — it is this thread, later. (`EngineWorker::onInterfaceThread`
// is that same shape and is how a worker's answer is published.) The entry-point
// exemption keeps the wider list, because there the question is "did the entry
// point hand its device work away before doing anything", and a hop-named helper
// (`openTapOffThread`) answers it. Here the question is narrower and must be
// answered literally: does this primitive's call happen on another thread?
const OFF_THREAD_SPAN =
  /QThread::create\s*\(|QThreadPool|QtConcurrent|\b\w*(?:OffThread|AsyncWork|Worker)\w*\s*\(|\bstd::thread\b|\b\w*(?:[Tt]hread|[Ww]orker)\w*\s*(?:->|\.)\s*start\s*\(\s*\)/g;

// The argument region of every hop call in `body`: the text that runs on another
// thread. A primitive found inside one of these is not on the caller's thread,
// which is the exemption an entry point already gets for hopping itself —
// without it a helper that reads the engine on a worker still labelled every
// caller that queued it (`via:refreshMarksOffThread:cross-engine singleton`),
// which is the guard reporting a wait that does not exist. A balanced-paren scan,
// so it is textual like the rest of this file: a paren inside a string literal or
// a comment can end a span early, and a span that is missed only ever reports
// MORE, never less.
// The balanced region of the call the hop NAMES: the first `(` group after the
// match, or — when that group is empty/argument-less and a later one carries the
// lambda — the first group that actually holds a `{` block.
//
// That second case is not a detail: `QThreadPool::globalInstance()->start([…]{…})`
// is how this codebase posts pool work, and the first `(` after `QThreadPool`
// belongs to `globalInstance()`, whose matching `)` closes long before the
// lambda. Reading the span as that first group put the WORK — the engine call
// inside the lambda — back on the caller's thread, which is the opposite of what
// the shape means. Bounded look-ahead (a handful of groups) so a pathological
// body cannot make this scan expensive.
function spanOf(body, fromIdx) {
  let first = null;
  let i = body.indexOf("(", fromIdx);
  for (let groups = 0; i >= 0 && groups < 4; ++groups) {
    let depth = 0, end = -1, hasBrace = false;
    for (let j = i; j < body.length; ++j) {
      const c = body[j];
      if (c === "{") hasBrace = true;
      else if (c === "(") depth++;
      else if (c === ")") {
        depth--;
        if (depth === 0) { end = j; break; }
      }
    }
    if (end < 0) break;
    if (first === null) first = [fromIdx, end];
    if (hasBrace) return [fromIdx, end];   // the group carrying the work
    i = body.indexOf("(", end);
  }
  return first;
}

function offThreadSpans(body) {
  const spans = [];
  const re = new RegExp(OFF_THREAD_SPAN.source, "g");
  let m;
  while ((m = re.exec(body)) !== null) {
    const span = spanOf(body, m.index);
    if (span) spans.push(span);
    if (re.lastIndex === m.index) re.lastIndex = m.index + 1;   // zero-length guard
  }
  return spans;
}

// One hit per rule, with the text it matched and the LINE it is on — the label
// alone ("engine bridge / logger") does not say which member call earned it,
// and the fix is per-call. Line arithmetic: `body` begins at the definition's
// brace, so the hit's line is bodyStartLine plus the newlines before it.
function directHits(def) {
  const body = def.body;
  const spans = offThreadSpans(body);
  const inSpan = (i) => spans.some(([a, b]) => i >= a && i <= b);
  const lineAt = (k) => (def.bodyStartLine || def.startLine) + (body.slice(0, k).match(/\n/g) || []).length;
  const hits = [];
  for (const [re, label] of BLOCKING) {
    const g = new RegExp(re.source, re.flags.includes("g") ? re.flags : re.flags + "g");
    let m;
    while ((m = g.exec(body)) !== null) {
      if (!inSpan(m.index)) {
        hits.push({ label, text: m[0].replace(/\s+/g, " ").trim(), line: lineAt(m.index) });
        break;
      }
      if (g.lastIndex === m.index) g.lastIndex = m.index + 1;
    }
  }
  return hits;
}

const HARD_LABELS = new Set(HARD.map(([, l]) => l));

const defsByFile = new Map();
for (const d of allDefs) {
  if (!defsByFile.has(d.file)) defsByFile.set(d.file, []);
  defsByFile.get(d.file).push(d);
}

// A definition that HANDS ITS WORK to another thread contributes no label to its
// callers — the same exemption an ENTRY POINT gets when it hops, applied one
// level down, because the reason is identical: the blocking call is not running
// on the interface thread. Without it a helper that posts a job and keeps only
// bookkeeping (`LiveOutputService::armNdiSenderForSession`, which resolves a
// provider pointer and posts the SDK reset through a queued invoke; the bridge's
// `refreshNdiConnections`, whose SDK query starts on the pool) still labelled
// every command that reached it. The same looseness as the entry-point rule
// applies and is deliberate: a hop ANYWHERE in the body counts, because "did
// this hand the device work away" is the question the guard is answering, and a
// helper that blocks on the caller's thread BEFORE posting is the one shape this
// cannot see.
function hopsAway(def) {
  return OFF_THREAD_HOP.test(def.body);
}

const blocking = new Map();   // def -> {labels:Set, via:name}
const directHitsOf = new Map();   // def -> [{label, text, line}] (for the report)
for (const d of allDefs) {
  const hits = directHits(d);
  directHitsOf.set(d, hits);
  blocking.set(d, new Set(hits.map((h) => h.label)));
}
let changed = true;
let rounds = 0;
while (changed && rounds++ < 12) {
  changed = false;
  for (const d of allDefs) {
    const set = blocking.get(d);
    if (set.size && ![...set].some((s) => s.startsWith("via:"))) {
      // Already has a direct primitive; still record call edges for messages.
    }
    for (const callee of defsByFile.get(d.file) || []) {
      if (callee === d) continue;
      if (!new RegExp(`\\b${callee.name}\\s*\\(`).test(d.body)) continue;
      if (hopsAway(callee)) continue;   // posts its work: the caller inherits nothing
      const cs = blocking.get(callee);
      if (!cs) continue;
      for (const label of cs) {
        const via = label.startsWith("via:") ? label : `via:${callee.name}:${label}`;
        if (!set.has(via)) { set.add(via); changed = true; }
      }
    }
    // Also link across files by class (a service calling its own helper that
    // lives in another translation unit of the same class is rare here, but a
    // static helper in the same class is not).
    if (d.cls && index.has(d.cls)) {
      for (const [name, callee] of index.get(d.cls).defs) {
        if (callee === d || callee.file === d.file) continue;
        if (!new RegExp(`\\b${name}\\s*\\(`).test(d.body)) continue;
        if (hopsAway(callee)) continue;   // posts its work: the caller inherits nothing
        const cs = blocking.get(callee);
        if (!cs) continue;
        for (const label of cs) {
          const via = label.startsWith("via:") ? label : `via:${name}:${label}`;
          if (!set.has(via)) { set.add(via); changed = true; }
        }
      }
    }
  }
}

// ---- Judge the entry points -------------------------------------------------
const findings = [];
const stale = [];
let waivers = 0;
let exempted = 0;

for (const file of files) {
  if (!/\.(hpp|h)$/.test(file)) continue;
  const headerText = readFileSync(file, "utf8");
  const eps = entryPoints(headerText);
  if (eps.size === 0) continue;
  const headerLines = headerText.split(/\r?\n/);
  const headerDefs = bodies(headerText).map((d) => ({ ...d, file }));
  for (const [key, ep] of eps) {
    // Definition: inline in the header, else the .cpp of the same base name,
    // else any file that defines Class::name.
    let def = headerDefs.find((d) => d.cls === ep.cls && d.name === ep.name) || null;
    if (!def) {
      const sibling = files.filter((f) => /\.(cpp|cc)$/.test(f) && basename(f).replace(/\.\w+$/, "") === basename(file).replace(/\.\w+$/, ""));
      for (const s of sibling) {
        const d = (defsByFile.get(s) || []).find((x) => x.cls === ep.cls && x.name === ep.name);
        if (d) { def = d; break; }
      }
    }
    if (!def) def = (index.get(ep.cls)?.defs.get(ep.name)) || null;
    if (!def) continue;   // declaration only, no body to judge
    const set = blocking.get(def);
    const hops = OFF_THREAD_HOP.test(def.body);
    const reasons = [...set].filter((s) => !s.startsWith("via:"));
    const chains = [...set].filter((s) => s.startsWith("via:"));
    // A chain inherits the severity of the primitive it ends in (`via:x:Sleep`).
    const hardReached = [...set].some((s) => HARD_LABELS.has(s) || [...HARD_LABELS].some((h) => s.endsWith(`:${h}`)));
    const severity = hardReached ? "hard" : "soft";

    // Where would the waiver sit? On the entry point's declaration.
    const declLine = headerLines[ep.line - 1] || "";
    const w = /gui-ok:(.*)$/.exec(declLine);
    const hasWaiver = w !== null;
    const reason = w ? w[1].replace(/\*\/\s*$/, "").trim() : null;

    if (!set.size) {
      if (hasWaiver && reason.length > 0) stale.push({ file, line: ep.line, key, why: "no blocking call reachable any more" });
      else if (hasWaiver) findings.push({ file, line: ep.line, key, labels: ["gui-ok without a reason"], kind: ep.kind, def });
      continue;
    }
    if (hasWaiver && reason.length === 0) {
      findings.push({ file, line: ep.line, key, labels: ["gui-ok without a reason"], kind: ep.kind, def });
      continue;
    }
    if (hasWaiver) { waivers++; continue; }
    if (hops) { exempted++; continue; }
    findings.push({ file, line: ep.line, key, labels: reasons.length ? reasons : chains, chain: chains, kind: ep.kind, def, severity,
                    hits: directHitsOf.get(def) || [] });
  }
}

// ---- The QML sweep -----------------------------------------------------------
const qmlFiles = [];
for (const root of ["qml"]) {
  try { walkQml(root, qmlFiles); } catch { /* no qml/ in scope: skip the sweep */ }
}
const pairs = asyncPairs(files);
const qmlFindings = qmlFiles.length ? qmlSyncFindings(qmlFiles, pairs) : [];
const imageFindings = qmlFiles.length ? qmlSyncImageFindings(qmlFiles) : [];

// ---- Report -----------------------------------------------------------------
const rel = (p) => relative(ROOT, p).split(sep).join("/");
const BASELINE = "tools/gui_thread_baseline.json";
const baseline = existsSync(BASELINE) ? new Set(JSON.parse(readFileSync(BASELINE, "utf8")).keys || []) : new Set();
const key = (f) => `${rel(f.file)}:${f.key}`;

if (updateBaseline) {
  const keys = [...findings.map(key), ...qmlFindings.map((f) => `${rel(f.file)}:${f.key}`)].sort();
  writeFileSync(BASELINE, JSON.stringify({ note: "Known GUI-thread blocking entry points. Shrink with `node tools/check_gui_thread_blocks.mjs --update-baseline` as they are fixed.", keys }, null, 2) + "\n");
  console.log(`check_gui_thread_blocks: baseline rewritten — ${keys.length} known entry point(s) → ${BASELINE}`);
  process.exit(0);
}

const fresh = findings.filter((f) => !baseline.has(key(f)));
const known = findings.filter((f) => baseline.has(key(f)));
const hardFresh = fresh.filter((f) => f.severity === "hard");
const softList = findings.filter((f) => f.severity === "soft");
const qmlFresh = qmlFindings.filter((f) => !baseline.has(`${rel(f.file)}:${f.key}`));

if (verdictMode) {
  for (const f of findings) console.log(`VERDICT ${f.severity || "waiver"} ${f.key}`);
  for (const s of stale) console.log(`STALE ${s.key}`);
  for (const f of findings.filter((x) => x.labels.includes("gui-ok without a reason")))
    console.log(`BARE ${f.key}`);
  console.log(`SUMMARY hard=${hardFresh.length} exempt=${exempted} waivers=${waivers}`);
} else if (listMode) {
  for (const f of findings) console.log(`${f.severity === "hard" ? "HARD" : "soft"} ${rel(f.file)}:${f.line} ${f.key}`);
} else if (allMode) {
  for (const f of findings) {
    console.log(`${f.severity === "hard" ? "HARD" : "soft"} ${rel(f.file)}:${f.line} [${f.kind}] ${f.key} — ${f.labels.join(", ")}`);
    // The offending line itself, where the finding is a DIRECT one (a chain
    // names the helper it went through instead, which --verbose prints).
    for (const h of f.hits) console.log(`      ${rel(f.def.file)}:${h.line} [${h.label}] ${h.text}`);
  }
} else {
  for (const f of hardFresh) {
    console.log(`HARD ${rel(f.file)}:${f.line} [${f.kind}] ${f.key}`);
    console.log(`    blocks on: ${f.labels.join(", ")}`);
    for (const h of f.hits) console.log(`      ${rel(f.def.file)}:${h.line} [${h.label}] ${h.text}`);
    if (verbose && f.chain.length) for (const c of f.chain.slice(0, 4)) console.log(`      ${c}`);
    console.log(`    definition: ${rel(f.def.file)}:${f.def.startLine}`);
    console.log(`    fix: hop off the interface thread (Qt::QueuedConnection / a worker thread),`);
    console.log(`         or waive with "// gui-ok: <what makes this call safe>" on the declaration.`);
  }
  for (const s of stale) console.log(`STALE WAIVER ${rel(s.file)}:${s.line} ${s.key} — ${s.why}`);
  for (const f of qmlFresh) {
    console.log(`SYNC ${rel(f.file)}:${f.line} ${f.key}(...) runs on the interface thread`);
    console.log(`    ${f.text}`);
    console.log(`    fix: use ${f.key}Async(...) (the worker, the token guard and the`);
    console.log(`         newest-request-wins contract already exist), or waive the line with`);
    console.log(`         "// sync-ok: <what makes this call cheap>" — a stale waiver fails too.`);
  }
  for (const f of imageFindings) {
    console.log(`SYNC-IMAGE ${rel(f.file)}:${f.line} an Image with asynchronous: false loads an image:// provider ON the interface thread`);
    console.log(`    ${f.text}`);
    console.log(`    fix: load it asynchronously (LiveFrameImage double-buffers a live frame so it never blanks),`);
    console.log(`         or waive the block with "// gui-ok: <why this provider is cheap or interface-thread-owned>".`);
  }
  if (qmlMode) for (const f of qmlFindings) console.log(`SYNC ${rel(f.file)}:${f.line} ${f.key}`);
}

// A `gui-ok:` with no reason is a finding in its own right (AGENTS.md: "a bare
// gui-ok: is itself a finding"), so it GATES like one — the stale-waiver rule
// already fails the build, and a waiver that cannot say what makes the call safe
// is the same failure one step earlier.
const bareWaivers = findings.filter((f) => f.labels.includes("gui-ok without a reason"));
const summary =
  `check_gui_thread_blocks: ${hardFresh.length} new HARD (unbounded-wait) entry point(s), ` +
  `${qmlFresh.length} sync-where-async-exists QML call site(s), ` +
  `${imageFindings.length} sync image-provider load(s), ` +
  `${known.length} known (baselined), ${softList.length} soft (lock/singleton inventory), ` +
  `${waivers} waived, ${stale.length} stale waiver(s), ${bareWaivers.length} bare waiver(s), ` +
  `${exempted} already off-thread.`;
console.log(summary);
// Only a hard finding gates the build; the soft inventory is for the audit,
// and `--all` prints it. The QML sync sweep gates too: it names a call site
// whose async twin already exists, so leaving one is a decision, not an
// oversight, and it costs nothing to state it.
process.exit(hardFresh.length || stale.length || qmlFresh.length || imageFindings.length || bareWaivers.length ? 1 : 0);
