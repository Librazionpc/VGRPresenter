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
// while a lock is in scope — on THAT line, or through a helper the file itself
// defines whose body reaches one of those calls (see the helper section below:
// the innocent-looking line and the blocking work are usually one call apart).
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
//   node tools/check_lock_scope.mjs --json            # findings as JSON, always exit 0
//   node tools/check_lock_scope.mjs --self-test       # prove the rules still fire (see below)
//   node tools/check_lock_scope.mjs --update-baseline # rewrite the known-findings ledger
//
// Looked at only from the outside, a guard whose rules have quietly stopped
// firing is indistinguishable from a clean tree. `--self-test` is the answer:
// it runs the scan over tools/lock_scope_fixtures — text shapes that are never
// compiled — and compares the result with expectations.json, so the two rules
// that report nothing on a healthy tree (the helper rule, the self-deadlock
// rule) are proven live on every build.
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
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

// This script's own path: the self-test re-runs the guard as a child process, and
// an ES module has no __filename.
const SELF = fileURLToPath(import.meta.url);

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

// The mutex EXPRESSION a lock declaration binds, and whether that mutex is
// re-entrant — the two facts the self-deadlock rule needs (see it below). A
// declaration may name several mutexes (`std::scoped_lock lk(a, b)`), so the
// names come back as a list; the re-entrancy flag is true only for the recursive
// types, whose whole purpose is a second acquisition by the same thread.
function lockTarget(line) {
  const templated = /\bstd::(lock_guard|unique_lock|scoped_lock|shared_lock)\s*<([^>]*)>\s*\w+\s*\(\s*([^;]*?)\s*\)/.exec(line);
  if (templated) {
    return {
      names: templated[3].split(",").map((s) => s.trim()).filter(Boolean),
      recursive: /recursive_mutex|recursive_timed_mutex/.test(templated[2]),
    };
  }
  const deduced = /\bstd::(lock_guard|unique_lock|scoped_lock)\s+\w+\s*\(\s*([^;]*?)\s*\)/.exec(line);
  if (deduced)
    return {
      names: deduced[2].split(",").map((s) => s.trim()).filter(Boolean),
      recursive: false,
    };
  const explicit = /\b(\w+)\s*\.\s*lock\s*\(\s*\)/.exec(line);
  return explicit ? { names: [explicit[1]], recursive: false } : null;
}

