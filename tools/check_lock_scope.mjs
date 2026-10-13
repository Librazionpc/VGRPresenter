#!/usr/bin/env node
// check_lock_scope.mjs — lock-scope regression guard.
//
// Flags a module lock (lock_guard / unique_lock / scoped_lock / shared_lock or
// an explicit .lock()) that is held across a provider, PAL or cross-engine call,
// or across a CALLBACK/EVENT DISPATCH (EventBus::Publish, a listener
// notification, a stored std::function member, std::invoke/visit/apply).
//
// The dispatch half matters for the same reason the provider half does: the
// callback body is not visible from the calling line, so a subscriber that
// takes payments from a device, a renderer that re-enters the engine, or a
// listener that saves to disk all run *inside* the lock — and every other
// thread that needs that lock waits behind arbitrary third-party code. So a
// dispatch site under a lock is reported unless the callback is provably
// trivial (see the waiver rules in AGENTS.md's "Lock-scope guard" section).
//
// That pattern is what turned a blocking NDI/encoder/disk call into an
// app-wide lock pile-up (see the recording-open hang).
//
// It is a *heuristic* source scan, not a compiler: it tracks brace depth to
// approximate scope and reports a finding when a "blocking-ish" call appears
// while a lock is in scope.
//
// Wired into the build (CMake `lock_scope_check`, part of the default build) so
// a new violation fails it. Bypass a whole build with
// -DBPS_ENABLE_LOCK_SCOPE_CHECK=OFF — a last resort; prefer a waiver.
//
// Waivers: a finding is silenced by a `lock-ok: <reason>` comment on the
// flagged line. The reason is MANDATORY and must say what makes that specific
// call safe — a bare `lock-ok:` is itself a finding, and the build output lists
// waived count so the waivers stay auditable (`grep -rn "lock-ok:"`).
//
// Waivers go STALE: the call they justified is deleted, hoisted out of the lock,
// or replaced by an in-memory one — and the comment then sits on a line the
// guard no longer flags, misrepresenting it as audited. Both stale shapes are
// themselves findings:
//   * the waived line no longer has a blocking/dispatch call on it, or
//   * the waived line is no longer inside a lock scope at all
// so a `lock-ok:` survives only while it is still waiving a live finding.
//
// Usage:
//   node tools/check_lock_scope.mjs [paths...]        # default: src/PresentationEngine/modules
//                                                     #   + src/PresentationEngine/core/resources
//                                                     #   + src/PresentationEngine/platform
//   node tools/check_lock_scope.mjs --list            # compact file:line list, still non-zero
//   node tools/check_lock_scope.mjs --update-baseline # rewrite the known-findings ledger
//
// core/resources is in the default scope because ResourceManager's own sampler
// held its mutex_ across an EventBus dispatch, whose subscriber re-enters the
// manager — freezing the GUI thread (a settings write waits on the
// AdaptiveRuntime lock the deadlocked sampler owns) instead of failing in the
// module that has the bug.
//
// platform/ is in the default scope for the same reason one layer down: the PAL
// is where a thread is handed to the OPERATING SYSTEM — a device read, a Win32
// wait object, a socket, a capture callback — and those waits have no bound at
// all, so a lock held across one blocks every thread that needs it for as long
// as the driver feels like taking (a camera that stops delivering frames, an
// HDMI unplug, an NDI stream that never answers). The blocking rules there are
// the OS calls themselves (see the PAL-side waits below), not the module names
// the other roots are full of.
//
// The ONE wait that is not a lock held across a wait is `cv.wait(lock)`: it
// releases the lock it is handed for the duration by design, so it is reported
// only when another lock is also in scope, or when the lock it is handed is one
// this scan never saw declared (see cvWaitReleasesOnlyLock).
//
// Exit: 0 = clean (or only baselined/fixed sites), 1 = new findings or stale waivers.

import { readFileSync, readdirSync, statSync, writeFileSync, existsSync } from "node:fs";
import { join, relative, sep } from "node:path";

const ROOT = process.cwd();

