#!/usr/bin/env node
// ============================================================================
// tools/check_list_stripe.mjs — does the list stripe still land on the step
// ListRows.qml says it does?
//
// WHY THIS EXISTS: ListRows.qml's two alphas (darkStripeAlpha, lightStripeAlpha)
// are not a taste in isolation — they are tuned so the alternating row tint
// raises/lowers a list's ground by the SAME perceptual step (~2.9 in CIE L*) in
// both themes. That match holds only for the exact ink and ground it was
// measured against: Theme.textPrimary over Theme.surface / Theme.card. The
// moment one of those neutrals moves — a slightly lighter card, a re-inked
// textPrimary — the alphas quietly stop matching, and the two themes' lists
// start reading one stronger than the other. Nothing fails; the app just looks
// subtly wrong, which is the hardest kind of wrong to notice after the fact.
//
// So this recomputes the three documented pairs from the QML itself, in the
// same space the comments were measured in (sRGB-encoded channels blended with
// straight alpha, then CIE L* from relative luminance), and fails when a pair
// drifts off the shared step. It reads the tokens, it does not restate them:
// change Theme.qml or ListRows.qml and this measures the new numbers.
//
// Usage:
//   node tools/check_list_stripe.mjs             # the check
//   node tools/check_list_stripe.mjs --self-test # the maths, on known values
//   node tools/check_list_stripe.mjs --verbose   # + each pair's L* values
//
// Verdict (last line, greppable): LIST_STRIPE: PASS | LIST_STRIPE: FAIL
// Exit codes: 0 pass, 1 the stripe no longer matches, 2 the sources could not
// be read/parsed or the maths self-test failed (nothing was really checked).
// ============================================================================

import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const THEME = join(ROOT, "qml/settings/Theme.qml");
const LIST_ROWS = join(ROOT, "qml/settings/ListRows.qml");

// The intent, in one number: the step (in L*) that ListRows.qml was measured
// to produce in BOTH themes. It is a taste decision, recorded in that file's
// comment — moving it is a deliberate re-tune, so it is spelled out here rather
// than derived (there is nothing to derive it from).
const TARGET_STEP = 2.9;
// How far a pair may sit from TARGET_STEP, and how far the three pairs may sit
// from each other. Both are wider than the rounding in the file's comment
// (2.822…2.962, a 0.14 spread) and far narrower than any real palette move: a
// couple of L* steps on textPrimary, or ~1% of white on a ground, blows past
// them. L* steps under ~0.3 are not visible side by side, so nothing that
// passes here can be seen as a mismatch.
const TOLERANCE = 0.2;

// ---------------------------------------------------------------------------
// The maths — sRGB (0..255, gamma-encoded, exactly as the tokens are written)
// to CIE L*. L* depends only on relative luminance, so Y is all we need.
// ---------------------------------------------------------------------------

const srgbToLinear = (v) => {
    const c = v / 255;
    return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
};

/**
 * CIE L* (0 = black, 100 = the reference white) of an [r,g,b] in 0..255.
 *
 * The piecewise definition matters here rather than being pedantry: a dark
 * ground (#15161d) sits BELOW the (6/29)^3 knee, so it takes the linear branch
 * and the cube root would report it a whole 0.2 low — the same order as the
 * tolerance this file checks. Both branches are continuous at the knee, so the
 * step between two colours that straddle it is still right.
 */
const lstar = (rgb) => {
    const y = 0.2126 * srgbToLinear(rgb[0]) +
              0.7152 * srgbToLinear(rgb[1]) +
              0.0722 * srgbToLinear(rgb[2]);
    return y <= 216 / 24389 ? y * (24389 / 27) : 116 * Math.cbrt(y) - 16;
};

/** "#rrggbb" -> [r,g,b]. */
const hex = (s) => [1, 3, 5].map((i) => parseInt(s.slice(i, i + 2), 16));

/**
 * A tint laid over a ground: straight-alpha blend of the two channel values.
 * This is what Qt does for an ARGB overlay on an sRGB surface — the blend
 * happens on the stored (gamma-encoded) values, not in linear light. Blending
 * in linear light would make the same alpha read much stronger, which is why
 * the alphas are as small as they are; getting this wrong here would report a
 * match that the screen does not have.
 */
const over = (ink, ground, alpha) =>
    ink.map((c, i) => alpha * c + (1 - alpha) * ground[i]);

// ---------------------------------------------------------------------------
// Reading the tokens out of the QML
// ---------------------------------------------------------------------------

