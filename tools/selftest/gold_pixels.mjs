#!/usr/bin/env node
// ============================================================================
// tools/selftest/gold_pixels.mjs — did the highlight actually PAINT?
//
// WHY THIS EXISTS: every property-level trace of the pane's result rows said
// the markup was there ("<b><font color=\"#ffd54a\">compelling</font></b>") while
// the user's screenshot showed the word plain grey beside a yellow "that". The
// only witness that settles it is the pixel: Qt's StyledText honors a color
// attribute or it does not, and no binding trace can tell you which.
//
// Reads a PNG (8-bit, greyscale/RGB/RGBA — the SelfTestDriver's grabToImage
// output), deflates it with node's zlib, unfilters it, and counts pixels within
// `--tol` of the target color. Also reports the most common WARM pixel, so a
// render in the WRONG gold (the app has two: the highlight's #ffd54a and
// Theme.accent) is visible instead of a silent zero.
//
// Usage:
//   node tools/selftest/gold_pixels.mjs <png> [--color #ffd54a] [--tol 40] [--min 20]
//
// VERDICT (last line, greppable):
//   GOLDPIX: PASS <png> gold=<n> warm=<n> warmTop=#rrggbb
//   GOLDPIX: FAIL <png> gold=<n> (want >= <min>) warm=<n> warmTop=#rrggbb
//   GOLDPIX: FAIL read — <reason>
// ============================================================================

import { readFileSync } from 'node:fs';
import { inflateSync } from 'node:zlib';

function arg(name, fallback) {
    const i = process.argv.indexOf(name);
    return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
}

const path = process.argv[2];
const target = arg('--color', '#ffd54a');
const tol = Number(arg('--tol', '40'));
const min = Number(arg('--min', '20'));

function parseHex(hex) {
    const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
    if (!m) throw new Error(`not a #rrggbb color: ${hex}`);
    const n = parseInt(m[1], 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

function decode(file) {
    const buf = readFileSync(file);
    if (buf.length < 8 || buf.readUInt32BE(0) !== 0x89504e47)
        throw new Error('not a PNG');
    let pos = 8;
    let width = 0, height = 0, depth = 0, colorType = 0;
    const idat = [];
    while (pos + 8 <= buf.length) {
        const len = buf.readUInt32BE(pos);
        const type = buf.toString('ascii', pos + 4, pos + 8);
        const data = buf.subarray(pos + 8, pos + 8 + len);
        pos += 12 + len;
        if (type === 'IHDR') {
            width = data.readUInt32BE(0);
            height = data.readUInt32BE(4);
            depth = data[8];
            colorType = data[9];
            if (depth !== 8) throw new Error(`bit depth ${depth} (want 8)`);
            if (data[12] !== 0) throw new Error('interlaced PNG');
        } else if (type === 'IDAT') {
            idat.push(data);
        } else if (type === 'IEND') {
            break;
        }
    }
    const channels = { 0: 1, 2: 3, 4: 2, 6: 4 }[colorType];
    if (!channels) throw new Error(`unsupported color type ${colorType}`);
    const raw = inflateSync(Buffer.concat(idat));
    const stride = width * channels;
    const out = Buffer.alloc(height * stride);
    let p = 0;
    for (let y = 0; y < height; ++y) {
        const filter = raw[p++];
        const line = Buffer.from(raw.subarray(p, p + stride));
        p += stride;
        const prev = y > 0 ? out.subarray((y - 1) * stride, y * stride) : Buffer.alloc(stride);
        for (let i = 0; i < stride; ++i) {
            const a = i >= channels ? line[i - channels] : 0;
            const b = prev[i];
            const c = i >= channels ? prev[i - channels] : 0;
            let add = 0;
            if (filter === 1) add = a;
            else if (filter === 2) add = b;
            else if (filter === 3) add = (a + b) >> 1;
            else if (filter === 4) {
                const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
                add = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            } else if (filter !== 0) {
                throw new Error(`unknown filter ${filter} on row ${y}`);
            }
            line[i] = (line[i] + add) & 255;
        }
        line.copy(out, y * stride);
    }
    return { width, height, channels, pixels: out };
}

try {
    const [tr, tg, tb] = parseHex(target);
    const { width, height, channels, pixels } = decode(path);
    let gold = 0;
    const warm = new Map();
    for (let i = 0; i < pixels.length; i += channels) {
        const r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
        if (Math.abs(r - tr) <= tol && Math.abs(g - tg) <= tol && Math.abs(b - tb) <= tol)
            ++gold;
        // Warm = a highlight-ish hue whatever its exact gold: enough to name the
        // color the row was painted in when it was not the one asked for.
        if (r > 150 && g > 110 && b < 150 && r - b > 50) {
            const key = (r << 16) | (g << 8) | b;
            warm.set(key, (warm.get(key) ?? 0) + 1);
        }
    }
    let warmTotal = 0, warmTop = 0x000000;
    for (const [key, n] of warm) {
        warmTotal += n;
        if (n > (warm.get(warmTop) ?? -1)) warmTop = key;
    }
    const hex = '#' + warmTop.toString(16).padStart(6, '0');
    const head = `${path} ${width}x${height} gold=${gold} warm=${warmTotal} warmTop=${hex}`;
    if (gold >= min) {
        console.log(`GOLDPIX: PASS ${head}`);
        process.exit(0);
    }
    console.log(`GOLDPIX: FAIL ${head} (want >= ${min} of ${target} ±${tol})`);
    process.exit(1);
} catch (err) {
    console.log(`GOLDPIX: FAIL read — ${err.message}`);
    process.exit(2);
}
