#!/usr/bin/env node
// ============================================================================
// tools/selftest/corner_empty_pixels.mjs — is the caption column EMPTY below
// the caption buttons?
//
// WHY THIS EXISTS: the report "the header tile is overlapping the minimize,
// maximize, close button" is about a picture, and the layout numbers
// (run_header_clear_test.sh's clearance=) only say where the two rects were
// supposed to land. This reads the PIXELS of a whole-window grab (SelfTest.
// grab("", ...) — the content item) and asks the question the eye asks: is
// anything DRAWN in the column under the caption buttons?
//
// The region it samples: from just left of the cluster's left edge to the
// window's right edge, from just below the cluster's bottom edge down through
// the tab row (48 design px). The header's own 1px border is excluded at the
// right and the region stops above the header's bottom border, so the only
// thing that can put pixels in there is a TILE that strayed into the column —
// which is exactly the defect (GO LIVE used to end 4 design units from the
// cluster's edge, so its red pill sat in this region).
//
// VERDICT (last line, greppable):
//   CORNERPIX: PASS <png> scale=<s> region=<w>x<h> ink=<n> top=#rrggbb
//   CORNERPIX: FAIL tile-in-column <png> ... ink=<n> top=#rrggbb
//   CORNERPIX: FAIL read — <reason>
//
// Usage:
//   node tools/selftest/corner_empty_pixels.mjs <png> <winW> <winH> \
//        <clusterX> <clusterY> <clusterW> <clusterH> <uiScale> [--max 40] [--bg R,G,B]
// ============================================================================

import { readFileSync } from 'node:fs';
import { inflateSync } from 'node:zlib';

const argv = process.argv.slice(2);
const nums = argv.slice(0, 8).map(Number);
const [png, winW, winH, clusterX, clusterY, clusterW, clusterH, uiScale] = nums;
const maxInk = Number((argv[argv.indexOf('--max') + 1]) || 40);
const bgArg = argv.indexOf('--bg');
const bg = bgArg >= 0 ? argv[bgArg + 1].split(',').map(Number) : null;

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
    const img = decode(png);
    if (!winW || !winH)
        throw new Error('window size is required');
    const scale = img.width / winW;
    // The region: the caption column, one row below the buttons, stopping
    // 4 design px short of the right edge (the header's own border) and 4
    // above the header's bottom border.
    const x0 = Math.round((clusterX - 6 * uiScale) * scale);
    const x1 = Math.round((winW - 4 * uiScale) * scale);
    const y0 = Math.round((clusterY + clusterH + 1) * scale);
    const y1 = Math.round((clusterY + clusterH + 48 * uiScale) * scale);
    if (x1 <= x0 || y1 <= y0)
        throw new Error(`empty region (${x0},${y0})-(${x1},${y1})`);
    const base = bg ?? (() => {
        // Most common colour in the region's leftmost column (header ground).
        const counts = new Map();
        for (let y = y0; y < y1; ++y) {
            const i = (y * img.width + x0) * img.channels;
            const key = (img.pixels[i] << 16) | (img.pixels[i + 1] << 8) | img.pixels[i + 2];
            counts.set(key, (counts.get(key) ?? 0) + 1);
        }
        let top = 0, best = -1;
        for (const [k, n] of counts)
            if (n > best) { best = n; top = k; }
        return [(top >> 16) & 255, (top >> 8) & 255, top & 255];
    })();
    let ink = 0;
    const inks = new Map();
    for (let y = y0; y < y1; ++y) {
        for (let x = x0; x < x1; ++x) {
            const i = (y * img.width + x) * img.channels;
            const r = img.pixels[i], g = img.pixels[i + 1], b = img.pixels[i + 2];
            if (Math.abs(r - bg[0]) > 24 || Math.abs(g - bg[1]) > 24 || Math.abs(b - bg[2]) > 24) {
                ++ink;
                const key = (r << 16) | (g << 8) | b;
                inks.set(key, (inks.get(key) ?? 0) + 1);
            }
        }
    }
    let top = 0, best = -1;
    for (const [k, n] of inks)
        if (n > best) { best = n; top = k; }
    const hex = '#' + top.toString(16).padStart(6, '0');
    const head = `${png} ${img.width}x${img.height} scale=${scale.toFixed(3)}`
               + ` region=${x1 - x0}x${y1 - y0} bg=#${bg.map((v) => v.toString(16).padStart(2, '0')).join('')}`
               + ` ink=${ink} top=${hex}`;
    if (ink <= maxInk) {
        console.log(`CORNERPIX: PASS ${head}`);
        process.exit(0);
    }
    console.log(`CORNERPIX: FAIL tile-in-column ${head} (want <= ${maxInk})`);
    process.exit(1);
} catch (err) {
    console.log(`CORNERPIX: FAIL read — ${err.message}`);
    process.exit(2);
}
