#!/usr/bin/env python3
"""Measure and assert the internal padding of Settings section-card screenshots.

WHY THIS EXISTS
---------------
Every card on Settings -> General must wear the SAME 20px internal padding:
the Appearance / Startup cards define it and the plain `SettingsSection`
cards follow. That is a *layout* fact, and layout drift is invisible to
property-level checks - the 2026-10 section cards sat at 14px against the
20px cards above them and nothing noticed. This checker reads the rendered
PIXELS and asserts the padding directly.

HOW IT MEASURES
---------------
For each card PNG it finds the card's interior background colour (the most
common opaque colour) and its border colour (the straight top edge's
midpoint), then the bounding box of every pixel that is neither - the card's
content. The four insets are the distances from the image edges to that box.

The measurement calibrates against itself: the cards are compared to their
own median, so a consistent glyph side-bearing (text ink starts a pixel or
two inside its Text box) cancels out. An absolute floor still guards the
case where EVERY card drifts together (all-padding-shrunk).

The BOTTOM edge is reported but not asserted: the last thing in a card is a
control whose own height differs (a swatch, a chip, a toggle), so the bottom
gap is not comparable card to card.

USAGE
-----
    py tools/selftest/measure_insets.py insets_*.png
    py tools/selftest/measure_insets.py --selftest     # validate the checker

Exit code 0 = every card agrees and clears the floor; 1 = drift.
"""
import argparse
import os
import sys
import tempfile
from statistics import median

try:
    from PIL import Image
except ImportError:  # pragma: no cover - environment guard
    sys.exit("measure_insets: Pillow (PIL) is required (py -m pip install Pillow)")

# The edges whose inset is driven purely by the shared horizontal/top padding.
# (bottom is excluded - see the module docstring.)
EDGES = ("left", "top", "right")

DEFAULT_TOL = 3      # px a card may differ from the median
DEFAULT_FLOOR = 18   # px every measured inset must clear
DEFAULT_MARGIN = 2   # px of the outer edge to ignore (the border + its AA)


def _common_opaque(image):
    """The most common fully-opaque RGB triple (the card's fill)."""
    counts = {}
    px = image.load()
    w, h = image.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 250:
                continue
            key = (r, g, b)
            counts[key] = counts.get(key, 0) + 1
    if not counts:
        return None
    return max(counts, key=counts.get)


def _dist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])


