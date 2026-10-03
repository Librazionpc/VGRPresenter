#!/usr/bin/env python3
# ============================================================================
# tools/glyph_from_svg.py <asset.svg> [--name NAME] [--insert] [--icon-file F]
#
# One command to turn a dropped-in qml/assets SVG into the IconGlyph block the
# icon bank expects, so adding a glyph never means hand-copying path data (the
# thing that goes wrong: a path pasted through a formatter, a stroke-width
# nudged, a path dropped, the 24-grid scale forgotten).
#
# It reads the asset and prints:
#
#   * the Component {...} block, in the bank's own style —
#       - a 24 x 24 viewBox asset becomes a 24-grid StrokeIcon/FillIcon with
#         scale: 0.5833 (14/24), the way template/bible/book/calendar are;
#       - any other size keeps its natural width/height and gets no scale
#         (the hand-sized glyphs: folder, search, chevron...);
#       - fill="currentColor" (or a filled path) picks FillIcon and its
#         svgPath alias; a stroked asset picks StrokeIcon and its ShapePath.
#   * the two registration lines that name it: the grid24 array (only for a
#     24-grid glyph) and the `case` arm of the name switch.
#
# With --insert it performs those edits in qml/components/IconGlyph.qml itself
# (component appended before the file's last brace, grid24/`case` added, all
# guarded against a name that is already there) — the true one command.
#
# HOW TO RUN (this shell has no `python` on PATH — use the launcher):
#     py tools/glyph_from_svg.py qml/assets/overlay.svg
#     py tools/glyph_from_svg.py "qml/assets/media icon.svg" --name media
#     py tools/glyph_from_svg.py qml/assets/newthing.svg --insert
#
# Notes the script applies for you:
#   * Tabler assets open with a sentinel background path
#     (d="M0 0h24v24H0z", stroke="none") — it is DROPPED; keeping it draws a
#     24-box outline behind the glyph.
#   * Every path `d` is emitted VERBATIM (Qt's PathSvg takes Tabler's relative
#     commands, implicit repetition and leading-dot numbers), joined with a
#     space — the same form the existing assets use.
#   * The Tabler class attribute ("icon-tabler-layers-intersect") supplies the
#     icon's real name for the comment when the asset carries one.
# ============================================================================
import argparse
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# The bank's constants — keep in sync with IconGlyph.qml.
GRID = 24
GRID_SCALE = 0.5833          # 14/24, what every 24-grid glyph in the bank uses
DEFAULT_STROKE = 2           # Tabler's stroke-width; the bank hardcodes 2 on the 24 grid
SENTINEL_D = re.compile(r"^\s*M0\s*0h24v24H0z\s*$", re.IGNORECASE)


def localname(tag):
    """'{http://www.w3.org/2000/svg}path' -> 'path'."""
    return tag.rsplit("}", 1)[-1]


def parse_len(value):
    """'24' / '24px' -> 24.0, else None."""
    if value is None:
        return None
    m = re.match(r"^\s*([0-9]*\.?[0-9]+)", value)
    return float(m.group(1)) if m else None


def sanitize(name):
    """An asset stem -> a glyph name: 'bible ' -> 'bible', 'media icon' -> 'mediaicon'."""
    return re.sub(r"[^0-9A-Za-z]", "", name)


