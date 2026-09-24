"""Build the app's the_table.json straight from the converted .txt sermons.

The Table tab loads this file from <LocalAppData>/bps/the_table.json at
startup (TheTableService -> TheTableLibrary::Open). Feeding it directly
means no in-app import pass at all — no freeze, no PDF parsing; the
converter (tools/vgr_pdf_to_txt.py) already did the heavy lifting.

Schema (TheTableLibrary::SaveLocked):
    { "schema": 1, "books": [ { "id": "Y1965", "name": "1965", "order": N,
        "chapters": [ { "number": 1, "title": "0117 A Paradox",
                        "verses": [ { "number": 2, "text": "As a little boy…",
                                      "heading": "0117 A Paradox" }, ... ] } ] } ] }

Chapter title keeps the mmdd-style code (CodeOf): "65_0117_A_Paradox" ->
"0117 A Paradox", so chapters sort in preaching order. Verse numbers are
the PRINTED paragraph numbers from the PDF (kept by the converter in the
.txt), NOT renumbered 1..N — the app quotes them ("47-0412 - ... 3" must
point at the paragraph the PDF prints as 3). The source's shape:
  * the first block is the sermon title (ALL CAPS) — dropped;
  * a block starting with a number starts that printed paragraph;
  * an UNNUMBERED block is a CONTINUATION of the previous paragraph and is
    merged into it (user-reported: continuations were getting their own
    numbers, shifting every later quote by one);
  * a few sermons print no numbers at all — their blocks stay sequential
    1..N so they remain addressable.

Backup: an existing the_table.json is copied to the_table.json.bak before
writing. Run:  .venv/Scripts/python.exe tools/build_the_table_json.py
"""

import json
import os
import re
import shutil
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src" / "downloads_vgr_txt"
DST = Path(os.environ["LOCALAPPDATA"]) / "bps" / "the_table.json"

_NUM = re.compile(r'^(\d{1,3})\s+(.*)$', re.S)


def code_of(stem: str) -> str:
    """The mmdd-style code: '65_0117_A_Paradox' -> '0117' (like CodeOf)."""
    m = re.match(r'^\d{2}_([0-9]{3,4})_', stem)
    return m.group(1) if m else ""


def title_of(stem: str) -> str:
    """'65_0117_A_Paradox' -> '0117 A Paradox'."""
    stem = re.sub(r'^\d{2}_', '', stem)          # the yy_ prefix
    title = stem.replace('_', ' ').strip()
    return title


def build() -> dict:
    books: dict[str, dict] = {}
    for txt in sorted(SRC.glob('*/*.txt')):
        year = txt.parent.name
        stem = txt.stem
        norm = txt.read_text(encoding='utf-8').replace('\r\n', '\n')
        blocks = [b.strip() for b in re.split(r'\n\s*\n', norm) if b.strip()]
        if not blocks:
            continue

        # The first block is the sermon's title line (the app takes the title
        # from the file name) — drop it only when SHORT and matching the file
        # name (a content paragraph can quote the title; the length guard
        # keeps it, mirroring the normalizer).
        stem_norm = re.sub(r'[^a-z0-9]', '', re.sub(r'^\d{2}_', '', stem).lower())
        b0_norm = re.sub(r'[^a-z0-9]', '', blocks[0].lower())
        if (len(blocks) > 1 and len(blocks[0]) <= 120 and b0_norm
                and (b0_norm in stem_norm or stem_norm in b0_norm)):
            blocks = blocks[1:]

        # Merge into PRINTED paragraphs: a numbered block starts its printed
        # paragraph; an unnumbered block CONTINUES the previous one. Numbers
        # stay exactly as printed (no 1..N renumbering) so citations match
        # the PDF. A sermon with no printed numbers at all keeps its blocks
        # as sequential 1..N (merging would destroy its structure).
        verses: list[dict] = []
        if not any(_NUM.match(b) for b in blocks):
            verses = [{"number": i, "text": b} for i, b in enumerate(blocks, 1)]
        else:
            last_num = 0
            for b in blocks:
                m = _NUM.match(b)
                # A printed paragraph number is small (<= 500) and never runs
                # backwards — a sentence that merely OPENS with digits ("1933
                # was the year…") is a continuation, not paragraph 1933.
                if m and 1 <= int(m.group(1)) <= 500 and int(m.group(1)) >= last_num:
                    last_num = int(m.group(1))
                    verses.append({"number": last_num, "text": m.group(2).strip()})
                elif verses:
                    verses[-1]["text"] += " " + b   # same paragraph, split by a page/column break
                else:
                    # Leading unnumbered block(s) before any printed number.
                    verses.append({"number": 0, "text": b})
            if verses and all(v["number"] == 0 for v in verses):
                for i, v in enumerate(verses, 1):
                    v["number"] = i
        # The sermon's opening remark(s) sit before the first printed number.
        # When the PDF starts counting at 2 (0412 does: its paragraph 1 is the
        # unnumbered opening), the opening IS paragraph 1. When the PDF also
        # prints a 1 (converter artifact), fold the opening into it instead —
        # verse numbers must stay unique to stay addressable.
        if len(verses) > 1 and verses[0]["number"] == 0:
            if verses[1]["number"] == 1:
                verses[1]["text"] = verses[0]["text"] + " " + verses[1]["text"]
                del verses[0]
            else:
                verses[0]["number"] = 1
        if not verses:
            continue
        verses[0]["heading"] = title_of(stem)

        book = books.setdefault(year, {
            "id": f"Y{year}", "name": year, "order": int(year), "chapters": []})
        code = code_of(stem)
        title = title_of(stem)
        book["chapters"].append({
            "number": len(book["chapters"]) + 1,
            "title": title if title.lower().startswith(code.lower()) else f"{code} {title}".strip(),
            "verses": verses,
        })

    ordered = sorted(books.values(), key=lambda b: b["order"])
    # Chapter numbers were assigned per-book in glob order; re-sort titles
    # chronologically and renumber (glob order == name order here, but keep
    # it explicit so future sources sort right too).
    for book in ordered:
        book["chapters"].sort(key=lambda c: c["title"])
        for i, ch in enumerate(book["chapters"], 1):
            ch["number"] = i
    return {"schema": 1, "books": ordered}


def main() -> None:
    doc = build()
    n_books = len(doc["books"])
    n_ch = sum(len(b["chapters"]) for b in doc["books"])
    n_v = sum(len(c["verses"]) for b in doc["books"] for c in b["chapters"])

    DST.parent.mkdir(parents=True, exist_ok=True)
    if DST.exists():
        bak = DST.with_suffix('.json.bak')
        shutil.copy2(DST, bak)
        print(f"backed up old library -> {bak}")
    DST.write_text(json.dumps(doc, ensure_ascii=False), encoding='utf-8')
    print(f"wrote {DST}")
    print(f"books: {n_books} · sermons: {n_ch} · paragraphs: {n_v}")


if __name__ == "__main__":
    sys.exit(main())
