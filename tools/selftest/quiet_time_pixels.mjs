#!/usr/bin/env node
// ============================================================================
// tools/selftest/quiet_time_pixels.mjs — did the WALL's tile actually paint the
// Quiet Time design after GO LIVE?
//
// WHY THIS EXISTS: the composition map can be right while the tile is black.
// A monitor tile draws "the staged slide, else the on-air one" and swaps to the
// engine's distributed frame when the design needs it, so only the app's own
// pixels say what the operator saw. This reads the PNG that SelfTest.grab wrote
// (8-bit greyscale/RGB/RGBA) and counts what is actually on it:
//
//   bright — opaque pixels brighter than 200 luma: the design's TEXT
//   ink    — opaque pixels away from the tile's own backdrop
//   opaque — pixels with any alpha at all (0 = fully transparent tile)
//
// Usage:
//   node tools/selftest/quiet_time_pixels.mjs <png> [--min 40]
//
// VERDICT (last line, greppable):
//   QTPIX: PASS <png> bright=<n> opaque=<n>
//   QTPIX: FAIL blank <png> bright=<n> opaque=<n> (want >= <min>)
//   QTPIX: FAIL read — <reason>
// ============================================================================

import { readFileSync } from 'node:fs';
import { inflateSync } from 'node:zlib';

function arg(name, fallback) {
    const i = process.argv.indexOf(name);
    return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
}

const path = process.argv[2];
const min = Number(arg('--min', '40'));

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
        const prev = y > 0 ? out.subarray((y - 1) * stride, y * stride) : null;
        const cur = out.subarray(y * stride, (y + 1) * stride);
        for (let x = 0; x < stride; ++x) {
            const a = x >= channels ? cur[x - channels] : 0;
            const b = prev ? prev[x] : 0;
            const c = prev && x >= channels ? prev[x - channels] : 0;
            let v = line[x];
            switch (filter) {
            case 0: break;
            case 1: v = v + a; break;
            case 2: v = v + b; break;
            case 3: v = v + ((a + b) >> 1); break;
            case 4: {
                const pp = a + b - c;
                const pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c);
                v = v + (pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
                break;
            }
            default: throw new Error(`filter ${filter}`);
            }
            cur[x] = v & 0xff;
        }
    }
    return { width, height, channels, pixels: out };
}

function report(file, png, minBright) {
    const { width, height, channels, pixels } = png;
    let bright = 0, opaque = 0, ink = 0;
    for (let y = 0; y < height; ++y) {
        for (let x = 0; x < width; ++x) {
            const i = (y * width + x) * channels;
            let r, g, b, a = 255;
            if (channels === 1) { r = g = b = pixels[i]; }
            else if (channels === 2) { r = g = b = pixels[i]; a = pixels[i + 1]; }
            else { r = pixels[i]; g = pixels[i + 1]; b = pixels[i + 2]; if (channels === 4) a = pixels[i + 3]; }
            if (a <= 8) continue;
            ++opaque;
            const luma = 0.299 * r + 0.587 * g + 0.114 * b;
            if (luma > 200) ++bright;
            if (luma > 40) ++ink;
        }
    }
    if (bright < minBright)
        return { ok: false, line: `QTPIX: FAIL blank ${file} bright=${bright} ink=${ink} opaque=${opaque} (want >= ${minBright})` };
    return { ok: true, line: `QTPIX: PASS ${file} bright=${bright} ink=${ink} opaque=${opaque}` };
}

if (!path) {
    console.log('usage: node tools/selftest/quiet_time_pixels.mjs <png> [--min 40]');
    process.exit(2);
}

try {
    const png = decode(path);
    const r = report(path, png, min);
    console.log(`QTPIX: ${path} ${png.width}x${png.height} channels=${png.channels}`);
    console.log(r.line);
    process.exit(r.ok ? 0 : 1);
} catch (e) {
    console.log(`QTPIX: FAIL read — ${path}: ${e.message}`);
    process.exit(2);
}