// Every mutex name a chunk of code LOCKS (a declaration or an explicit call).
function lockedNames(code) {
  const out = new Set();
  for (const line of code.split("\n")) {
    const t = lockTarget(line);
    if (t) for (const n of t.names) out.add(n);
  }
  return out;
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


// ---- Helpers that wrap a blocking call -------------------------------------
// The guard's line-local rules cannot see the shape that started this file's
// whole story one level down: Import() looks innocent, and the PAL write — or
// the dispatch — sits inside the private member helper it calls. That is how
// BibleEngine held mutex_ across a multi-megabyte store write (WriteAtomic
// inside SaveStoreLocked) with this very guard reporting clean: the blocked line
// WAS the lock, and the blocking call was a name on it.
//
// So every top-level definition in the file is parsed (brace matching, the same
// heuristic the GUI-thread guard uses) and asked what its body can reach —
// directly, then transitively through other definitions in the same file. A call
// to such a helper while a lock is in scope is reported with the helper's name,
// so the reader sees where the blocking work actually is.
const HEAD_RE =
  /(?:^|[\s\*&>])(?:([A-Za-z_]\w*)\s*::\s*)?(~?[A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?$/;
const CTRL_DECL = /^(if|for|while|switch|return|catch|do|else|sizeof|new|delete|static_assert)$/;
// `namespace a {`, `namespace a::b {`, the anonymous `namespace {` and the
// `extern "C" {` wrapper are TRANSPARENT scopes: a definition inside one is
// still file scope as far as this scan is concerned, so their braces are not
// counted and what they contain is parsed exactly like a definition at the top
// of the file. Both halves matter. Without it every file that wraps its contents
// in a namespace reads as EMPTY — no definitions, so the helper rule and the
// self-deadlock rule (which needs to know whose method a locked line is inside)
// silently judge nothing at all. That is not hypothetical: it is how the
// fixture self-test below caught these rules going quiet, and how a whole
// namespaced translation unit could hold a lock across a device call with this
// guard reporting clean. One head can open several of them (`namespace a {
// namespace b {` on one line), so every leading group is stripped.
// A group ALREADY opened in the same head, and the declarator this brace opens:
// `namespace a { namespace b` is one head whose tail (the part after the last
// boundary) is the second declarator.
const TRANSPARENT_OPEN =
  /^(?:inline\s+)?namespace(?:\s+[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)?\s*\{|^extern\s*"C"\s*\{/;
const TRANSPARENT_DECL =
  /^(?:inline\s+)?namespace(?:\s+[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)?$|^extern\s*"C"$/;
function isTransparentScopeHead(head) {
  // Everything since the last statement boundary — and a preprocessor line
  // (`#include <mutex>`) ends with neither `;` nor `}`, so the head of the first
  // brace in a file routinely has one glued to its front. Only the part after
  // the last boundary can be the declarator being opened. The head arrives RAW
  // (newlines and all): collapsing its whitespace first would erase the very
  // boundary this cut is looking for.
  const cut = Math.max(head.lastIndexOf("\n"), head.lastIndexOf(";"), head.lastIndexOf("}"));
  let rest = head.slice(cut + 1).replace(/\s+/g, " ").trim();
  for (;;) {
    const m = TRANSPARENT_OPEN.exec(rest);
    if (!m) break;
    rest = rest.slice(m[0].length).trim();
  }
  return TRANSPARENT_DECL.test(rest);
}
function definitionBodies(code) {
  const out = [];
  const n = code.length;
  let depth = 0;
  let stmtStart = 0;   // start of the current top-level statement
  const lineOf = (idx) => {
    let l = 1;
    for (let i = 0; i < idx; i++) if (code[i] === "\n") l++;
    return l;
  };
  for (let i = 0; i < n; i++) {
    const c = code[i];
    if (c === "{") {
      if (depth === 0 && isTransparentScopeHead(code.slice(stmtStart, i))) {
        // Transparent: the brace is not counted, so what follows is parsed at
        // file scope. Its matching close falls through the `depth > 0` guard.
        stmtStart = i + 1;
        continue;
      }
      if (depth === 0) {
        const head = code.slice(stmtStart, i).trim();
        const m = HEAD_RE.exec(head.replace(/\s+/g, " ").trim());
        const lastChar = head[head.length - 1] || "";
        const isLambda = /[\[\]]\s*\([^)]*\)\s*$/.test(head) || head.includes("](");
        if (m && !CTRL_DECL.test(m[2]) && lastChar !== "=" && !isLambda) {
          let d = 1;
          let j = i + 1;
          while (j < n && d > 0) {
            if (code[j] === "{") d++;
            else if (code[j] === "}") d--;
            j++;
          }
          const body = code.slice(i + 1, j - 1);
          out.push({
            cls: m[1] || null,
            name: m[2],
            // The parameter list, kept because it answers a question the body
            // cannot: whether this helper is HANDED the caller's lock.
            params: m[3] || "",
            startLine: lineOf(stmtStart),
            endLine: lineOf(j),
            body,
            // Which mutexes this body LOCKS (see the self-deadlock rule): the
            // one thing a caller has to know about a callee that a body-parse
            // can answer.
            locks: lockedNames(body),
          });
          i = j - 1;
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

// ---- The whole-tree definition index (helpers across translation units) ----
// The helper rule below started FILE-LOCAL, and that was its stated limit: the
// shape that started this guard — `BibleEngine::Import` -> `SaveStoreLocked` ->
// `WriteAtomic` — was caught only because those two definitions live in one
// .cpp. A helper in ANOTHER translation unit of the same class, or a free helper
// in a sibling file, was invisible, so a locked line calling one read clean.
//
// Every scanned file is parsed up front instead, and the closure over "what can
// this body reach" runs on the whole graph:
//   * a call resolves to the ONE definition of that name (its class member first
//     when the caller is a member of the same class);
//   * a name defined in MORE THAN ONE place is AMBIGUOUS and stays UNRESOLVED —
//     guessing which one a call binds to is how a static analysis grows false
//     positives, so those calls are counted and reported as a stated limit
//     (`N ambiguous helper call(s) left unresolved`) rather than guessed at.
// This is still a name-based body-parse: it cannot see overload resolution,
// virtual dispatch or templates, which the report says out loud.
const allDefs = [];
for (const file of files) {
  const text = readFileSync(file, "utf8");
  const stripped = text.split(/\r?\n/).map((l) => strip(l)).join("\n");
  for (const d of definitionBodies(stripped)) {
    d.file = relative(ROOT, file).split(sep).join("/");
    allDefs.push(d);
  }
}
const defsByName = new Map();
for (const d of allDefs) {
  if (!defsByName.has(d.name)) defsByName.set(d.name, []);
  defsByName.get(d.name).push(d);
}
const uniqueDefsByName = new Map();
const ambiguousNames = new Set();
for (const [name, list] of defsByName) {
  if (list.length === 1) uniqueDefsByName.set(name, list[0]);
  else ambiguousNames.add(name);
}
// Every call in a chunk of code, with HOW it reaches the name. That distinction
// is the difference between a rule and a guess: a call on THIS object (`Foo()`,
// `this->Foo()`) can be resolved by name, because the caller's own class is
// known; a call on ANOTHER object (`imp->Foo()`, `x.Foo()`) cannot, because the
// scan has no idea what type that object is. Resolving those by name anyway is
// how a guard invents findings — `imp->SupportedExtensions()` is the importer's
// method, not ImportManager's — so they are counted as unresolved instead.
const CALL_RE = /(?:(\w+|this)\s*(?:->|\.)\s*)?([A-Za-z_]\w*)\s*\(/g;
function calleeCalls(code) {
  const out = [];
  CALL_RE.lastIndex = 0;
  let m;
  while ((m = CALL_RE.exec(code)))
    out.push({ name: m[2], onThis: !m[1] || m[1] === "this", at: m.index });
  return out;
}

// The body of a lambda written on the same line (`worker_ =
// std::thread([this]() { Loop(); });`). A call inside one is not executed by
// this line, so it is not this line's blocking work: the thread it is handed to
// does not hold the caller's lock. Skipping these is what stops "starting a
// worker" from reading as "calling the worker's body under the lock".
function lambdaBodyRanges(code) {
  const out = [];
  const re = /\[[^\]]*\]\s*(?:\([^)]*\))?\s*(?:mutable\s*)?(?:->[^{;]*)?\{/g;
  let m;
  while ((m = re.exec(code))) {
    let i = m.index + m[0].length;
    let d = 1;
    while (i < code.length && d > 0) {
      if (code[i] === "{") d++;
      else if (code[i] === "}") d--;
      i++;
    }
    out.push([m.index + m[0].length, i]);
  }
  return out;
}
const insideLambda = (ranges, at) => ranges.some(([a, b]) => at >= a && at < b);
for (const d of allDefs) d.labels = new Set();
for (const d of allDefs)
  for (const [re, l] of BLOCKING) if (re.test(d.body)) d.labels.add(l);
// What a call binds to, given how it reaches the name. The ONE resolver: the
// tree-wide label closure below and the per-line scan both come through here, so
// the two can never disagree about what a call means.
//   * a call on this object, and a call to a definition in the caller's own file
//     with exactly one definition of that name, are resolved;
//   * a call on another object never is (no type information);
//   * a name with several definitions never is (which one a call binds to is not
//     answerable from the text — see the ambiguous fixture);
//   * a lone definition that is a member of a DIFFERENT class never is: a member
//     of another class is another object's method, whatever the name looks like.
function resolveCalleeAt(name, onThis, fromDef) {
  if (!onThis) return null;
  // Outside any definition this scan could parse — an inline member body inside
  // a class definition, say — there is no caller, so no file and no class to
  // match against: a tree-wide name match would be a guess (`Ensure()` there is
  // the local sampler's, and `ThumbnailCache::Ensure` is another object's).
  if (!fromDef) return null;
  const own = defsByName.get(name) || [];
  if (fromDef) {
    const here = own.filter((d) => d.file === fromDef.file);
    if (here.length === 1) return here[0];
    if (fromDef.cls) {
      const member = here.find((d) => d.cls === fromDef.cls);
      if (member) return member;
    }
  }
  if (ambiguousNames.has(name)) return null;
  const only = uniqueDefsByName.get(name) || null;
  if (only && fromDef && fromDef.cls && only.cls && only.cls !== fromDef.cls) return null;
  return only;
}

// A helper that TAKES a lock — `Drain(std::unique_lock<std::recursive_mutex>& lk,
// Execution&)` — is handed the caller's lock on purpose: the unique_lock exists
// so the callee can RELEASE it around the work that must not run under it (this
// codebase's pattern, spelled out at those call sites). Whether it does is not
// visible from the caller's line, but the parameter is about the only thing that
// can make a synchronous call on a held lock legitimate, so such calls are not
// reported as held-lock findings. They are counted, and the count is printed.
function takesLockParam(d) {
  if (!d || !d.params) return false;
  return (
    /\b(unique_lock|scoped_lock|shared_lock|lock_guard)\s*</.test(d.params) ||
    /\b\w*[Mm]utex\s*[&*]/.test(d.params)
  );
}

// Transitive closure over the whole tree: a body inherits whatever a body it
// calls can reach.
for (const d of allDefs) d.lambdaRanges = lambdaBodyRanges(d.body);
for (let round = 0; round < 8; round++) {
  let grew = false;
  for (const d of allDefs) {
    for (const call of calleeCalls(d.body)) {
      if (insideLambda(d.lambdaRanges, call.at)) continue;
      const t = resolveCalleeAt(call.name, call.onThis, d);
      if (!t || t === d) continue;
      for (const l of t.labels) if (!d.labels.has(l)) { d.labels.add(l); grew = true; }
    }
  }
  if (!grew) break;
}
const fileDefs = new Map();   // file -> its definitions, in line order
for (const d of allDefs) {
  if (!fileDefs.has(d.file)) fileDefs.set(d.file, []);
  fileDefs.get(d.file).push(d);
}
const unresolved = new Set();   // file:line:name of helper calls left ambiguous

const findings = [];
const stale = [];
let waivers = 0;
// Lock declarations this scan can see but cannot judge: they sit inside a
// `#define`, so whether they are held — and for how long — depends on where the
// macro is expanded. Counted (and stated in the summary) instead of either
// guessed at or silently ignored.
let macroLocks = 0;
// Calls that hand the caller's lock to a helper (`Drain(lock, exec)`), where the
// point of the parameter is to release it. Counted, not judged — see
// takesLockParam.
let handedLocks = 0;
for (const file of files) {
  // POSIX separators in every reported/ledger path, so the baseline is not
  // Windows-only.
  const rel = relative(ROOT, file).split(sep).join("/");
  const text = readFileSync(file, "utf8");
  const lines = text.split(/\r?\n/);
  let depth = 0;
  const active = []; // { depth, line, text }

  // This file's own definitions (labelled by the TREE-wide closure above, so a
  // same-file helper and a cross-file one are judged the same way) and a
  // line -> definition index, which the self-deadlock rule below needs to know
  // WHOSE method a locked line is inside.
  const ownDefs = fileDefs.get(relative(ROOT, file).split(sep).join("/")) || [];
  const defAtLine = new Array(lines.length + 2).fill(null);
  for (const d of ownDefs)
    for (let l = d.startLine; l <= Math.min(d.endLine, lines.length); l++) defAtLine[l] = d;
  // Names of local std::function objects in this file — calling one is a
  // callback invocation, same class of hazard as a stored member.
  const fnNames = localFunctionNames(text);
  // Inside a `#define … \` continuation. A directive is TEXT: a lock_guard in
  // one is not a scope the compiler ever opens on that line, and treating it as
  // one puts every later line in the file "under a lock" — which is how a
  // macro-defined lock turned `ProjectLibrary.cpp` into one 200-line locked
  // scope with this guard's blessing.
  let inDirective = false;

  lines.forEach((raw, idx) => {
    const line = strip(raw);
    const no = idx + 1;
    const isDirective = inDirective || /^\s*#/.test(raw);
    inDirective = isDirective && /\\\s*$/.test(raw);
    // `lock-ok: <reason>` waives the line; the reason is required, so a bare
    // `lock-ok:` (or one followed only by the closing comment) is a finding.
    const m = /lock-ok:(.*)$/.exec(raw);
    const reason = m ? m[1].replace(/\*\/\s*$/, "").trim() : null;
    const hasWaiver = reason !== null;

    // Resolve what this line would trip, independent of any waiver, so a waiver
    // can be judged against exactly the signal a finding would use.
    // A lock declaration line is never itself a held-across-call site, and a
    // preprocessor directive is not code at all.
    let hit = null;
    if (isDirective) {
      if (LOCK_DECL.test(line)) macroLocks++;   // real, but not visible here
      return;
    }
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
      const cur = defAtLine[no];
      // The head of a definition is not a call site. `Result<Project>
      // ProjectLibrary::Get(std::string_view) const {` matches `Get(` and then
      // resolves to the very definition it is opening, so the method is reported
      // as calling itself — and, worse, the method's own name resolved to a body
      // that reaches the PAL was reported as that method's blocking helper. Only
      // the text after the opening brace is scanned for calls.
      let lineText = line;
      if (cur && no === cur.startLine) {
        const brace = line.indexOf("{");
        lineText = brace >= 0 ? line.slice(brace + 1) : "";
      }
      const lambdas = lambdaBodyRanges(lineText);
      const calls = lineText ? calleeCalls(lineText) : [];
      if (cb) hit = cb;
      else {
        // A call to a helper whose body reaches a primitive — this file's own, or
        // another translation unit's (see the tree index). The call is blocking
        // even though nothing on THIS line is.
        for (const call of calls) {
          if (insideLambda(lambdas, call.at)) continue;
          const t = resolveCalleeAt(call.name, call.onThis, cur);
          if (!t) {
            // Not resolved, and worth SAYING so: either the name has several
            // definitions in scope (which one a call binds to is unanswerable
            // from the text) or the call is on an object this scan cannot type —
            // and in both cases a definition of that name reaches a blocking
            // call, so silence here would read as "checked, clean".
            const own = defsByName.get(call.name) || [];
            if (own.some((d) => d.labels.size > 0))
              unresolved.add(
                `${rel}:${no}:${call.name}` +
                  (!cur
                    ? " (no enclosing definition parsed)"
                    : call.onThis
                      ? " (several definitions)"
                      : " (receiver unknown)")
              );
            continue;
          }
          if (takesLockParam(t)) {
            handedLocks++;
            continue;
          }
          if (t.labels.size === 0) continue;
          hit = t.file === rel
            ? "helper '" + call.name + "' wraps " + [...t.labels].join("/")
            : "helper '" + call.name + "' (" + t.file + ":" + t.startLine + ") wraps " +
              [...t.labels].join("/");
          break;
        }
        for (const [re, label] of BLOCKING) {
          if (hit) break;
          if (!re.test(lineText)) continue;
          // The release-and-wait idiom is not a lock held across a wait.
          if (label === "condition-variable wait" && cvWaitReleasesOnlyLock(lineText, active))
            continue;
          hit = label;
          break;
        }
      }
      // ---- The same-mutex re-lock (self-deadlock) -------------------------
      // A helper this line calls that locks the SAME mutex object, while that
      // mutex is held right here, deadlocks: the thread waits forever on a lock
      // it already holds. This is the ResourceManager shape one step earlier than
      // the dispatch rule catches it (Sample() held mutex_ across a publish whose
      // subscriber came back into the manager).
      //
      // Four things have to hold for a finding, and the first attempt at this
      // rule got the last two wrong (5 findings, all false positives, so the rule
      // was left out of the guard until they could be stated):
      //   * the call is on THIS object — `Refresh()` or `this->Refresh()`, never
      //     `other.Refresh()`, which is another instance's mutex_ and no hazard;
      //   * the callee is a member of the SAME class, so `mutex_` really is the
      //     same member and not another class's equally-named one;
      //   * the mutex is non-recursive (re-locking a recursive_mutex is legal by
      //     construction);
      //   * and the callee really does lock a mutex this line is holding.
      if (!hit && cur) {
        const top = active.length ? active[active.length - 1] : null;
        if (top && top.mutexes && !top.mutexes.recursive) {
          for (const call of calls) {
            if (insideLambda(lambdas, call.at)) continue;
            if (!call.onThis) continue;
            const t = resolveCalleeAt(call.name, call.onThis, cur);
            if (!t || !t.cls || !cur.cls || t.cls !== cur.cls) continue;
            const same = [...t.locks].filter((n) => top.mutexes.names.includes(n));
            if (same.length) {
              hit =
                "self-deadlock: '" + call.name + "' re-locks '" + same[0] +
                "' held at :" + top.line;
              break;
            }
          }
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
    const lm = LOCK_DECL.exec(line);
    if (lm) {
      // The lock's scope is the brace depth AT THE DECLARATION, not at the end of
      // the line. On a one-liner — `std::vector<X> F::Folders() const {
      // std::lock_guard<std::mutex> lock(mutex_); return folders_; }` — the scope
      // opens and closes between the two, and recording the end-of-line depth (0)
      // left the lock ACTIVE for the rest of the file: every later line read as
      // "under a lock", which is exactly how a guard reports hundreds of findings
      // that are all the same phantom. A lock whose block has already closed by
      // the end of the line is not held on the next one.
      let after = 0;
      for (const ch of line.slice(lm.index))
        if (ch === "{") after++;
        else if (ch === "}") after--;
      const atLock = depth - after;
      if (atLock <= depth)
        active.push({
          depth: atLock,
          line: no,
          text: raw.trim(),
          var: lockVarName(line),
          mutexes: lockTarget(line),
        });
    }
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
// The ledger (tools/lock_scope_baseline.json) exists so a guard that suddenly
// sees MORE than it used to can record the sites it just revealed without
// gating the build on them. That is a staging tool, not a permanent allowance:
// it shipped with 71 entries when the cross-TU helper rule started resolving
// namespaced translation units, and every one of those was then repaired — the
// file is EMPTY today. A finding that is IN the ledger is reported as known debt
// and does not fail the build; a finding that is NOT fails it. Keys are file +
// rule + the flagged source line, counted, so ordinary line-number churn in a
// file never invalidates the ledger — only a new or changed flagged line does.
// Entries the code no longer produces are reported as stale (a warning, not a
// failure) so the ledger is visibly shrinkable, and `--update-baseline` rewrites
// it from the current tree. Adding an entry is a last resort: prefer the fix
// (hoist the call, or split the helper) and, where a call genuinely cannot move,
// a `lock-ok:` waiver carrying its reason.
//
// Limitation: the key is the flagged line, not the call's payload, so swapping a
// locked dispatch for a different one on the same line in the same file is not
// distinguished. Per-file counts still catch an ADDED dispatch.
const BASELINE_PATH = join(ROOT, "tools", "lock_scope_baseline.json");
const keyOf = (f) => `${f.label}\u0000${f.sig}`;

// Machine-readable output, for the self-test below and for anything else that
// wants the findings without parsing the human report.
if (argv.includes("--json")) {
  console.log(
    JSON.stringify(
      { findings, stale, unresolved: [...unresolved] },
      null,
      2
    )
  );
  process.exit(0);
}

// ---- Self-test ---------------------------------------------------------------
// Both of the guard's non-obvious rules (a locked line calling a helper that
// reaches a blocking call — in this file or another translation unit — and a
// locked method calling something that re-locks the SAME mutex) report NOTHING
// on a healthy tree. A clean run therefore cannot tell a rule that works from
// one that has rotted: a renamed regex, a dropped branch, a mis-anchored brace
// parse all read as "clean".
//
// So the rule is checked against shapes whose verdict is known. `--self-test`
// re-runs the scan over tools/lock_scope_fixtures (text files, never compiled)
// and compares what it reports against the expectations.json sitting beside
// them: every shape that must be flagged, every shape that must stay quiet, and
// the count of calls the scan refuses to resolve. The build runs it next to the
// scan itself (`lock_scope_check`), so a rule cannot rot unnoticed.
if (argv.includes("--self-test")) {
  const FIX_DIR = join(ROOT, "tools", "lock_scope_fixtures");
  const FIX_PREFIX = "tools/lock_scope_fixtures/";
  const expPath = join(FIX_DIR, "expectations.json");
  const relFixPath = relative(ROOT, expPath).split(sep).join("/");
  if (!existsSync(expPath)) {
    console.error(`lock_scope_selftest: FAIL — ${relFixPath} is missing (the guard's rules are unchecked)`);
    process.exit(1);
  }
  let exp;
  try {
    exp = JSON.parse(readFileSync(expPath, "utf8"));
  } catch (e) {
    console.error(`lock_scope_selftest: FAIL — ${relFixPath} is not valid JSON: ${e.message}`);
    process.exit(1);
  }
  let got;
  try {
    const raw = execFileSync(
      process.execPath,
      [SELF, "--json", "tools/lock_scope_fixtures"],
      { cwd: ROOT, encoding: "utf8" }
    );
    got = JSON.parse(raw);
  } catch (e) {
    console.error(`lock_scope_selftest: FAIL — the fixture scan did not run: ${e.message}`);
    process.exit(1);
  }
  const bare = (f) => (f.startsWith(FIX_PREFIX) ? f.slice(FIX_PREFIX.length) : f);
  const found = (got.findings || []).map((f) => ({ file: bare(f.file), label: f.label, line: f.line }));
  const problems = [];
  const claimed = new Set();
  for (const want of exp.flagged || []) {
    const m = found.find((f) => f.file === want.file && f.label.includes(want.labelContains));
    if (!m) {
      problems.push(
        `${want.file}: expected a finding matching \`${want.labelContains}\` — got ` +
          (found.filter((f) => f.file === want.file).map((f) => `[${f.label}]`).join(" ") || "nothing")
      );
      continue;
    }
    claimed.add(m);
  }
  const quiet = new Set((exp.quiet || []).map((q) => (typeof q === "string" ? q : q.file)));
  for (const f of found) {
    if (claimed.has(f)) continue;
    if (quiet.has(f.file)) {
      problems.push(`${f.file}:${f.line}: reported [${f.label}] but this shape must stay quiet`);
      continue;
    }
    problems.push(`${f.file}:${f.line}: reported [${f.label}] — not named in expectations.json`);
  }
  const unresolvedN = (got.unresolved || []).length;
  if (exp.unresolvedMin !== undefined && unresolvedN < exp.unresolvedMin)
    problems.push(
      `expected at least ${exp.unresolvedMin} unresolved helper call(s) (the ambiguous fixture) — got ${unresolvedN}`
    );
  const leftover = [...quiet].filter((q) => !existsSync(join(FIX_DIR, q)));
  for (const l of leftover) problems.push(`${l}: named in expectations.json but not present in the fixture directory`);

  if (problems.length) {
    console.error(`lock_scope_selftest: FAIL — ${problems.length} expectation(s) not met:`);
    for (const p of problems) console.error(`  ${p}`);
    console.error(
      "  The fixtures are the guard's only proof that its rules still fire; fix the rule " +
        "(or, if the shape is deliberately no longer caught, expectations.json and the fixture's " +
        "// expect: comment) rather than deleting the fixture."
    );
    process.exit(1);
  }
  console.log(
    `lock_scope_selftest: PASS flagged=${(exp.flagged || []).length} quiet=${quiet.size} ` +
      `unresolved=${unresolvedN}`
  );
  process.exit(0);
}

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
  for (const u of unresolved) console.error(`  ${u}  (not judged)`);
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
// The two stated limits, printed only when they are non-zero so a clean run stays
// quiet — but said out loud whenever they apply, because "0 findings" and "0
// findings because 40 calls could not be resolved" are not the same result.
if (unresolved.size)
  console.error(
    `check_lock_scope: ${unresolved.size} helper call(s) NOT judged (a name-based scan\n` +
      `  cannot resolve them: several definitions in scope, or a receiver whose type is\n` +
      `  unknown — \`imp->Open()\` is not this object's Open). Run with --list to see them.`
  );
if (macroLocks)
  console.error(
    `check_lock_scope: ${macroLocks} lock declaration(s) inside a #define — whether they\n` +
      `  are held depends on where the macro expands, which this scan cannot see.`
  );
if (handedLocks)
  console.error(
    `check_lock_scope: ${handedLocks} call(s) handed this line's lock (a lock parameter:\n` +
      `  the callee releases it around the work, which is the point of the parameter).`
  );
