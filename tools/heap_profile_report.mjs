#!/usr/bin/env node
// Reads build/logs/heapprofile_<s>s.txt (written by src/HeapProfile.cpp when the app runs with VGR_HEAP_PROFILE=1) and
// prints where the live heap is: by the module that asked for the memory, by the first function of OURS on the stack,
// and the biggest stacks with their frames named. Our exe is named with nm; other modules are named by their export
// table when they have one, else by module+offset.
//
//   node tools/heap_profile_report.mjs build/logs/heapprofile_20s.txt [--top 30] [--stacks 12]
import fs from 'fs';
import path from 'path';
import { execFileSync, spawnSync } from 'child_process';

const args = process.argv.slice(2);
const file = args.find(a => !a.startsWith('--'));
const opt = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? Number(args[i + 1]) : d; };
if (!file) { console.error('usage: heap_profile_report.mjs <heapprofile_Ns.txt> [--top N] [--stacks N]'); process.exit(2); }
const TOP = opt('top', 30), NSTACKS = opt('stacks', 12);
const root = path.resolve(process.argv[1], "..", "..");
const buildDir = path.join(root, 'build');
const NM = process.env.NM_BIN || 'C:/msys64/ucrt64/bin/nm.exe';

const text = fs.readFileSync(file, 'utf8').split(/\r?\n/);
const header = text[0];
const stacks = [];
for (let i = 1; i < text.length; i++) {
    const m = /^S ([\d.]+) MB (\d+) blocks/.exec(text[i]);
    if (!m) continue;
    const frames = [];
    while (i + 1 < text.length && text[i + 1].startsWith('  ')) { frames.push(text[++i].trim()); }
    stacks.push({ mb: Number(m[1]), blocks: Number(m[2]), frames: frames.map(f => { const k = f.lastIndexOf('+0x'); return { mod: f.slice(0, k), rva: parseInt(f.slice(k + 3), 16) }; }) });
}