const read = (path) => {
    try {
        return readFileSync(path, "utf8");
    } catch (e) {
        throw new Error(`cannot read ${path}: ${e.message}`);
    }
};

/**
 * `readonly property color name: dark ? "#xxxxxx" : "#yyyyyy"` — the shape
 * every neutral in Theme.qml uses. A token that stops matching this shape is a
 * FAILURE, not a skip: this check cannot measure what it cannot read, and
 * silently reporting "fine" is the failure mode it exists to prevent.
 */
function themePair(source, name) {
    const re = new RegExp(
        `readonly\\s+property\\s+color\\s+${name}\\s*:\\s*dark\\s*\\?\\s*"(#[0-9a-fA-F]{6})"\\s*:\\s*"(#[0-9a-fA-F]{6})"`);
    const m = re.exec(source);
    if (!m)
        throw new Error(`Theme.qml: could not read \`${name}\` as \`dark ? "#…" : "#…"\` ` +
                        `(did the token get renamed, or stop being a per-theme pair?)`);
    return { dark: m[1].toLowerCase(), light: m[2].toLowerCase() };
}

/** `readonly property real darkStripeAlpha: 0.028`. */
function listRowsReal(source, name) {
    const re = new RegExp(`readonly\\s+property\\s+real\\s+${name}\\s*:\\s*([0-9.]+)`);
    const m = re.exec(source);
    if (!m)
        throw new Error(`ListRows.qml: could not read \`${name}\` as a literal number`);
    const v = Number(m[1]);
    if (!(v > 0 && v < 1))
        throw new Error(`ListRows.qml: \`${name}\` is ${m[1]}, which is not an alpha in (0,1)`);
    return v;
}

// ---------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------

function measure() {
    const themeSrc = read(THEME);
    const rowsSrc = read(LIST_ROWS);

    // The stripe is the theme's own ink laid over a list's ground — that SHAPE
    // is what the numbers were measured for, so verify it is still the shape.
    // (A stripe switched to its own tint, or to an opaque colour, is not
    // covered by this measurement at all.)
    if (!/Qt\.alpha\(\s*Theme\.textPrimary\s*,\s*stripeAlpha\s*\)/.test(rowsSrc))
        throw new Error("ListRows.qml: `stripe` is no longer `Qt.alpha(Theme.textPrimary, stripeAlpha)` — " +
                        "this check measures that shape and nothing else");

    const textPrimary = themePair(themeSrc, "textPrimary");
    const surface = themePair(themeSrc, "surface");
    const card = themePair(themeSrc, "card");

    const darkAlpha = listRowsReal(rowsSrc, "darkStripeAlpha");
    const lightAlpha = listRowsReal(rowsSrc, "lightStripeAlpha");

    const rows = [
        // The two dark grounds the stripe is documented against: the Settings
        // surface and the card that sits on it (a list inside a card is the
        // common case, and it is the lighter ground, so its step is the base
        // the dark alpha is judged by).
        { theme: "dark", over: "surface", ink: textPrimary.dark, alpha: darkAlpha, ground: surface.dark },
        { theme: "dark", over: "card", ink: textPrimary.dark, alpha: darkAlpha, ground: card.dark },
        // Light: the ink is the light textPrimary and the ground is white in
        // both the surface and the card, so one pair covers it.
        { theme: "light", over: "surface", ink: textPrimary.light, alpha: lightAlpha, ground: surface.light },
    ].map((r) => {
        const base = lstar(hex(r.ground));
        const tinted = lstar(over(hex(r.ink), hex(r.ground), r.alpha));
        return { ...r, base, tinted, delta: tinted - base };
    });

    return { rows, darkAlpha, lightAlpha };
}

