#!/usr/bin/env python3
"""Convert the sermon-PDF library (src/downloads) into the engine's Bible JSON.

Mapping (user's spec):
  year folder  -> book   ("1953")
  sermon PDF   -> chapter (numbered in filename order within the year)
  paragraph    -> verse   (blank-line-separated blocks; verse 1 = opening)
  sermon title -> verse 1's heading

Output: src/VGR_XML_JSON/the_table.json  (the dev data dir the engine's
boot-time Bible loader scans). Import once, then the engine drives everything:
books/chapters/verses, reference resolution and full-text search — the same
machinery the Scripture tab uses. Run from the repo root:

    python tools/sermons_to_bible.py
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "downloads")
OUT = os.path.join(ROOT, "src", "VGR_XML_JSON", "the_table.json")

# pdftotext (poppler) does the extraction; -layout keeps reading order, -enc
# pins UTF-8. Windows Git Bash ships it in /mingw64/bin.
PDFTOTEXT = "pdftotext"

CHAPTER_RE = re.compile(r"^(\d{2,4})[_ ]")
YEAR_RE = re.compile(r"^(19|20)\d{2}$")


def pdf_paragraphs(path):
    """One PDF -> its paragraphs (blank-line-separated text blocks)."""
    try:
        raw = subprocess.run(
            [PDFTOTEXT, "-layout", "-enc", "UTF-8", path, "-"],
            capture_output=True, check=True, timeout=120).stdout.decode("utf-8", "replace")
    except (subprocess.SubprocessError, OSError) as exc:
        print(f"  !! {os.path.basename(path)}: {exc}", file=sys.stderr)
        return None

    paragraphs = []
    buf = []
    for line in raw.splitlines():
        stripped = line.strip()
        if not stripped:
            if buf:
                paragraphs.append(" ".join(buf))
                buf = []
            continue
        buf.append(stripped)
    if buf:
        paragraphs.append(" ".join(buf))

    # A "paragraph" shorter than ~40 chars is a page artifact (headers,
    # page numbers) — drop it unless it is the only text the PDF gave.
    strong = [p for p in paragraphs if len(p) >= 40]
    return strong if strong else paragraphs


def human_title(filename):
    """53_0217_Only_Believe.pdf -> ('0217', 'Only Believe')."""
    base = os.path.splitext(os.path.basename(filename))[0]
    m = CHAPTER_RE.match(base)
    num = m.group(1) if m else ""
    title = base[m.end():] if m else base
    title = title.replace("_", " ").strip()
    title = re.sub(r"\s+", " ", title)
    return num, title


def main():
    if not os.path.isdir(SRC):
        sys.exit(f"no library at {SRC}")

    years = sorted(
        d for d in os.listdir(SRC)
        if os.path.isdir(os.path.join(SRC, d)) and YEAR_RE.match(d))
    if not years:
        sys.exit("no year folders found")

    books = []
    verses = []
    total_sermons = 0
    failed = 0

    for yi, year in enumerate(years, start=1):
        book_id = f"Y{year.replace('-', '')}"          # "1947-1949" -> Y19471949
        pdfs = sorted(
            f for f in os.listdir(os.path.join(SRC, year))
            if f.lower().endswith(".pdf"))
        books.append({"id": book_id, "name": year, "testament": "table", "order": yi})
        chapter = 0
        for pdf in pdfs:
            num, title = human_title(pdf)
            paragraphs = pdf_paragraphs(os.path.join(SRC, year, pdf))
            if not paragraphs:
                failed += 1
                continue
            chapter += 1
            total_sermons += 1
            label = f"{num} {title}".strip() if num else title
            for vi, text in enumerate(paragraphs, start=1):
                verses.append({
                    "book": book_id,
                    "chapter": chapter,
                    "verse": vi,
                    "text": text,
                    "heading": label if vi == 1 else "",
                })
        print(f"{year}: {chapter} sermons")

    doc = {
        "metadata": {
            "id": "the-table",
            "name": "The Table",
            "abbreviation": "TABLE",
            "language": "en",
            "copyright": "end-time-message.org sermon library",
        },
        "books": books,
        "verses": verses,
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, ensure_ascii=False)
    print(f"\n{len(books)} books, {total_sermons} sermons, {len(verses)} verses"
          f" ({failed} unreadable) -> {OUT}")


if __name__ == "__main__":
    main()