const LOCK_DECL =
  /\b(std::)?(lock_guard|unique_lock|scoped_lock|shared_lock)\s*<[^>]*>\s*\w+\s*\(|\b\w+\.(lock|lock_shared)\s*\(/;

// Calls that can block: PAL, filesystem, vendor SDKs, other engines, the DB.
const BLOCKING = [
  [/\bPlatformAccessor::Get\s*\(/, "PAL (PlatformAccessor)"],
  [/\bstd::filesystem::/, "PAL (std::filesystem)"],
  [/\b(BroadcastEngine|DisplayEngine|MediaEngine|RenderEngine|ProductionEngine|RecordingEngine|FlowEngine|SceneCompositionEngine|NotificationService|DatabaseManager|BibleEngine|SongEngine|SearchEngine|ProjectLibrary|DesignLibrary|WorkspaceManager)::Instance\s*\(/, "cross-engine singleton"],
  // Dispatch: the callback runs inside the lock.
  [/\bEventBus::Instance\s*\(\s*\)\s*\.\s*(\w+)\s*\(/, "event-bus dispatch"],
  [/(\.|->)\s*(Publish|Emit|Dispatch|DispatchEvent|Notify|NotifyAll|Fire|Broadcast)\s*\(/, "listener notification"],
  [/\b(Publish|Emit|DispatchEvent|NotifyAll|Broadcast)\s*\(/, "listener notification"],
  [/\bstd::(invoke|visit|apply)\s*\(/, "callback invocation (std)"],
  [/->\s*(Probe|Discover\w*|Enumerate\w*|CreateSender|CreateReceiver|SendVideo|SendAudio|SendVideoFrame|ReceiveFrame|StartSender|StopSender|Disconnect\w*|ShutdownProvider|Open|Close|Start|Stop)\s*\(/, "provider call"],
  [/\bstd::this_thread::sleep_for\b/, "sleep under a lock"],
  // ---- PAL-side waits -------------------------------------------------------
  // The platform layer is scanned by default (see the usage note above), and
  // inside it the blocking call is rarely a provider or another engine: it is
  // the operating system's own wait. These are the shapes measured in
  // platform/ today — a device read, a sleep, a condition-variable wait, a
  // socket, a Win32 wait object — and each one parks the thread for an
  // unbounded time, which is exactly what makes holding a lock across it fatal
  // rather than merely slow.
  [/\b(WaitForSingleObject|WaitForMultipleObjects|WaitForMultipleObjectsEx|MsgWaitForMultipleObjects|MsgWaitForMultipleObjectsEx|CoWaitForMultipleHandles)\s*\(/, "PAL (Win32 wait)"],
  // Both spellings of a sleep: the OS's own, and Qt's (QThread's statics, which
  // the app-side code reaches for) — a thread parked for a fixed time is the
  // same hazard whichever API parked it.
  [/\b(SleepEx|Sleep|QThread::sleep|msleep|usleep)\s*\(/, "PAL (sleep)"],
  [/\.wait(_for|_until)?\s*\(/, "condition-variable wait"],
  [/\b(ReadFile|ReadFileEx|WriteFile|WriteFileEx|DeviceIoControl)\s*\(/, "PAL (blocking device I/O)"],
  [/\b(recv|recvfrom|send|sendto|accept|connect|select)\s*\(/, "PAL (blocking socket)"],
  [/\bDwmFlush\s*\(/, "PAL (DWM flush)"],
];

// The name a lock declaration binds, so a wait that is HANDED that lock can be
// judged instead of guessed at (see cvWaitReleasesOnlyLock). `std::scoped_lock
// lk(m);` and `std::lock_guard<std::mutex> lk(m);` both count; CTAD forms with
// no template arguments count too. An explicit `mutex_.lock()` binds no name —
// there is nothing to hand a wait, so those stay unexempted.
function lockVarName(line) {
  const templated = /\bstd::(?:lock_guard|unique_lock|scoped_lock|shared_lock)\s*<[^>]*>\s*(\w+)\s*[({;,]/.exec(line);
  if (templated) return templated[1];
  const deduced = /\bstd::(?:lock_guard|unique_lock|scoped_lock)\s+(\w+)\s*[({;,]/.exec(line);
  if (deduced) return deduced[1];
  // An explicit `lock.lock()` / `lock.lock_shared()` re-acquires a lock that is
  // already known by name — the same lock, so a wait handed that name is
  // releasing the same object (this is the unlock/re-lock pair around a
  // blocking cancellation call, a pattern this codebase uses on purpose).
  const explicit = /\b(\w+)\s*\.\s*(?:lock|lock_shared)\s*\(/.exec(line);
  return explicit ? explicit[1] : null;
}

// The one "wait" that does NOT hold a lock across it: `cv.wait(lock)` (and
// wait_for/wait_until) RELEASES the lock it is handed for the duration and
// re-acquires it before returning — that is what the call is for. So a wait is
// exempt only when the lock it is handed is the ONLY lock in scope: with a
// second lock also held, that one really is held across the wait, which is the
// hazard this guard exists for (and the shape it must keep reporting).
// A wait on a lock this scan never saw declared (a by-reference parameter, a
// member) is reported: the reasoning then has to be visible, not assumed.
function cvWaitReleasesOnlyLock(line, active) {
  const m = /\.wait(?:_for|_until)?\s*\(\s*(\w+)/.exec(line);
  if (!m) return false;
  const handed = m[1];
  return active.length > 0 && active.every((a) => a.var === handed);
}

// A callable-owned member (`fn_(...)`, `onFrame_(...)`, `handler_(...)`) is a
// stored std::function / functor member: trailing-underscore members are state,
// never methods, so a trailing `_(` is a callback invocation.
const CALLBACK_MEMBER = /\b\w+_\s*\(/;

// Local std::function variables are found per file (they cannot be spotted from
// the calling line): every `std::function<...> name = ...` becomes a name that
// the scan below treats like CALLBACK_MEMBER. A local initialised directly from
// a lambda is exempt — its body sits in this same function, a few lines up, so
// the auditor can read it (a local recursive `walk` helper is the common case).
// It stops being exempt the moment the name is assigned something else, since
// that source is then opaque again.
const FN_ASSIGN = /\b(\w+)\s*=\s*([^=][^;]*);/g;

function localFunctionNames(text) {
  const names = new Set();
  const lambdaInit = new Set();
  // `std::function<...>` template args may nest, and a `->` arrow inside them
  // is not the closer, so scan for the matching `>` by depth instead of a regex.
  const head = /\bstd::function\s*</g;
  let m;
  while ((m = head.exec(text))) {
    let i = head.lastIndex;
    let depth = 1;
    while (i < text.length && depth > 0) {
      if (text[i] === "-" && text[i + 1] === ">") {
        i += 2;
        continue;
      }
      if (text[i] === "<") depth++;
      else if (text[i] === ">") depth--;
      i++;
    }
    if (depth !== 0) continue;
    head.lastIndex = i;
    const tail = /^\s*(\w+)\s*([={,(])/.exec(text.slice(i));
    if (!tail) continue;
    const after = text.slice(i + tail[0].length).replace(/^\s*/, "");
    // `= [&](…) {…}` — a lambda literal defined right here: not opaque.
    if (tail[2] === "=" && after.startsWith("[")) lambdaInit.add(tail[1]);
    else names.add(tail[1]);
  }
  // Re-assigning an exempt name from anything but another lambda literal makes
  // the object opaque again (the body is no longer at the declaration).
  for (const m2 of text.matchAll(FN_ASSIGN)) {
    if (!lambdaInit.has(m2[1])) continue;
    if (m2[2].trim().startsWith("[")) continue;
    names.add(m2[1]);
  }
  return names;
}

function strip(code) {
  // Remove // comments and string/char literals so brace counting stays sane.
  let out = "";
  for (let i = 0; i < code.length; i++) {
    const c = code[i];
    if (c === "/" && code[i + 1] === "/") break;
    if (c === '"' || c === "'") {
      const q = c;
      i++;
      while (i < code.length && code[i] !== q) {
        if (code[i] === "\\") i++;
        i++;
      }
      out += q + q;
      continue;
    }
    out += c;
  }
  return out;
}

function walk(dir, out) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) walk(p, out);
    else if (/\.(cpp|cc|hpp|h)$/.test(name)) out.push(p);
  }
  return out;
}

const argv = process.argv.slice(2);
const listMode = argv.includes("--list");
const args = argv.filter((a) => !a.startsWith("--"));
// The default scope is the feature modules, core/resources and the PLATFORM
// layer — the three places a lock is held across something that can park the
// thread. CMake passes the same three roots explicitly
// (BPS_LOCK_SCOPE_TARGET), so the build gate and this default cannot drift.
const targets = args.length
  ? args
  : [
      "src/PresentationEngine/modules",
      "src/PresentationEngine/core/resources",
      "src/PresentationEngine/platform",
    ];
const files = [];
for (const t of targets) {
  const st = statSync(t);
  if (st.isDirectory()) walk(t, files);
  else files.push(t);
}

const findings = [];
const stale = [];
let waivers = 0;
for (const file of files) {
  // POSIX separators in every reported/ledger path, so the baseline is not
  // Windows-only.
  const rel = relative(ROOT, file).split(sep).join("/");
  const text = readFileSync(file, "utf8");
  const lines = text.split(/\r?\n/);
  let depth = 0;
  const active = []; // { depth, line, text }

  // Names of local std::function objects in this file — calling one is a
  // callback invocation, same class of hazard as a stored member.
  const fnNames = localFunctionNames(text);

  lines.forEach((raw, idx) => {
    const line = strip(raw);
    const no = idx + 1;
    // `lock-ok: <reason>` waives the line; the reason is required, so a bare
    // `lock-ok:` (or one followed only by the closing comment) is a finding.
    const m = /lock-ok:(.*)$/.exec(raw);
    const reason = m ? m[1].replace(/\*\/\s*$/, "").trim() : null;
    const hasWaiver = reason !== null;

    // Resolve what this line would trip, independent of any waiver, so a waiver
    // can be judged against exactly the signal a finding would use.
    // A lock declaration line is never itself a held-across-call site.
    let hit = null;
    if (!LOCK_DECL.test(line)) {
      // Callback dispatch that is not a named engine/bus call: a stored
      // std::function member or a local std::function variable.
      let cb = null;
      if (CALLBACK_MEMBER.test(line)) cb = "stored callback (member)";
      else if (!/\bstd::function\b/.test(line)) {
        for (const name of fnNames) {
          if (new RegExp(`\\b${name}\\s*\\(`).test(line)) {
            cb = "std::function call";
            break;
          }
        }
      }
      if (cb) hit = cb;
      else {
        for (const [re, label] of BLOCKING) {
          if (!re.test(line)) continue;
          // The release-and-wait idiom is not a lock held across a wait.
          if (label === "condition-variable wait" && cvWaitReleasesOnlyLock(line, active))
            continue;
          hit = label;
          break;
        }
      }
    }
    const lockAt = active.length ? active[active.length - 1].line : null;
    // Ledger key: the line with comments and literal contents removed, so
    // appending a comment or renaming a string payload never re-keys a finding.
    const sig = strip(raw).replace(/\s+/g, " ").trim();

    if (hasWaiver && reason.length === 0) {
      // Report the missing justification itself and skip the blocking rule for
      // this line: one finding per commented-out site, not two.
      findings.push({
        file: rel,
        line: no,
        lock: "-",
        label: "lock-ok without a reason",
        text: raw.trim(),
        sig,
      });
    } else if (hasWaiver) {
      // A waiver has to still be doing work. If the line no longer trips a rule
      // (call deleted / hoisted / replaced by an in-memory one) or the lock that
      // made it necessary is gone, the comment is stale and misrepresents the
      // line as audited — report it instead of counting it as justified.
      if (hit && lockAt !== null) waivers++;
      else
        stale.push({
          file: rel,
          line: no,
          lock: lockAt === null ? "-" : lockAt,
          label: "stale lock-ok waiver",
          text: raw.trim(),
          sig,
          why: hit
            ? "waived line is no longer inside a lock scope"
            : "no blocking/dispatch call on the waived line any more",
        });
    } else if (hit && lockAt !== null) {
      findings.push({ file: rel, line: no, lock: lockAt, label: hit, text: raw.trim(), sig });
    }

    for (const ch of line) {
      if (ch === "{") depth++;
      else if (ch === "}") depth--;
    }
    while (active.length && depth < active[active.length - 1].depth) active.pop();
    if (LOCK_DECL.test(line))
      active.push({ depth, line: no, text: raw.trim(), var: lockVarName(line) });
  });
}

function report(f) {
  if (listMode) console.error(`  ${f.file}:${f.line}  [${f.label}]`);
  else if (f.why)
    console.error(`  ${f.file}:${f.line}  [${f.label}]  ${f.why}\n      ${f.text}`);
  else
    console.error(`  ${f.file}:${f.line}  [${f.label}]  (lock at :${f.lock})\n      ${f.text}`);
}

// --- Baseline ----------------------------------------------------------------
// The dispatch rules landed on a codebase that already had 50-odd lock-held
// dispatch sites, so the guard ships with a checked-in ledger of those known
// findings (tools/lock_scope_baseline.json). A finding that is IN the ledger is
// reported as known debt and does not fail the build; a finding that is NOT
// fails it. Keys are file + rule + the flagged source line, counted, so ordinary
// line-number churn in a file never invalidates the ledger — only a new or
// changed flagged line does. Entries the code no longer produces are reported as
// stale (a warning, not a failure) so the ledger is visibly shrinkable, and
// `--update-baseline` rewrites it from the current tree.
//
// Limitation: the key is the flagged line, not the call's payload, so swapping a
// locked dispatch for a different one on the same line in the same file is not
// distinguished. Per-file counts still catch an ADDED dispatch.
const BASELINE_PATH = join(ROOT, "tools", "lock_scope_baseline.json");
const keyOf = (f) => `${f.label}\u0000${f.sig}`;

if (argv.includes("--update-baseline")) {
  const files = {};
  for (const f of findings) {
    const key = keyOf(f);
    const bucket = (files[f.file] ||= new Map());
    const entry = bucket.get(key) || { rule: f.label, text: f.sig, count: 0 };
    entry.count++;
    bucket.set(key, entry);
  }
  const out = {
    _note:
      "Known pre-existing lock-scope findings, keyed by file + rule + flagged source line " +
      "(counts allow repeats). Anything not listed here fails the build. Regenerate with " +
      "`node tools/check_lock_scope.mjs --update-baseline`; keep this ledger shrinking.",
    files: {},
  };
  for (const [file, bucket] of Object.entries(files))
    out.files[file] = [...bucket.values()].sort((a, b) =>
      a.rule === b.rule ? a.text.localeCompare(b.text) : a.rule.localeCompare(b.rule)
    );
  writeFileSync(BASELINE_PATH, JSON.stringify(out, null, 2) + "\n");
  console.log(
    `check_lock_scope: baseline written (${findings.length} finding(s) over ` +
      `${Object.keys(out.files).length} file(s)) -> tools/lock_scope_baseline.json`
  );
  process.exit(stale.length ? 1 : 0);
}

let baseline = { files: {} };
if (existsSync(BASELINE_PATH)) {
  try {
    baseline = JSON.parse(readFileSync(BASELINE_PATH, "utf8"));
  } catch (e) {
    console.error(`check_lock_scope: ${BASELINE_PATH} is not valid JSON — ${e.message}`);
    process.exit(1);
  }
}

// Match each finding against its ledger allowance (per file + rule + line).
const consumed = new Map(); // "file\u0000key" -> { allowed, used, f }
for (const [file, entries] of Object.entries(baseline.files || {}))
  for (const e of entries)
    consumed.set(`${file}\u0000${e.rule}\u0000${e.text}`, {
      file,
      rule: e.rule,
      text: e.text,
      allowed: e.count,
      used: 0,
    });

const known = [];
const fresh = [];
for (const f of findings) {
  const slot = consumed.get(`${f.file}\u0000${keyOf(f)}`);
  if (slot && slot.used < slot.allowed) {
    slot.used++;
    known.push(f);
  } else fresh.push(f);
}
const baselineStale = [...consumed.values()].filter((s) => s.used < s.allowed);

const nStaleEntries = baselineStale.reduce((n, s) => n + (s.allowed - s.used), 0);
const fail = fresh.length > 0 || stale.length > 0;

if (listMode) {
  for (const f of fresh) report(f);
  for (const f of stale) report(f);
  for (const f of known) console.error(`  ${f.file}:${f.line}  [${f.label}]  (baselined)`);
} else {
  if (fresh.length) {
    console.error(`check_lock_scope: ${fresh.length} NEW lock(s) held across a blocking call:\n`);
    for (const f of fresh) report(f);
  }
  if (stale.length) {
    console.error(
      `\ncheck_lock_scope: ${stale.length} stale waiver(s) — the line no longer needs (or no ` +
        `longer is protected by) a lock-ok, so remove the comment or restore the call:\n`
    );
    for (const f of stale) report(f);
  }
  if (baselineStale.length) {
    console.error(
      `\ncheck_lock_scope: ${nStaleEntries} baseline entr(ies) no longer match any finding — ` +
        `run --update-baseline to shrink the ledger:\n`
    );
    for (const s of baselineStale)
      console.error(`  ${s.file}  [${s.rule}]  ${s.allowed - s.used}x  (over-allowance)\n      ${s.text}`);
  }
  if (known.length)
    console.error(`\ncheck_lock_scope: ${known.length} known-baselined finding(s) not gating the build.`);
}

if (fail) {
  console.error(
    "\nIf a site is intentional, add a `// lock-ok: <reason>` comment on the line —" +
      "\nthe reason is mandatory and must say what makes that call safe." +
      "\nRemove a lock-ok once the call it justified is gone or hoisted out of the lock." +
      "\nA genuinely pre-existing site may be recorded with --update-baseline."
  );
  process.exit(1);
}
console.log(
  `check_lock_scope: clean (${files.length} files scanned, ${waivers} justified waiver(s), ` +
    `0 stale, ${known.length} baselined, ${nStaleEntries} stale baseline entr(ies))`
);
