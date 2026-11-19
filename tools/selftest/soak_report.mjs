// Reads a soak run (see run_soak_test.sh) and prints one row per minute plus a verdict.
//   node soak_report.mjs <samples.csv> <app log> <engine.log> <burners>
import fs from 'node:fs';

const [csvPath, appLogPath, engineLogPath, burners] = process.argv.slice(2);
const read = (p) => { try { return fs.readFileSync(p, 'utf8'); } catch { return ''; } };

// ---- process samples ----
const rows = read(csvPath).trim().split(/\r?\n/).slice(1).filter(Boolean).map((l) => {
  const [t, priv, ws, handles, threads, cpu] = l.split(',').map(Number);
  return { t, priv, ws, handles, threads, cpu };
});

// ---- the scenario's own lag lines: [SELFTEST-SOAK] t=15 ... lagAvg=1.2 lagMax=210 lag100=2 ----
const lag = [];
for (const m of read(appLogPath).matchAll(/\[SELFTEST-SOAK\] t=(\d+) presses=(\d+) picks=(\d+).*?lagAvg=([\d.]+) lagMax=(\d+) lag100=(\d+)/g))
  lag.push({ t: +m[1], presses: +m[2], picks: +m[3], avg: +m[4], max: +m[5], over100: +m[6] });

// ---- GuiStall lines from engine.log: "GuiStall: the interface thread was silent for about 168 ms" ----
const stalls = [];
for (const m of read(engineLogPath).matchAll(/^(\d\d):(\d\d):(\d\d)\.\d+ .*GuiStall: the interface thread was silent for about (\d+) ms/gm))
  stalls.push({ at: (+m[1]) * 3600 + (+m[2]) * 60 + (+m[3]), ms: +m[4] });
const firstLogSec = (() => { const m = /^(\d\d):(\d\d):(\d\d)\./m.exec(read(engineLogPath)); return m ? (+m[1]) * 3600 + (+m[2]) * 60 + (+m[3]) : 0; })();

const stepCosts = [...read(engineLogPath).matchAll(/StepCost\] notify readingScrollChanged=(\d+)ms/g)].map((m) => +m[1]);
const pickCosts = [...read(engineLogPath).matchAll(/PickCostSvc\][^\n]*?(\d+)ms/g)].length;

if (rows.length < 3 || lag.length < 2) {
  console.log(`SOAK: FAIL notrun — samples=${rows.length} lag lines=${lag.length} (the scenario never ran long enough; see the app log)`);
  process.exit(3);
}

const minutes = Math.max(1, Math.ceil(rows[rows.length - 1].t / 60));
console.log('\n min | private MB | handles | threads | cpu s | lag avg  max  >100ms | stalls  stall-ms');
for (let m = 0; m < minutes; m++) {
  const inMin = rows.filter((r) => r.t >= m * 60 && r.t < (m + 1) * 60);
  const lagMin = lag.filter((l) => l.t >= m * 60 && l.t < (m + 1) * 60);
  const stallMin = stalls.filter((s) => s.at - firstLogSec >= m * 60 && s.at - firstLogSec < (m + 1) * 60);
  const last = inMin[inMin.length - 1];
  if (!last) continue;
  const avg = lagMin.length ? lagMin.reduce((a, l) => a + l.avg, 0) / lagMin.length : 0;
  const mx = lagMin.reduce((a, l) => Math.max(a, l.max), 0);
  const over = lagMin.reduce((a, l) => a + l.over100, 0);
  console.log(
    ` ${String(m + 1).padStart(3)} | ${last.priv.toFixed(0).padStart(10)} | ${String(last.handles).padStart(7)} | ${String(last.threads).padStart(7)} | ${last.cpu.toFixed(0).padStart(5)} | ` +
    `${avg.toFixed(1).padStart(7)} ${String(mx).padStart(4)} ${String(over).padStart(7)} | ${String(stallMin.length).padStart(6)} ${String(stallMin.reduce((a, s) => a + s.ms, 0)).padStart(9)}`);
}

