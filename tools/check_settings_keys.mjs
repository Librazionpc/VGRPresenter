#!/usr/bin/env node
// ============================================================================
// tools/check_settings_keys.mjs — does every settings key the interface uses
// exist in the engine's descriptor table?
//
// WHY THIS EXISTS: AppSettings refuses a read/write of a key it does not know
// ("there is no setting 'outputs.defaultMediaFit'"), and SettingsService turns
// that refusal into a RED TOAST in front of the operator — while the write
// itself is simply lost. So an interface that names a key nobody declared
// looks fine in code review, builds fine, and then toasts at runtime the first
// time that control is touched, and forgets the choice on the next launch.
// That is exactly what happened with Settings · Outputs' "Fill": the QML wrote
// `outputs.defaultMediaFit` for months with no definition behind it.
//
// The key list is not restated here — it is READ from both sides:
//   * qml/**/*.qml: `SettingsService.value("…")`, `SettingsService.setValue("…")`
//     and the `SettingsService.values["…"]` map the singletons bind against;
//   * src/PresentationEngine/modules/settings/AppSettings.cpp: every descriptor
//     (`Bool`/`Text`/`Choice`/`IntRange`/`IntOf`), including the ones generated
//     per tab (`p + ".wordsPerSlide"` expanded over the kFillTabs table).
// A key in the first set and not the second is a finding.
//
// Usage:
//   node tools/check_settings_keys.mjs            # the gate
//   node tools/check_settings_keys.mjs --list     # compact file:line list
//
// Verdict (last line, greppable): SETTINGS_KEYS: PASS | SETTINGS_KEYS: FAIL
// Exit codes: 0 pass, 1 a key is undeclared, 2 the sources could not be read or
// parsed (nothing was really checked).
// ============================================================================