def measure_insets(path, margin=DEFAULT_MARGIN, tol=24):
    """Return (left, top, right, bottom) insets of the content in `path`, or None.

    `tol` is the per-pixel colour tolerance (sum of channel deltas) used to
    classify a pixel as "background" or "border" vs. "content". It is wide
    enough to absorb a divider's antialiased top/bottom blend (a 1px line of
    border colour on the fill) so the dividers are not mistaken for content.
    """
    image = Image.open(path).convert("RGBA")
    w, h = image.size
    if w <= 2 * margin or h <= 2 * margin:
        return None
    bg = _common_opaque(image)
    if bg is None:
        return None
    px = image.load()
    # The border colour: the midpoint of the top edge is the straight part of
    # the frame (rounded corners live at the ends only).
    border = px[w // 2, 0][:3]

    minx, miny, maxx, maxy = w, h, -1, -1
    found = False
    for y in range(margin, h - margin):
        for x in range(margin, w - margin):
            r, g, b, a = px[x, y]
            if a < 250:
                continue
            c = (r, g, b)
            if _dist(c, bg) <= tol or _dist(c, border) <= tol:
                continue
            found = True
            if x < minx:
                minx = x
            if x > maxx:
                maxx = x
            if y < miny:
                miny = y
            if y > maxy:
                maxy = y
    if not found:
        return None
    return (minx, miny, w - 1 - maxx, h - 1 - maxy)


def check_batch(measured, tol=DEFAULT_TOL, floor=DEFAULT_FLOOR):
    """Return a list of human-readable failures for a {name: insets} batch."""
    failures = []
    for edge_idx, edge in enumerate(EDGES):
        values = [v[edge_idx] for v in measured.values()]
        med = median(values)
        if med < floor:
            failures.append(f"{edge}: median {med:.0f} < floor {floor} (every card under-padded?)")
        for name, insets in measured.items():
            if abs(insets[edge_idx] - med) > tol:
                failures.append(f"{name}: {edge} inset {insets[edge_idx]} vs median {med:.0f} (> {tol})")
    return failures


def _synthetic_card(pad, w=400, h=220):
    """A dark card with a 1px border and content markers inset by `pad`.

    Four small markers (not a filled box) keep the fill the most common
    colour, and their bounding box is exactly the content rect - so the
    insets the checker should recover are exactly (pad, pad, pad, pad).
    """
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = im.load()
    card = (22, 23, 30)       # Theme.card (dark)
    border = (35, 37, 48)     # Theme.border (dark)
    ink = (226, 232, 240)     # Theme.textPrimary (dark)
    for y in range(h):
        for x in range(w):
            px[x, y] = (*card, 255)
    for x in range(w):
        px[x, 0] = (*border, 255)
        px[x, h - 1] = (*border, 255)
    for y in range(h):
        px[0, y] = (*border, 255)
        px[w - 1, y] = (*border, 255)

    def marker(x0, y0):
        for yy in range(y0, y0 + 6):
            for xx in range(x0, x0 + 6):
                px[xx, yy] = (*ink, 255)

    marker(pad, pad)                       # top-left
    marker(w - pad - 6, pad)               # top-right
    marker(pad, h - pad - 6)               # bottom-left
    marker(w - pad - 6, h - pad - 6)       # bottom-right
    return im


def _selftest():
    """Validate the measurement itself, then the batch assertion."""
    ok = True
    print("checker selftest: measuring synthetic cards")
    for pad in (14, 20, 24):
        path = None
        try:
            with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
                path = tf.name
            _synthetic_card(pad).save(path)
            got = measure_insets(path)
        finally:
            if path and os.path.exists(path):
                os.unlink(path)
        print(f"  synthetic pad={pad:>2} -> {got}")
        if got is None or any(abs(g - pad) > 1 for g in got):
            ok = False
            print(f"  FAIL: expected ~{pad} on every edge")

    # The assertion must actually catch drift: a 14px card among 20px cards
    # is 6px off, beyond the default tolerance.
    drifted = {"a": (20, 20, 20, 27), "b": (20, 20, 20, 27), "c": (14, 14, 20, 33)}
    failures = check_batch(drifted)
    if not failures:
        ok = False
        print("  FAIL: a 14px card among 20px cards was NOT flagged")
    else:
        print(f"  drift detection OK ({len(failures)} failure(s) raised)")

    # ...and a uniform batch must pass.
    uniform = {"a": (21, 23, 20, 27), "b": (22, 22, 20, 27), "c": (21, 23, 20, 33)}
    if check_batch(uniform):
        ok = False
        print("  FAIL: a uniform batch was flagged:", check_batch(uniform))
    else:
        print("  uniform batch OK")

    print("CHECKER SELFTEST:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main(argv):
    ap = argparse.ArgumentParser(
        description="Measure Settings card screenshot insets and assert they agree.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("images", nargs="*", help="card PNGs (insets_selfTestCard*.png)")
    ap.add_argument("--tol", type=int, default=DEFAULT_TOL,
                    help=f"max px a card may differ from the median (default {DEFAULT_TOL})")
    ap.add_argument("--floor", type=int, default=DEFAULT_FLOOR,
                    help=f"every measured inset must clear this many px (default {DEFAULT_FLOOR})")
    ap.add_argument("--selftest", action="store_true",
                    help="validate the measurement on synthetic cards and exit")
    args = ap.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.images:
        ap.error("no images given (or pass --selftest)")

    measured = {}
    print("measuring cards:")
    for path in args.images:
        if not os.path.exists(path):
            print(f"  MISSING    {path}")
            continue
        try:
            insets = measure_insets(path)
        except Exception as exc:  # noqa: BLE001 - report and continue
            print(f"  ERROR      {path}: {exc}")
            continue
        if insets is None:
            print(f"  UNREADABLE {path} (no content found)")
            continue
        measured[path] = insets
        print(f"  {os.path.basename(path):<40} L{insets[0]:>3}  T{insets[1]:>3}  R{insets[2]:>3}  B{insets[3]:>3}")

    if not measured:
        print("INSETS: FAIL - nothing measurable")
        return 1

    failures = check_batch(measured, args.tol, args.floor)
    if failures:
        print("INSETS: FAIL")
        for f in failures:
            print("  -", f)
        return 1
    print(f"INSETS: PASS - {len(measured)} card(s); left/top/right agree within {args.tol}px "
          f"(floor {args.floor})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