// symbol tables per module: the exe has symbols (nm); any other DLL is named by its export table, which for Qt is most of
// its public functions — the nearest export below an address is a good guess at the function it sits in.
const DLL_DIRS = [buildDir, 'C:/Qt/6.11.1/mingw_64/bin', 'C:/msys64/ucrt64/bin', 'C:/Windows/System32'];
const CPPFILT = process.env.CPPFILT_BIN || 'C:/msys64/ucrt64/bin/c++filt.exe';
function peExports(file) {
    const b = fs.readFileSync(file);
    const pe = b.readUInt32LE(0x3c);
    const nsec = b.readUInt16LE(pe + 6), optSize = b.readUInt16LE(pe + 20);
    const opt = pe + 24, magic = b.readUInt16LE(opt);
    const ddOff = opt + (magic === 0x20b ? 112 : 96);
    const expRva = b.readUInt32LE(ddOff);
    if (!expRva) return [];
    const secs = [];
    for (let i = 0; i < nsec; i++) { const o = opt + optSize + i * 40; secs.push({ va: b.readUInt32LE(o + 12), size: Math.max(b.readUInt32LE(o + 8), b.readUInt32LE(o + 16)), raw: b.readUInt32LE(o + 20) }); }
    const off = rva => { for (const sc of secs) if (rva >= sc.va && rva < sc.va + sc.size) return rva - sc.va + sc.raw; return -1; };
    const e = off(expRva);
    const nNames = b.readUInt32LE(e + 24), fns = off(b.readUInt32LE(e + 28)), names = off(b.readUInt32LE(e + 32)), ords = off(b.readUInt32LE(e + 36));
    const out = [];
    for (let i = 0; i < nNames; i++) {
        const no = off(b.readUInt32LE(names + i * 4)); let end = no; while (b[end]) end++;
        const ord = b.readUInt16LE(ords + i * 2); const rva = b.readUInt32LE(fns + ord * 4);
        if (rva && rva < 0x7fffffff) out.push([rva, b.toString('latin1', no, end)]);
    }
    out.sort((x, y) => x[0] - y[0]);
    const r = spawnSync(CPPFILT, [], { input: out.map(x => x[1]).join('\n'), maxBuffer: 1 << 28 });
    if (r.status === 0) { const dm = r.stdout.toString().split(/\r?\n/); out.forEach((x, i) => { if (dm[i]) x[1] = dm[i]; }); }
    return out;
}
const symCache = new Map();
function loadSyms(mod) {
    if (symCache.has(mod)) return symCache.get(mod);
    let syms = [];
    try {
        const exe = path.join(buildDir, mod);
        if (/^appVGRPresenterUI\.exe$/i.test(mod) && fs.existsSync(exe)) {
            const out = execFileSync(NM, ['-n', '-C', '--defined-only', exe], { maxBuffer: 1 << 30 }).toString();
            const base = 0x140000000;
            for (const line of out.split(/\r?\n/)) {
                const m = /^([0-9a-f]+) \w (.+)$/.exec(line);
                if (m) syms.push([parseInt(m[1], 16) - base, m[2]]);
            }
        } else {
            const dir = DLL_DIRS.find(d => fs.existsSync(path.join(d, mod)));
            if (dir) syms = peExports(path.join(dir, mod));
        }
    } catch (e) { /* no symbols */ }
    symCache.set(mod, syms);
    return syms;
}
function nameOf(f) {
    const syms = loadSyms(f.mod);
    if (!syms.length) return `${f.mod}+0x${f.rva.toString(16)}`;
    let lo = 0, hi = syms.length - 1, best = -1;
    while (lo <= hi) { const mid = (lo + hi) >> 1; if (syms[mid][0] <= f.rva) { best = mid; lo = mid + 1; } else hi = mid - 1; }
    return best < 0 ? `${f.mod}+0x${f.rva.toString(16)}` : syms[best][1].replace(/\(.*$/, '').slice(0, 110);
}

const RUNTIME = /^(ucrtbase|msvcrt|libstdc\+\+-6|ntdll|KERNEL32|KERNELBASE|libgcc_s_seh-1|api-ms-win.*|libwinpthread-1)\.dll$/i;
const byOwner = new Map(), byOurs = new Map();
let total = 0;
for (const s of stacks) {
    total += s.mb;
    const owner = s.frames.find(f => f.mod !== '?' && !RUNTIME.test(f.mod));
    const va = s.frames[0] && s.frames[0].mod === '?';
    const o = (va ? '[VirtualAlloc] ' : '') + (owner ? owner.mod : '(runtime only)');
    byOwner.set(o, (byOwner.get(o) || 0) + s.mb);
    const ours = s.frames.find(f => /appVGRPresenterUI\.exe/i.test(f.mod));
    const k = ours ? nameOf(ours) : '(none of our frames within the captured depth)';
    byOurs.set(k, (byOurs.get(k) || 0) + s.mb);
}
const show = (title, map) => {
    console.log(`\n== ${title}`);
    [...map.entries()].sort((a, b) => b[1] - a[1]).slice(0, TOP).forEach(([k, v]) => console.log(`${v.toFixed(1).padStart(8)} MB  ${k}`));
};
console.log(header);
console.log(`(stacks listed in the file: ${stacks.length}, ${total.toFixed(1)} MB)`);
show('by the module that asked for the memory (first non-runtime frame, leaf outward)', byOwner);
show('by the first function of OURS on the stack', byOurs);
console.log('\n== biggest stacks');
stacks.slice().sort((a, b) => b.mb - a.mb).slice(0, NSTACKS).forEach(s => {
    console.log(`\n${s.mb.toFixed(2)} MB in ${s.blocks} block(s)`);
    s.frames.forEach(f => console.log('    ' + (loadSyms(f.mod).length ? nameOf(f) : `${f.mod}+0x${f.rva.toString(16)}`)));
});
