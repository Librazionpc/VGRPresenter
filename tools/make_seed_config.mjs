#!/usr/bin/env node
// ============================================================================
// tools/make_seed_config.mjs — bundle THIS machine's configuration into a build.
//
// Run by tools/make_installer.sh before Inno Setup compiles. It produces two
// different things from the operator's live data, and the difference matters:
//
//   1. dist/VGRPresenter/assets/settings.shipped.json
//      The app's SHIPPED BASELINE. Every install replaces it; the app adopts a
//      value from it only for a setting whose store has NO value (see
//      AppSettings::Load). So an update arrives configured — new settings and
//      the operator's choices for anything nobody changed — while every key an
//      install already chose keeps its own value. Nothing is reset, nobody
//      reconfigures, and the installer never deletes settings.json.
//
//   2. installer/seed-data/local/settings.json
//      The same baseline as a store file, so a FRESH install starts with a
//      settings.json that already reads as configured (the app would fill the
//      same values from the baseline; this just makes the file self-describing).
//
//   3. installer/seed-data/roaming/enginedata/kernel.json
//      The engine's saved CONFIGURATION collections (styles, outputs, the
//      production graph, profiles, template roster) — the operator's look and
//      signal path — with the session collections (history, recents, favourites,
//      recovery, last session) left behind. Copied only on a fresh install
//      (installer.iss: onlyifdoesntexist); an existing install keeps its own.
//
// WHAT IS STRIPPED, ALWAYS: this machine's absolute paths. A bundled settings
// file that names C:\Users\<somebody>\... is worse than an empty one — the other
// PC shows an error about a file it never had. The rule is deliberately literal:
// a key known to hold machine state, or a string value that contains a drive
// letter path (the rosters are JSON *text*, so a path can hide inside one).
//
// The design catalogs are NOT produced here — they are generated from the
// engine's own definitions by `bps_cli --dump-catalogs` (see make_installer.sh),
// so a shipped template can never carry one machine's fonts again.
//
// Usage:
//   node tools/make_seed_config.mjs
//
// Overrides (used by the tests and by a relocated checkout):
//   BPS_SETTINGS_FILE   default %LOCALAPPDATA%\bps\settings.json
//   BPS_KERNEL_FILE     default %APPDATA%\VGR\VGRPresenter\enginedata\kernel.json
//   BPS_SEED_DIR        default <repo>/installer/seed-data
//   BPS_DIST_DIR        default <repo>/dist/VGRPresenter
// ============================================================================

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SETTINGS_FILE = process.env.BPS_SETTINGS_FILE
    || path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'), 'bps', 'settings.json');