function report({ rows }, verbose) {
    let failures = 0;
    const steps = rows.map((r) => Math.abs(r.delta));

    for (const r of rows) {
        const step = Math.abs(r.delta);
        const off = step - TARGET_STEP;
        const ok = Math.abs(off) <= TOLERANCE;
        if (!ok)
            ++failures;
        console.log(
            `  ${r.theme.padEnd(5)} ink ${r.ink} @${r.alpha} over ${r.over.padEnd(7)} ${r.ground}` +
            `  L* ${r.base.toFixed(2)} -> ${r.tinted.toFixed(2)}` +
            `  step ${r.delta >= 0 ? "+" : ""}${r.delta.toFixed(2)}` +
            `  (${off >= 0 ? "+" : ""}${off.toFixed(2)} from ${TARGET_STEP.toFixed(1)})` +
            (ok ? "" : "  <-- MISMATCH") +
            (verbose ? "" : ""));
    }

    const spread = Math.max(...steps) - Math.min(...steps);
    if (spread > TOLERANCE) {
        ++failures;
        console.log(`  the two themes disagree: spread ${spread.toFixed(2)} > ${TOLERANCE.toFixed(1)} ` +
                    `(one list reads stronger than the other)`);
    }

    // Two ways to be wrong: a pair off the shared step, or the pairs split.
    // Naming the fix keeps the failure actionable — the alphas are the knob,
    // not the neutrals (unless the palette move was the point, in which case
    // both number and comment in ListRows.qml move together).
    if (failures > 0) {
        console.log("");
        console.log("  The stripe no longer lands on the shared step. Either:");
        console.log("    * put Theme.textPrimary / surface / card back where they were, or");
        console.log("    * re-tune darkStripeAlpha / lightStripeAlpha in qml/settings/ListRows.qml");
        console.log("      and update the measured numbers in its comment and TARGET_STEP here.");
        console.log("");
        console.log(`LIST_STRIPE: FAIL ${failures} pair(s)/inconsistenc(ies) — ` +
                    `dark ${rows[0].delta.toFixed(2)} / ${rows[1].delta.toFixed(2)}, ` +
                    `light ${rows[2].delta.toFixed(2)}, spread ${spread.toFixed(2)}`);
        return 1;
    }

    console.log("");
    console.log(`LIST_STRIPE: PASS step dark ${rows[0].delta.toFixed(2)} / ${rows[1].delta.toFixed(2)}, ` +
                `light ${rows[2].delta.toFixed(2)}, spread ${spread.toFixed(2)} (target ${TARGET_STEP.toFixed(1)} ±${TOLERANCE.toFixed(1)})`);
    return 0;
}

// ---------------------------------------------------------------------------
// --self-test: the conversion, not the stripe. A clean number from broken maths
// looks exactly like a clean number from a matched palette, so the maths is
// held to values that are true by definition before the tree is believed.
// ---------------------------------------------------------------------------

function selfTest() {
    let failures = 0;
    const check = (what, ok) => {
        console.log(`  ${what.padEnd(56)} ${ok ? "ok" : "FAIL"}`);
        if (!ok)
            ++failures;
    };

    check("L* of black is 0", Math.abs(lstar([0, 0, 0])) < 1e-9);
    check("L* of white is 100", Math.abs(lstar([255, 255, 255]) - 100) < 1e-9);
    check("L* of mid grey (#808080) is 53.59", Math.abs(lstar([128, 128, 128]) - 53.585) < 0.01);
    check("alpha 0 leaves the ground alone",
          over(hex("#e2e8f0"), hex("#15161d"), 0).every((c, i) => Math.abs(c - hex("#15161d")[i]) < 1e-9));
    check("alpha 1 is the ink exactly",
          over(hex("#e2e8f0"), hex("#15161d"), 1).every((c, i) => Math.abs(c - hex("#e2e8f0")[i]) < 1e-9));
    // The measurement the whole check rests on, on the numbers ListRows.qml was
    // tuned against — if this pair stops reading ~2.96 the maths changed, not
    // the palette.
    const documented = lstar(over(hex("#e2e8f0"), hex("#15161d"), 0.028)) - lstar(hex("#15161d"));
    check(`the documented dark pair still measures ${documented.toFixed(2)} (~+2.95)`,
          Math.abs(documented - 2.95) < 0.01);
    // A stripe that is too strong has to be SEEN by the check, not absorbed by
    // a tolerance: doubling the alpha must be a large step, not a small one.
    const doubled = Math.abs(lstar(over(hex("#e2e8f0"), hex("#15161d"), 0.056)) - lstar(hex("#15161d")));
    check("a doubled alpha reads as roughly double the step", doubled > TARGET_STEP * 1.8);

    console.log("");
    if (failures === 0) {
        console.log("LIST_STRIPE_SELFTEST: PASS");
        return 0;
    }
    console.log(`LIST_STRIPE_SELFTEST: FAIL ${failures} assertion(s)`);
    return 2;
}

// ---------------------------------------------------------------------------

const argv = process.argv.slice(2);
const verbose = argv.includes("--verbose");

if (argv.includes("--self-test")) {
    process.exit(selfTest());
}

try {
    const result = measure();
    if (verbose)
        console.log(`darkStripeAlpha ${result.darkAlpha}  lightStripeAlpha ${result.lightAlpha}`);
    process.exit(report(result, verbose));
} catch (e) {
    console.log(`LIST_STRIPE: FAIL unreadable — ${e.message} (nothing was measured)`);
    process.exit(2);
}
