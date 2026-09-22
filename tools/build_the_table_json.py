"""Build the app's the_table.json straight from the converted .txt sermons.

The Table tab loads this file from <LocalAppData>/bps/the_table.json at
startup (TheTableService -> TheTableLibrary::Open). Feeding it directly
means no in-app import pass at all — no freeze, no PDF parsing; the
converter (tools/vgr_pdf_to_txt.py) already did the heavy lifting.

Schema (TheTableLibrary::SaveLocked):
    { "schema": 1, "books": [ { "id": "Y1965", "name": "1965", "order": N,
        "chapters": [ { "number": 1, "title": "0117 A Paradox",
                        "verses": [ { "number": 1, "text": "2 As a little boy…",
                                      "heading": "0117 A Paradox" }, ... ] } ] } ] }

Chapter title keeps the mmdd-style code (CodeOf): "65_0117_A_Paradox" ->
"0117 A Paradox", so chapters sort in preaching order. Verse numbers are
1..N in reading order; the PRINTED paragraph number stays inside the verse
text (same as a PDF import would produce), so it displays with the text.

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

_NUM = re.compile(r'^(\d{1,3})\s+')


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
        paras = [p.strip() for p in txt.read_text(encoding='utf-8').split('\n\n') if p.strip()]
        if not paras:
            continue
        verses = []
        for i, p in enumerate(paras, 1):
            verses.append({"number": i, "text": p})
        if verses:
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
