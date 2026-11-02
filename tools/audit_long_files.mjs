// Long-file audit measurement (see docs/long_file_audit.md).
//
// Prints, per file, its line count and its LONGEST functions measured by brace
// matching — the QML files carry `function name() {` blocks nested inside
// components, so a "next definition starts here" estimate overstates them badly
// (one QML handler measured 1905 lines that way and 27 by braces).
//
// Usage: node tools/audit_long_files.mjs [file ...]
import fs from "node:fs";

const DEFAULT_FILES = [
  "qml/Main.qml",
  "qml/services/LiveOutputService.cpp",
  "qml/services/LiveOutputReading.cpp",
  "qml/services/LiveOutputInputs.cpp",
  "qml/services/LiveOutputMedia.cpp",
  "qml/services/LiveOutputNdi.cpp",
  "qml/services/SlidePreviewMap.cpp",
  "qml/screens/edit/EditScreen.qml",
  "qml/components/ReferencePane.qml",
  "qml/screens/settings/AudioVideoScreen.qml",
  "qml/screens/settings/RoutingScreen.qml",
  "qml/services/EngineBridge.cpp",
  "src/PresentationEngine/modules/presentation/SceneBuilder.cpp",
  "src/PresentationEngine/modules/library/TheTableLibrary.cpp",
  "qml/screens/main/VGRPresenterMainScreen.qml",
  "qml/components/MonitorWall.qml",
  "qml/services/ShowService.cpp",
];

// Strip comments and string bodies so braces inside them cannot move the depth.
function scrub(line) {
  return line
    .replace(/\/\/.*$/, "")
    .replace(/"(?:[^"\\]|\\.)*"/g, '""')
    .replace(/'(?:[^'\\]|\\.)*'/g, "''");
}

// Every `function name(...)` in the file, with the line span its own braces own.
export function functionSpans(source) {
  const lines = source.split(/\r?\n/);
  const out = [];
  for (let i = 0; i < lines.length; i++) {
    if (!/^\s*function\s+[A-Za-z_0-9]+\s*\(/.test(lines[i])) continue;
    let depth = 0;
    let started = false;
    let j = i;
    for (; j < lines.length; j++) {
      for (const ch of scrub(lines[j])) {
        if (ch === "{") {
          depth++;
          started = true;
        } else if (ch === "}") {
          depth--;
        }
      }
      if (started && depth <= 0) break;
    }
    out.push({ lines: j - i + 1, at: i + 1, name: lines[i].trim().slice(0, 64) });
  }
  return out;
}

// Top-level C++ definitions (`Name::member(` at column 0) — the span to the next
// one IS this function's body, because the project defines them at column 0.
export function cppSpans(source) {
  const lines = source.split(/\r?\n/);
  const starts = [];
  lines.forEach((l, i) => {
    if (/^[A-Za-z_].*::[A-Za-z_~]+\(/.test(l)) starts.push(i);
  });
  return starts.map((s, k) => ({
    lines: (k + 1 < starts.length ? starts[k + 1] : lines.length) - s,
    at: s + 1,
    name: lines[s].trim().slice(0, 64),
  }));
}

// ---- the repo-wide inventory -----------------------------------------------
// Every source file over the threshold, longest first, with its function count
// and its worst body — the table docs/long_file_audit.md is built from.
const SKIP_DIRS = new Set(["build", "build-agent", "dev-profile", ".git", "node_modules", ".cache"]);

function walk(dir, out = []) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (SKIP_DIRS.has(entry.name)) continue;
    const p = `${dir}/${entry.name}`;
    if (entry.isDirectory()) walk(p, out);
    else if (/\.(cpp|hpp|h|qml)$/.test(entry.name)) out.push(p);
  }
  return out;
}

function inventory(threshold = 1200) {
  const rows = [];
  for (const f of [...walk("qml"), ...walk("src")]) {
    const source = fs.readFileSync(f, "utf8");
    const lines = source.split(/\r?\n/).length;
    if (lines < threshold) continue;
    // A header's functions are mostly one-line definitions; measure only bodies.
    const spans = (f.endsWith(".cpp") ? cppSpans(source) : functionSpans(source))
      .sort((a, b) => b.lines - a.lines);
    rows.push({ f, lines, count: spans.length, top: spans[0] || { lines: 0, at: 0, name: "" } });
  }
  return rows.sort((a, b) => b.lines - a.lines);
}

// Print only when run as a script: a caller may import the helpers above.
const asScript = process.argv[1] && import.meta.url.endsWith(
  process.argv[1].replaceAll("\\", "/").split("/").pop());
if (asScript) {
  const args = process.argv.slice(2);
  if (args[0] === "--all") {
    const rows = inventory(Number(args[1]) || 1200);
    console.log("  lines | funcs | worst | file");
    for (const r of rows)
      console.log(`${String(r.lines).padStart(7)} | ${String(r.count).padStart(5)} | `
        + `${String(r.top.lines).padStart(5)} | ${r.f}  ::  ${r.top.name.slice(0, 46)}`);
    console.log(`\n${rows.length} file(s) over ${Number(args[1]) || 1200} lines `
      + `(${rows.reduce((n, r) => n + r.lines, 0)} lines total)`);
  } else {
    const files = args.length ? args : DEFAULT_FILES;
    for (const f of files) {
      const source = fs.readFileSync(f, "utf8");
      const total = source.split(/\r?\n/).length;
      const spans = (f.endsWith(".cpp") ? cppSpans(source) : functionSpans(source))
        .sort((a, b) => b.lines - a.lines);
      console.log(`=== ${f}  ${total} lines, ${spans.length} function(s) measured ===`);
      for (const s of spans.slice(0, 6))
        console.log(`${String(s.lines).padStart(5)} lines  @${s.at}  ${s.name}`);
    }
  }
}