import { readdirSync, readFileSync, statSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const QML_DIR = join(ROOT, "qml");
const APP_SETTINGS = join(ROOT, "src/PresentationEngine/modules/settings/AppSettings.cpp");

// The descriptor helpers that carry a settings key as their FIRST argument.
// A helper that gains/loses a sibling here is the one place this list changes.
const DESCRIPTORS = ["Bool", "Text", "Choice", "IntRange", "IntOf"];
// The per-tab generator: `p + ".suffix"` descriptors are expanded over the
// prefixes of kFillTabs (scripture / table / shows today).
const TAB_TABLE = "kFillTabs";

const listMode = process.argv.includes("--list");

const fail = (message, code = 1) => {
    console.log(`SETTINGS_KEYS: FAIL ${message}`);
    process.exit(code);
};

const read = (path) => {
    try {
        return readFileSync(path, "utf8");
    } catch (e) {
        fail(`unreadable — ${path}: ${e.message} (nothing was checked)`, 2);
    }
};

function qmlFiles(dir) {
    const out = [];
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const path = join(dir, entry.name);
        if (entry.isDirectory())
            out.push(...qmlFiles(path));
        else if (entry.name.endsWith(".qml"))
            out.push(path);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The declared side: AppSettings.cpp
// ---------------------------------------------------------------------------

const source = read(APP_SETTINGS);
const declared = new Set();
let literalCount = 0;

for (const name of DESCRIPTORS) {
    // `Bool("a.b"` — a plain literal key.
    const literal = new RegExp(`d\\.push_back\\(\\s*${name}\\(\\s*"([^"]+)"`, "g");
    for (const m of source.matchAll(literal)) {
        declared.add(m[1]);
        ++literalCount;
    }
}

// `p + ".wordsPerSlide"` — generated once per tab in kFillTabs.
const suffixes = new Set();
for (const name of DESCRIPTORS) {
    const generated = new RegExp(`d\\.push_back\\(\\s*${name}\\(\\s*p\\s*\\+\\s*"(\\.[^"]+)"`, "g");
    for (const m of source.matchAll(generated))
        suffixes.add(m[1]);
}

const prefixes = new Set();
if (suffixes.size > 0) {
    const table = new RegExp(`${TAB_TABLE}\\[\\]\\s*=\\s*\\{([\\s\\S]*?)\\n\\s*\\};`).exec(source);
    if (!table)
        fail(`could not find the ${TAB_TABLE} table that generates ${suffixes.size} key(s) ` +
             `(${[...suffixes].join(", ")}) — nothing below can be trusted`, 2);
    for (const m of table[1].matchAll(/\{\s*"([^"]+)"/g))
        prefixes.add(m[1]);
    if (prefixes.size === 0)
        fail(`the ${TAB_TABLE} table parsed but named no prefix — the generated keys cannot be resolved`, 2);
}
const generated = [...prefixes].flatMap((p) => [...suffixes].map((s) => p + s));
for (const key of generated)
    declared.add(key);

// A PARSER THAT SILENTLY STOPS MATCHING looks exactly like a clean tree, so
// the generated keys are checked against the ones this table is known to make:
// if the expansion broke, every interface key would be reported as missing and
// the reason would be invisible in that noise.
for (const key of ["scripture.template", "table.wordsPerSlide", "shows.splitLongVerses"]) {
    if (!declared.has(key))
        fail(`the generated-key expansion is broken — '${key}' should come from ${TAB_TABLE} ` +
             `+ the \`p + ".…"\` descriptors`, 2);
}

if (literalCount < 40)
    fail(`only ${literalCount} literal key(s) parsed from AppSettings.cpp — the descriptor ` +
         `parsing has stopped matching the file`, 2);

// ---------------------------------------------------------------------------
// The used side: qml/**
// ---------------------------------------------------------------------------

const uses = [];   // { key, file, line }
let dynamic = 0;   // sites whose key is computed — see the note under the PASS line
for (const file of qmlFiles(QML_DIR)) {
    const text = readFileSync(file, "utf8");
    const lines = text.split(/\r?\n/);
    const patterns = [
        /SettingsService\.(?:value|setValue)\(\s*"([^"]+)"/g,      // call form
        /SettingsService\.values\[\s*"([^"]+)"\s*\]/g,            // map form (Theme.qml et al)
    ];
    // The same two shapes, with something other than a literal as the key
    // (`SettingsService.setValue(opt.settingKey, …)` from a model role). Their
    // value comes from the engine's own descriptor list, so they are counted
    // and not judged — but the count is PRINTED, because "we checked every key"
    // would be a lie if the blind spot stayed invisible.
    const computed = [
        /SettingsService\.(?:value|setValue)\(\s*([^\s"\)][^,)]*)/g,
        /SettingsService\.values\[\s*([^\s"\]][^\]]*)\]/g,
    ];
    for (let i = 0; i < lines.length; ++i) {
        for (const re of patterns) {
            re.lastIndex = 0;
            for (const m of lines[i].matchAll(re))
                uses.push({ key: m[1], file, line: i + 1 });
        }
        for (const re of computed) {
            re.lastIndex = 0;
            for (const _ of lines[i].matchAll(re))
                ++dynamic;
        }
    }
}

if (uses.length === 0)
    fail("no QML settings key was found at all — the scanning rule has stopped matching " +
         "the interface (nothing was checked)", 2);

const missing = uses.filter((u) => !declared.has(u.key));
const distinct = new Set(uses.map((u) => u.key));

// ---------------------------------------------------------------------------

if (missing.length > 0) {
    // One line per KEY (a key used five times is one defect with five sites) —
    // the file:line points at the first use, which is where to start looking.
    const byKey = new Map();
    for (const u of missing) {
        if (!byKey.has(u.key))
            byKey.set(u.key, []);
        byKey.get(u.key).push(u);
    }
    for (const [key, sites] of byKey) {
        const where = sites.map((s) => `${relative(ROOT, s.file)}:${s.line}`);
        if (listMode) {
            for (const w of where)
                console.log(w);
            continue;
        }
        console.log(`  ${key}  — NOT declared in AppSettings (${where.length} site(s): ${where.join(", ")})`);
    }
    if (!listMode) {
        console.log("");
        console.log("  A key the engine does not know refuses every read/write with");
        console.log("  \"there is no setting '<key>'\" — a red Settings toast, and the value is lost.");
        console.log("  Declare it in AppSettings.cpp's descriptor list (Bool/Text/Choice/IntRange/IntOf),");
        console.log("  or stop naming it from the interface.");
        console.log("");
    }
    console.log(`SETTINGS_KEYS: FAIL ${byKey.size} key(s) used by the interface, declared nowhere ` +
                `(${uses.length} use site(s) over ${distinct.size} key(s))`);
    process.exit(1);
}

if (listMode) {
    for (const u of uses)
        console.log(`${relative(ROOT, u.file)}:${u.line}  ${u.key}`);
}

console.log(`SETTINGS_KEYS: PASS ${distinct.size} key(s) used by the interface, all declared ` +
            `(${declared.size} descriptor(s): ${literalCount} literal` +
            `${generated.length ? ` + ${generated.length} generated over ${prefixes.size} tab(s)` : ""})`);
if (dynamic > 0) {
    console.log(`SETTINGS_KEYS: ${dynamic} call site(s) pass a COMPUTED key (a model role, a
  local variable) and are NOT judged here — their key comes from the engine's own
  descriptor list, which is exactly where a literal key has to be declared anyway.`);
}
process.exit(0);