def read_asset(path):
    """Pull the drawing facts out of one SVG."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    root = ET.fromstring(text)
    root_attrs = {localname(k): v for k, v in root.attrib.items()}

    # Tabler tags every icon with its own name in `class`.
    tabler = None
    m = re.search(r"icon-tabler-([A-Za-z0-9][A-Za-z0-9-]*)", root_attrs.get("class", ""))
    if m:
        tabler = m.group(1)

    view_box = root_attrs.get("viewBox", "")
    nums = [parse_len(v) for v in re.split(r"[,\s]+", view_box.strip()) if v != ""]
    box = nums if len(nums) == 4 else None

    paths = []
    for el in root.iter():
        if localname(el.tag) != "path":
            continue
        a = {localname(k): v for k, v in el.attrib.items()}
        d = (a.get("d") or "").strip()
        if not d or SENTINEL_D.match(d):
            continue                      # the Tabler background box — never draw it
        paths.append({"d": d, "fill": a.get("fill"), "stroke": a.get("stroke")})

    # Filled when the asset says so on the root, or on any kept path (Tabler's
    # filled variants put fill="currentColor" on the root, Material's on each path).
    filled = root_attrs.get("fill") == "currentColor" or any(
        p["fill"] == "currentColor" for p in paths)

    stroke_w = parse_len(root_attrs.get("stroke-width"))
    if stroke_w is None:
        stroke_w = float(DEFAULT_STROKE)   # the bank hardcodes 2 on the 24 grid anyway

    width = parse_len(root_attrs.get("width"))
    height = parse_len(root_attrs.get("height"))
    if box:
        grid = box[2] == GRID and box[3] == GRID
        w, h = box[2], box[3]
    else:
        grid = width == GRID and height == GRID
        w = width if width is not None else GRID
        h = height if height is not None else GRID

    return {
        "tabler": tabler,
        "grid": grid,
        "filled": filled,
        "paths": paths,
        "stroke_width": stroke_w,
        "w": w,
        "h": h,
    }


def fmt_num(x):
    return str(int(x)) if float(x).is_integer() else str(x)


def component_block(info, name, asset_name):
    cid = sanitize(name) + "C"
    label = 'Tabler "%s"' % info["tabler"] if info["tabler"] else "icon"
    lines = []
    lines.append("    // %s (%s): TODO - one line on what it draws, and where it is" % (label, asset_name))
    lines.append("    // used (which screen/tab wears it).")
    lines.append("    Component {")
    lines.append("        id: %s" % cid)
    kind = "FillIcon" if info["filled"] else "StrokeIcon"
    if info["grid"]:
        lines.append("        %s {" % kind)
        lines.append("            width: 24; height: 24; scale: %s // 14/24, like the other 24-grid glyphs" % GRID_SCALE)
    else:
        lines.append("        %s {" % kind)
        lines.append("            width: %s; height: %s // natural size - NOT on the 24 grid, so no scale" % (
            fmt_num(info["w"]), fmt_num(info["h"])))
    joined = " ".join(p["d"] for p in info["paths"])
    if info["filled"]:
        lines.append('            svgPath: "%s"' % joined)
    else:
        lines.append("            ShapePath {")
        lines.append('                fillColor: "transparent"')
        lines.append("                strokeColor: root.color")
        lines.append("                strokeWidth: %s" % fmt_num(info["stroke_width"]))
        lines.append("                capStyle: ShapePath.RoundCap")
        lines.append("                joinStyle: ShapePath.RoundJoin")
        lines.append('                PathSvg { path: "%s" }' % joined)
        lines.append("            }")
    lines.append("        }")
    lines.append("    }")
    return cid, "\n".join(lines)


def registration_lines(info, name, cid):
    out = ["", "Registration (in qml/components/IconGlyph.qml):"]
    if info["grid"]:
        out.append('  1. grid24 (line ~22): append "%s" to the array' % name)
        out.append('  2. name switch:        case "%s": return %s' % (name, cid))
    else:
        out.append('  1. name switch:        case "%s": return %s' % (name, cid))
        out.append("     (hand-sized: do NOT add it to grid24 - that array is the 24-grid list)")
    out.append('  then use it:           IconGlyph { name: "%s"; width: N; height: N }' % name)
    return "\n".join(out)


def insert_into(icon_file, info, name, cid, block):
    """Append the component and add the grid24/case registration, idempotently."""
    path = Path(icon_file)
    text = path.read_text(encoding="utf-8")

    if re.search(r'\bid:\s*%s\b' % re.escape(cid), text) or re.search(
            r'case\s+"%s"' % re.escape(name), text):
        print("insert: %r already present in %s - nothing written" % (name, icon_file), file=sys.stderr)
        return False

    # 2. the name switch arm, after the last existing `case`.
    arms = list(re.finditer(r'^(\s*)case\s+"[^"]+":\s*return\s+\w+\s*$', text, re.MULTILINE))
    if not arms:
        print("insert: no `case` arms found in %s" % icon_file, file=sys.stderr)
        return False
    last = arms[-1]
    indent = last.group(1)
    arm = '\n%scase "%s": return %s' % (indent, name, cid)
    text = text[:last.end()] + arm + text[last.end():]

    # 1. the grid24 array entry, inside the closest [...] after `grid24`.
    if info["grid"]:
        g = re.search(r"(readonly property bool grid24:\s*\[)([^\]]*)(\])", text)
        if not g:
            print("insert: could not find the grid24 array in %s" % icon_file, file=sys.stderr)
            return False
        body = g.group(2).rstrip()
        text = text[:g.start(2)] + body + ', "%s"' % name + text[g.end(2):]

    # 3. the component block, before the file's final closing brace.
    end = text.rstrip()
    assert end.endswith("}"), "IconGlyph.qml does not end with '}'"
    cut = end.rfind("}")
    text = end[:cut].rstrip("\n") + "\n\n" + block + "\n\n" + end[cut:] + "\n"

    path.write_text(text, encoding="utf-8")
    print("insert: wrote %r to %s (component + registration)" % (name, icon_file), file=sys.stderr)
    return True


def main(argv):
    ap = argparse.ArgumentParser(description="IconGlyph component block from a qml/assets SVG.")
    ap.add_argument("svg", help="path to the SVG asset (qml/assets/*.svg)")
    ap.add_argument("--name", help="glyph name (default: the asset's filename stem, sanitized)")
    ap.add_argument("--insert", action="store_true",
                    help="also write the component + registration into IconGlyph.qml")
    ap.add_argument("--icon-file", default="qml/components/IconGlyph.qml",
                    help="the icon bank to --insert into (default: %(default)s)")
    args = ap.parse_args(argv)

    asset = Path(args.svg)
    if not asset.is_file():
        print("glyph_from_svg.py: no such file: %s" % asset, file=sys.stderr)
        return 2

    info = read_asset(asset)
    if not info["paths"]:
        print("glyph_from_svg.py: %s has no drawable <path> (only the background?)" % asset, file=sys.stderr)
        return 1

    name = sanitize(args.name if args.name else asset.stem)
    if not name:
        print("glyph_from_svg.py: could not derive a glyph name - pass --name", file=sys.stderr)
        return 2

    cid, block = component_block(info, name, asset.name)
    print(block)
    print(registration_lines(info, name, cid))

    if args.insert:
        insert_into(args.icon_file, info, name, cid, block)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