const KERNEL_FILE = process.env.BPS_KERNEL_FILE
    || path.join(process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming'), 'VGR', 'VGRPresenter', 'enginedata', 'kernel.json');
const SEED_DIR = process.env.BPS_SEED_DIR || path.join(ROOT, 'installer', 'seed-data');
const DIST_DIR = process.env.BPS_DIST_DIR || path.join(ROOT, 'dist', 'VGRPresenter');

// Settings keys that describe THIS machine (a file it has open, a project it
// last touched) rather than a way the app should behave.
const MACHINE_KEYS = new Set([
    'session.lastShowPath',
    'session.lastProject',
    'session.quickSearchPicks',
]);

// The engine collections that are CONFIGURATION (a fresh install should look and
// route like the operator's) — everything else in kernel.json is session state.
const KERNEL_CONFIG_COLLECTIONS = ['styles', 'outputs', 'production', 'profiles', 'templates'];

const say = (msg) => console.log(`[SEED] ${msg}`);
const die = (stage, msg) => {
    console.error(`[SEED] ${msg}`);
    console.error(`SEED: FAIL ${stage}`);
    process.exit(1);
};

const readJson = (file) => {
    try {
        return JSON.parse(fs.readFileSync(file, 'utf8'));
    } catch (err) {
        return { __error: err.message };
    }
};

// An absolute path of any flavour Windows understands: a drive letter, or a UNC
// share. Checked against string VALUES, because half of settings.json is JSON
// text stored in a string (the audio/video rosters) and a path can live in there.
const HAS_MACHINE_PATH = /(^|[^A-Za-z0-9_])[A-Za-z]:[\\/]|\\\\[^\\]/;

const looksMachineBound = (value) => {
    if (typeof value !== 'string') return false;
    if (HAS_MACHINE_PATH.test(value)) return true;
    const home = process.env.USERPROFILE;
    return Boolean(home) && value.toLowerCase().includes(home.toLowerCase());
};

const catalogTemplateIds = () => {
    const file = path.join(SEED_DIR, 'local', 'templates.json');
    const doc = readJson(file);
    if (doc.__error || !Array.isArray(doc.designs)) return null;   // not generated yet: leave refs alone
    return new Set(doc.designs.map((d) => d.id));
};

const main = () => {
    // ---- 1. settings ------------------------------------------------------
    const settingsDoc = readJson(SETTINGS_FILE);
    if (settingsDoc.__error) {
        say(`settings: cannot read ${SETTINGS_FILE} (${settingsDoc.__error}) — leaving the seed as it is`);
    } else if (!settingsDoc.settings || typeof settingsDoc.settings !== 'object') {
        die('settings', `${SETTINGS_FILE} has no "settings" object`);
    } else {
        const templates = catalogTemplateIds();
        const kept = {};
        const dropped = [];
        for (const [key, value] of Object.entries(settingsDoc.settings)) {
            if (MACHINE_KEYS.has(key)) { dropped.push(`${key} (machine state)`); continue; }
            if (looksMachineBound(value)) { dropped.push(`${key} (names a path on this PC)`); continue; }
            // A template id that this build does not ship would point a fresh
            // install at a design it does not have: drop it so the code default
            // answers instead of a dangling id.
            if (templates && /template$/i.test(key) && typeof value === 'string'
                && value.startsWith('tpl-') && !templates.has(value)) {
                dropped.push(`${key}=${value} (not in this build's catalogs)`);
                continue;
            }
            kept[key] = value;
        }
        const document = JSON.stringify({ settings: kept, version: 1 });
        const seedSettings = path.join(SEED_DIR, 'local', 'settings.json');
        fs.mkdirSync(path.dirname(seedSettings), { recursive: true });
        fs.writeFileSync(seedSettings, document);
        // The baseline beside the exe — what every install actually merges from.
        const shipped = path.join(DIST_DIR, 'assets', 'settings.shipped.json');
        fs.mkdirSync(path.dirname(shipped), { recursive: true });
        fs.writeFileSync(shipped, document);
        say(`settings: ${Object.keys(kept).length} kept, ${dropped.length} dropped`);
        for (const why of dropped) say(`  - ${why}`);
        say(`settings: wrote ${path.relative(ROOT, seedSettings)} and ${path.relative(ROOT, shipped)}`);
    }

    // ---- 2. the engine's configuration collections ------------------------
    const kernelDoc = readJson(KERNEL_FILE);
    if (kernelDoc.__error) {
        say(`kernel: cannot read ${KERNEL_FILE} (${kernelDoc.__error}) — leaving the seed as it is`);
        return;
    }
    const collections = kernelDoc.collections || {};
    const keptCollections = {};
    let keptNames = [];
    for (const name of KERNEL_CONFIG_COLLECTIONS) {
        if (collections[name] === undefined) continue;
        keptCollections[name] = collections[name];
        keptNames.push(name);
    }
    if (keptNames.length === 0) {
        say('kernel: no configuration collections in the live store — leaving the seed as it is');
        return;
    }
    const kernelSeed = path.join(SEED_DIR, 'roaming', 'enginedata', 'kernel.json');
    fs.mkdirSync(path.dirname(kernelSeed), { recursive: true });
    fs.writeFileSync(kernelSeed, JSON.stringify({ collections: keptCollections }));
    say(`kernel: kept ${keptNames.join(', ')}`);
    say(`kernel: wrote ${path.relative(ROOT, kernelSeed)}`);

    // THE SAME CONFIGURATION AS AN EXISTING-INSTALL BASELINE. kernel.json is
    // seeded only on a fresh install; this file is the one an UPGRADE reads
    // (ShippedConfig: merged once per content fingerprint, by entry id, so the
    // build's own styles/outputs reach a running install and the user's own
    // entries are never touched). It sits beside the exe, so every install
    // replaces it — the baseline is always this build's, never a stale copy.
    const shipped = path.join(DIST_DIR, 'assets', 'kernel.shipped.json');
    fs.mkdirSync(path.dirname(shipped), { recursive: true });
    fs.writeFileSync(shipped, JSON.stringify({ version: 1, collections: keptCollections }));
    say(`kernel: wrote ${path.relative(ROOT, shipped)} (upgrade baseline)`);
};

main();
console.log('SEED: PASS');
