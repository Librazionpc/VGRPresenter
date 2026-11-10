#!/usr/bin/env node
// ---------------------------------------------------------------------------
// check_platform_boundary.mjs — the platform cage's source guard.
//
// The cage (docs/platform/qt_first_platform.md) says OS work happens in ONE
// place: src/PresentationEngine/platform/. Everywhere else talks to a PAL
// interface (IFilesystem, IPaths, IVideo, ...). A single `#include <windows.h>`
// in a feature module re-opens the door the whole architecture closes — three
// divergent implementations, a build that only compiles on one OS, and a lock
// or device wait nobody audited.
//
// TWO RULES:
//   * core / modules / interfaces  — no OS header AND no Qt header. This is the
//     platform-free production engine: standard C++ plus the PAL interfaces.
//   * qml/                          — no OS header. Qt is its framework (QML is
//     Qt), but a Win32/Unix call in the interface layer is the same bug.
//
// platform/ is exempt by construction (it IS the cage); tests/unit is exempt
// because the parity suites deliberately compare a Qt service against the
// native one.
//
// Usage:
//   node tools/check_platform_boundary.mjs                  # the gate
//   node tools/check_platform_boundary.mjs --list           # compact file:line list
//   node tools/check_platform_boundary.mjs --update-baseline  # record current findings
//
// Existing violations are recorded in tools/platform_boundary_baseline.json
// (path + rule, with the reason they are still native/Qt). A NEW one fails the
// build; shrink the ledger as they are moved behind the PAL.
// ---------------------------------------------------------------------------

import { existsSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { join, relative, sep } from 'node:path';

const ROOT = process.cwd();
const BASELINE = join(ROOT, 'tools', 'platform_boundary_baseline.json');
const ENGINE = 'src/PresentationEngine';
const SOURCE_EXT = /\.(h|hpp|c|cc|cpp|cxx|mm)$/;

const RULES = {
  'os-header': {
    // An operating system's own headers. Angle includes only: a quoted include
    // of a PAL header ("platform/windows/...") is the legal seam.
    pattern: /#\s*include\s*<(?:windows|winsock2|ws2tcpip|shlobj|shellapi|d3d11|d3d12|dxgi|mfapi|mferror|mfidl|mfreadwrite|mftransform|audioclient|mmdeviceapi|endpointvolume|iphlpapi|netioapi|pdh|tlhelp32|psapi|setupapi|cfgmgr32|unistd|fcntl|dlfcn|pthread|signal|ifaddrs|sys\/[^>]+|X11\/[^>]+|linux\/[^>]+|CoreServices\/[^>]+|mach\/[^>]+|IOKit\/[^>]+)>/i,
    label: 'OS header',
  },
  'qt-header': {
    // Qt's own headers. The engine may link Qt (Gui is a PUBLIC dependency for
    // the renderer's text engine), but "OS work" and Qt widgets must not spread
    // into feature modules: that is what the PAL and the services layer are for.
    pattern: /#\s*include\s*<(?:Q[A-Z][^>]*|QtCore\/[^>]+|QtGui\/[^>]+|QtQuick\/[^>]+|QtWidgets\/[^>]+)>/,
    label: 'Qt header',
  },
};

const SCOPES = [
  { dir: `${ENGINE}/core`, rules: ['os-header', 'qt-header'] },
  { dir: `${ENGINE}/modules`, rules: ['os-header', 'qt-header'] },
  { dir: `${ENGINE}/interfaces`, rules: ['os-header', 'qt-header'] },
  { dir: 'qml', rules: ['os-header'] },
];

function walk(dir, out) {
  let entries;
  try {
    entries = readdirSync(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const entry of entries) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === '.git' || entry.name === 'node_modules' || entry.name === 'build') continue;
      walk(path, out);
    } else if (SOURCE_EXT.test(entry.name)) {
      out.push(path);
    }
  }
}

function normalise(path) {
  return relative(ROOT, path).split(sep).join('/');
}

function loadBaseline() {
  if (!existsSync(BASELINE)) return [];
  try {
    const parsed = JSON.parse(readFileSync(BASELINE, 'utf8'));
    return Array.isArray(parsed.entries) ? parsed.entries : [];
  } catch {
    return [];
  }
}

function scan() {
  const findings = [];
  const files = [];
  for (const scope of SCOPES) walk(join(ROOT, scope.dir), files);
  for (const file of files) {
    const rel = normalise(file);
    const scope = SCOPES.find((s) => rel === s.dir || rel.startsWith(`${s.dir}/`));
    if (!scope) continue;
    let text;
    try {
      text = readFileSync(file, 'utf8');
    } catch {
      continue;
    }
    const lines = text.split(/\r?\n/);
    for (const ruleId of scope.rules) {
      const rule = RULES[ruleId];
      lines.forEach((line, i) => {
        // A commented-out include is not a dependency (the guards' fixtures and
        // the documentation both spell examples in comments).
        const code = line.replace(/^\s*\/\/.*$/, '');
        if (rule.pattern.test(code)) {
          findings.push({
            path: rel,
            line: i + 1,
            rule: ruleId,
            label: rule.label,
            source: line.trim(),
          });
        }
      });
    }
  }
  return findings;
}

function main() {
  const args = process.argv.slice(2);
  const updateBaseline = args.includes('--update-baseline');
  const list = args.includes('--list');
  const findings = scan();
  const baseline = loadBaseline();
  const waived = new Map(baseline.map((e) => [`${e.path}\u0000${e.rule}`, e.reason || '']));

  if (updateBaseline) {
    const entries = [];
    const seen = new Set();
    for (const f of findings) {
      const key = `${f.path}\u0000${f.rule}`;
      if (seen.has(key)) continue;
      seen.add(key);
      const prior = waived.get(key);
      entries.push({ path: f.path, rule: f.rule, reason: prior || 'existing site — move behind the PAL or record why it must stay' });
    }
    const stale = baseline.filter((e) => !seen.has(`${e.path}\u0000${e.rule}`));
    writeFileSync(
      BASELINE,
      `${JSON.stringify(
        {
          note: 'Existing platform-boundary violations, waived by file+rule. Shrink this file as sites move behind the PAL; every entry needs a reason.',
          entries,
        },
        null,
        2,
      )}\n`,
    );
    console.log(`platform-boundary: baseline written — ${entries.length} waived, ${stale.length} stale removed`);
  }

  const fresh = findings.filter((f) => !waived.has(`${f.path}\u0000${f.rule}`));
  const waivedCount = findings.length - fresh.length;

  if (list) {
    for (const f of fresh) console.log(`${f.path}:${f.line} [${f.rule}] ${f.source}`);
  } else {
    for (const f of fresh) {
      console.error(`FAIL ${f.path}:${f.line} [${f.rule}] ${f.source}`);
      console.error(`     ${f.label}: move this behind the PAL (platform/) or a Qt service — docs/platform/qt_first_platform.md`);
    }
  }

  if (fresh.length > 0) {
    console.error(
      `platform-boundary: FAIL ${fresh.length} new finding(s)${waivedCount ? `, ${waivedCount} waived` : ''}`,
    );
    process.exit(1);
  }
  console.log(`platform-boundary: PASS (${findings.length} findings, all waived by the baseline ledger)`);
}

main();