// ---- verdict: judge the second half against the first (warm-up excluded) ----
const half = Math.floor(rows.length / 2);
const early = rows.slice(Math.min(2, half), half || 1);
const late = rows.slice(half);
const mean = (xs, f) => xs.reduce((a, x) => a + f(x), 0) / Math.max(1, xs.length);
const privGrowth = mean(late, (r) => r.priv) / Math.max(1, mean(early, (r) => r.priv)) - 1;
const handleGrowth = mean(late, (r) => r.handles) - mean(early, (r) => r.handles);
const threadGrowth = mean(late, (r) => r.threads) - mean(early, (r) => r.threads);
const lagEarly = lag.slice(0, Math.floor(lag.length / 2));
const lagLate = lag.slice(Math.floor(lag.length / 2));
const over100Late = lagLate.reduce((a, l) => a + l.over100, 0);
const over100Early = lagEarly.reduce((a, l) => a + l.over100, 0);
const lagMaxAll = lag.reduce((a, l) => Math.max(a, l.max), 0);
const worstStall = stalls.reduce((a, s) => Math.max(a, s.ms), 0);
const stepP95 = (() => { const s = [...stepCosts].sort((a, b) => a - b); return s.length ? s[Math.floor(s.length * 0.95)] : 0; })();

console.log(`\n burners=${burners}  memory growth (2nd half vs 1st): ${(privGrowth * 100).toFixed(1)}%  handles ${handleGrowth >= 0 ? '+' : ''}${handleGrowth.toFixed(0)}  threads ${threadGrowth >= 0 ? '+' : ''}${threadGrowth.toFixed(0)}`);
console.log(` lag >100 ms: first half ${over100Early}, second half ${over100Late}; worst lag ${lagMaxAll} ms; worst GuiStall ${worstStall} ms; ` +
            `step cost p95 ${stepP95} ms over ${stepCosts.length} steps; GuiStalls total ${stalls.length}`);

// LONG RUNS (over 20 minutes) are judged by SLOPE, not by half against half: the first ten minutes are the app loading and
// settling, after that a flat line is the goal. Least-squares slope of the samples after the warm-up, per hour.
const slopePerHour = (xs) => {
  const pts = xs.filter((r) => r.t >= 600);
  if (pts.length < 6) return null;
  const n = pts.length, mx = pts.reduce((a, p) => a + p.t, 0) / n;
  const my = pts.reduce((a, p) => a + p.v, 0) / n;
  let num = 0, den = 0;
  for (const p of pts) { num += (p.t - mx) * (p.v - my); den += (p.t - mx) ** 2; }
  return den === 0 ? null : (num / den) * 3600;
};
const memSlope = slopePerHour(rows.map((r) => ({ t: r.t, v: r.priv })));
const handleSlope = slopePerHour(rows.map((r) => ({ t: r.t, v: r.handles })));
const threadSlope = slopePerHour(rows.map((r) => ({ t: r.t, v: r.threads })));
const longRun = rows[rows.length - 1].t > 20 * 60 && memSlope !== null;
if (longRun)
  console.log(` slope after warm-up: memory ${memSlope.toFixed(0)} MB/h, handles ${handleSlope.toFixed(0)}/h, threads ${threadSlope.toFixed(1)}/h`);

const fails = [];
if (longRun) {
  if (memSlope > 150) fails.push(`memory (+${memSlope.toFixed(0)} MB/h)`);
  if (handleSlope > 300) fails.push(`handles (+${handleSlope.toFixed(0)}/h)`);
  if (threadSlope > 10) fails.push(`threads (+${threadSlope.toFixed(1)}/h)`);
} else {
  if (privGrowth > 0.25) fails.push(`memory (+${(privGrowth * 100).toFixed(0)}%)`);
  if (handleGrowth > 200) fails.push(`handles (+${handleGrowth.toFixed(0)})`);
  if (threadGrowth > 20) fails.push(`threads (+${threadGrowth.toFixed(0)})`);
}
if (over100Late > over100Early * 2 + 10) fails.push(`lag (the second half is slower: ${over100Early} -> ${over100Late} late ticks)`);
if (lagMaxAll > 2000) fails.push(`stalls (worst lag ${lagMaxAll} ms)`);
if (fails.length) { console.log(`SOAK: FAIL ${fails.join(' | ')}`); process.exit(1); }
console.log('SOAK: PASS');
