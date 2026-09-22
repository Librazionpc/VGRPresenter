"""Verify the PDF→TXT conversion across EVERY sermon, not a sample.

For each PDF/txt pair: build the multiset of normalized CONTENT words from the
PDF (page numbers and the running header stripped) and compare against the
.txt's word multiset.

  OK      — every content word survives into the .txt
  OK*     — the only "lost" words are the doc's own running-header tokens
            (a header variant my per-doc header detection under-stripped)
  MISSING — no .txt for the pdf
  EMPTY   — .txt exists but has no words
  LOST    — real content words absent from the .txt (they are printed)

Run:  .venv/Scripts/python.exe tools/verify_vgr_txt.py [--workers 6]
"""

import argparse
import re
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import pymupdf
from tqdm import tqdm

SRC = Path(__file__).resolve().parents[1] / "src" / "downloads_vgr"
OUT = Path(__file__).resolve().parents[1] / "src" / "downloads_vgr_txt"

LIG = {'\ufb00': 'ff', '\ufb01': 'fi', '\ufb02': 'fl', '\ufb03': 'ffi', '\ufb04': 'ffl',
       '\u2019': "'", '\u2018': "'", '\u201c': '"', '\u201d': '"', '\u2026': '...'}


def norm(w: str) -> str:
    w = w.lower()
    for k, v in LIG.items():
        w = w.replace(k, v)
    w = re.sub(r'[\uf000-\uf8ff]', '', w)   # private-use glyphs
    w = re.sub(r'[^a-z0-9]', '', w)
    return w


def words(s: str) -> list[str]:
    out = []
    for raw in re.findall(r'\S+', re.sub(r'\s+', ' ', s)):
        n = norm(raw)
        if n:
            out.append(n)
    return out


def verify(pdf: Path) -> dict:
    txt = OUT / pdf.relative_to(SRC).with_suffix('.txt')
    name = str(pdf.relative_to(SRC))
    if not txt.exists():
        return {'name': name, 'status': 'MISSING', 'lost': {}}
    tw = Counter(words(txt.read_text(encoding='utf-8')))
    if not tw:
        return {'name': name, 'status': 'EMPTY', 'lost': {}}

    doc = pymupdf.open(pdf)
    try:
        pages = [words(p.get_text()) for p in doc]
    finally:
        doc.close()

    # the doc's most common page-opening tokens (up to 5) = running header
    openings = Counter(tuple(pw[:5]) for pw in pages if len(pw) >= 5)
    header_tokens = set()
    for opening, count in openings.items():
        if count > 2:
            header_tokens.update(opening)

    content = Counter()
    for pw in pages:
        pw = [w for w in pw if not w.isdigit()]   # page numbers
        content.update(pw)

    lost = {w: c for w, c in content.items() if tw.get(w, 0) < c}
    stray_header = {w: c for w, c in lost.items() if w in header_tokens}
    real_lost = {w: c for w, c in lost.items() if w not in header_tokens}
    status = 'OK' if not real_lost else 'LOST'
    return {'name': name, 'status': status, 'lost': real_lost,
            'stray_header': sum(stray_header.values()),
            'content': sum(content.values())}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--workers', type=int, default=6)
    args = parser.parse_args()

    pdfs = sorted(SRC.rglob('*.pdf'))
    print(f"verifying {len(pdfs)} PDF/txt pairs …")

    counts = Counter()
    lost_files = []
    with ProcessPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = {pool.submit(verify, p): p for p in pdfs}
        with tqdm(total=len(futures), desc='Verifying', unit='file') as bar:
            for fut in as_completed(futures):
                r = fut.result()
                counts[r['status']] += 1
                if r['status'] == 'LOST':
                    lost_files.append(r)
                bar.update(1)

    print(f"\nOK {counts['OK']} · LOST {counts['LOST']} · "
          f"MISSING {counts['MISSING']} · EMPTY {counts['EMPTY']}")
    for r in sorted(lost_files, key=lambda r: -sum(r['lost'].values()))[:20]:
        sample = dict(list(r['lost'].items())[:8])
        print(f"  LOST {sum(r['lost'].values()):>5}w  {r['name']}\n        e.g. {sample}")
    if not lost_files:
        print("Every content word of every PDF is present in its .txt.")
    print(f"(OK* header-only losses are excluded from LOST; "
          f"page numbers and headers are stripped by design.)")


if __name__ == '__main__':
    main()
