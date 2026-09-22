"""Convert the complete VGR sermon PDFs (downloads from branham.org) to .txt.

What it produces — one paragraph per line-group, blank-line separated, each
prefixed with its PRINTED paragraph number from the PDF:

    25 If you're sick, I wish I could heal you, but no man can do that.

so the numbers you quote ("paragraph 25") are right there in the .txt and
survive into the app's The Table import (verse text = "25 ...").

How the numbers are found and kept honest:
  * pymupdf BLOCKS: the number is its own line at the start of a body block,
    or the leading token of the first line.
  * A candidate number is only accepted when it continues the sermon's
    monotonic sequence (1, 2, 3, ...; gaps allowed for front-matter) — page
    numbers and running-head digits cannot fake it.
  * The running head (title repeated at the top of every page) and the page's
    own number (a digit line that tracks the page index, at a page edge) are
    dropped. Private-use glyphs (the dove mark) are stripped.

Run with the project venv:
    .venv/Scripts/python.exe tools/vgr_pdf_to_txt.py [--src DIR] [--out DIR] [--workers N]
"""

import argparse
import re
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import pymupdf
from tqdm import tqdm

DEFAULT_SRC = Path(__file__).resolve().parents[1] / "src" / "downloads_vgr"
DEFAULT_OUT = Path(__file__).resolve().parents[1] / "src" / "downloads_vgr_txt"

_HEADER_ZONE = 52.0        # y above this is the running-head zone
_PUA = re.compile(r'[\uf000-\uf8ff]')          # private-use glyphs (dove, etc.)
_NUM_LINE = re.compile(r'^(\d{1,3})$')
_NUM_LEAD = re.compile(r'^(\d{1,3})\s+(\S.*)$')
_TERMINAL = ".!?\u201d\"')\u2026"


def strip_ligs(s: str) -> str:
    s = s.replace('\ufb00', 'ff').replace('\ufb01', 'fi').replace('\ufb02', 'fl')
    s = s.replace('\ufb03', 'ffi').replace('\ufb04', 'ffl')
    return s


def clean_lines(text: str) -> list[str]:
    """Block text -> cleaned lines (whitespace squeezed, PUA removed)."""
    lines = []
    for raw in text.splitlines():
        t = _PUA.sub('', strip_ligs(raw)).strip()
        t = re.sub(r'\s+', ' ', t)
        if t:
            lines.append(t)
    return lines


def _ends_sentence(text: str) -> bool:
    return bool(text) and (text[-1] in _TERMINAL or len(text) < 60)


def _is_caps_chrome(text: str) -> bool:
    """A short unnumbered ALL-CAPS block: cover chrome, not the sermon.
    ('ENGLISH' markers, (c) VGR footers, repeated title fragments.) Real
    paragraphs are numbered or mixed-case; the sermon's title comes from
    the file name in the app, so the title line is no loss either."""
    t = re.sub(r'\s+', ' ', text).strip()
    letters = re.sub(r'[^A-Za-z]', '', t)
    if not letters or len(t) > 70:
        return False
    return letters.isupper()


_BOILERPLATE = re.compile(
    r'^(?:copyright notice|all rights reserved[\s,.]'
    r'|voice of god recordings\s+p\.?o\.?\s*box'
    r'|for more information or for other available material)',
    re.IGNORECASE)


def _is_boilerplate(text: str) -> bool:
    """The front-matter copyright page, once per PDF ('Copyright Notice',
    'All rights reserved. This book may be printed…', the VGR P.O. Box
    address, the contact line). Sermon sentences that merely mention a
    copyright do not match — they start with other words."""
    t = re.sub(r'\s+', ' ', text).strip()
    return bool(_BOILERPLATE.match(t))


_CONTACT = re.compile(
    r'^(?:'
    r'\(\d{3}\)\s*\d{3}[\s.-]?\d{4}'          # the phone number footer
    r'|www\.'                                   # a bare site line
    r'|[A-Z][\w .\'-]*,\s*[A-Z][\w .\'-]*\s+U\.?S\.?A\.?\s*$'   # "Oakland, California U.S.A."
    r'|Branham Tabernacle\b.*U\.?S\.?A\.?\s*$'
    r')',
    re.IGNORECASE)


def _is_contact(text: str) -> bool:
    """The contact/office page: the phone+site footer and the 'City,
    State U.S.A.' office lines. A paragraph that only SPEAKS of a city
    is numbered or longer, so it never matches these exact shapes."""
    t = re.sub(r'\s+', ' ', text).strip()
    return bool(_CONTACT.match(t))


def sermon_paragraphs(pdf_path: Path) -> list[tuple[int, str]]:
    """[(printed number or 0, paragraph text)] in reading order."""
    doc = pymupdf.open(pdf_path)
    try:
        pages = []
        for page in doc:
            blocks = []
            for b in page.get_text("blocks"):
                if b[6] == 0:                      # text block
                    lines = clean_lines(b[4])
                    if lines:
                        blocks.append((b[1], lines))   # (y0, lines)
            blocks.sort(key=lambda t: t[0])
            pages.append(blocks)

        # Running head: the most common LETTERS-ONLY key among each page's
        # TOP blocks, regardless of exact y (layouts differ; 1940s PDFs put
        # the head lower than 1960s ones). The page number often rides ON
        # the head line itself ("4 THE SPOKEN WORD"), so the key ignores
        # every digit — otherwise each page looks like a different string.
        # Books ALTERNATE two heads (verso = sermon title, recto = book
        # title), so no single key reaches half the pages: a quarter is the
        # honest bar. Body text never repeats at the top of many pages.
        def head_key(lines: list[str]) -> str:
            letters = ''.join(ln for ln in lines if re.search(r'[A-Za-z]', ln))
            return re.sub(r'[^A-Za-z ]', ' ', letters).strip().lower()

        head_keys = Counter()
        for blocks in pages:
            # The head may be split into two blocks (a digit block above the
            # title block), so count the top FEW blocks of each page.
            for y0, lines in blocks[:3]:
                if y0 > 90:
                    break
                if head_key(lines):
                    head_keys[head_key(lines)] += 1
        running = {k for k, c in head_keys.items() if c >= max(3, len(pages) // 4)}

        # A short block whose letters extend OR continue a running key is
        # the head too — a part-title page ("THE SPOKEN WORD" alone) or a
        # title wrap fragment ("THE SPOKEN WORD IS" of "...IS THE ORIGINAL
        # SEED").
        def head_like(key: str) -> bool:
            if not key:
                return False
            for r in running:
                if key == r:
                    return True
                if len(key) < 60 and (r.startswith(key + ' ') or key.startswith(r + ' ')):
                    return True
            return False

        def is_pagenum(tok: str, page_index: int, top: bool) -> bool:
            if not tok.isdigit() or len(tok) > 3:
                return False
            return abs(int(tok) - (page_index + 1)) <= 2

        # A block whose letters, minus a leading page digit, are the running
        # head is the head itself ("4 THE SPOKEN WORD") — never content.
        def is_running_block(lines: list[str]) -> bool:
            body = list(lines)
            if body and body[0][:1].isdigit():
                rest = re.sub(r'^\d{1,3}\s*', '', body[0])
                body = [rest] + body[1:] if rest else body[1:]
            return bool(body) and head_like(head_key(body))

        # Pass one: flatten to (page_idx, y0, para_num|0, [lines]) body units.
        units = []
        first_title_kept = False
        for pi, blocks in enumerate(pages):
            for bi, (y0, lines) in enumerate(blocks):
                if is_running_block(lines):
                    # The document's very first block is the sermon's own
                    # title (it equals the running head by construction);
                    # every later repeat is a head or a part-title page.
                    if first_title_kept or pi != 0 or bi != 0:
                        continue
                    first_title_kept = True
                num = 0
                # page number alone on the last line at a page edge
                while lines and len(lines) > 1 and is_pagenum(lines[-1], pi, False) \
                        and (bi == len(blocks) - 1 or y0 > 700):
                    lines.pop()
                if lines and _NUM_LINE.match(lines[0]):
                    if is_running_block(lines[1:]):
                        continue                      # the head, digit included
                    num = int(lines[0]); lines = lines[1:]
                elif lines and (m := _NUM_LEAD.match(lines[0])):
                    if is_running_block([m.group(2)]):
                        continue                      # the head, digit included
                    num = int(m.group(1)); lines = [m.group(2)] + lines[1:]
                if not lines:
                    continue                          # a lone digit: the page's own number
                if not re.search(r'[A-Za-z]', ' '.join(lines)):
                    continue                          # digits/punct only: a code or folio
                units.append([num, ' '.join(lines)])

        # Pass two: keep numbers only when they continue the sermon's own
        # increasing sequence — anything else is a stray page digit.
        out = []
        last = 0
        for num, text in units:
            if num:
                ok = (last == 0 and num <= 3) or (last < num <= last + 60)
                if not ok:
                    num = 0
                else:
                    last = num
            out.append((num, text))

        # Pass three: merge blocks into paragraphs. A numbered unit ALWAYS
        # starts a paragraph; an unnumbered one continues the current
        # paragraph while it lacks terminal punctuation (a short unpunctuated
        # unit is a heading and closes itself).
        paras = []
        cur = cur_num = 0, ""
        started = False

        def flush():
            if started and cur[1]:
                paras.append((cur[0], cur[1]))

        for num, text in out:
            if num:                                   # hard paragraph break
                flush()
                cur, started = (num, text), True
            elif not started:
                cur, started = (0, text), True
            else:
                prev_num, prev_text = cur
                if _ends_sentence(prev_text):
                    flush()
                    cur, started = (0, text), True
                else:
                    cur = (prev_num, f"{prev_text} {text}".strip())
        flush()
        # Strip block-letter chrome: every unnumbered ALL-CAPS short
        # paragraph is a cover artifact (language tag, copyright footer,
        # repeated title fragment), never the preached text — EXCEPT the
        # sermon's own title, kept once: the leading caps lines that match
        # the running-head strings (or their wrap fragments).
        body_started = False
        kept = []
        for n, t in paras:
            if n:
                body_started = True
                kept.append((n, t))
                continue
            if _is_boilerplate(t) or _is_contact(t):
                continue                          # the copyright/contact pages
            if _is_caps_chrome(t):
                if not body_started and head_like(head_key([t])):
                    kept.append((0, t))       # the title (once)
                continue
            body_started = True
            kept.append((0, t))
        return kept
    finally:
        doc.close()


def convert_one(job: tuple[Path, Path]) -> tuple[Path, int, str]:
    src, dst = job
    try:
        paras = sermon_paragraphs(src)
        if not paras:
            return dst, 0, "empty"
        body = []
        for num, text in paras:
            body.append(f"{num} {text}" if num else text)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text("\n\n".join(body) + "\n", encoding="utf-8")
        return dst, len(paras), "ok"
    except Exception as e:  # noqa: BLE001 - report and keep the batch going
        return dst, 0, f"error: {e}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--src", type=Path, default=DEFAULT_SRC)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--workers", type=int, default=6)
    args = parser.parse_args()

    pdfs = sorted(args.src.rglob("*.pdf"))
    print(f"{len(pdfs)} PDFs under {args.src}")
    jobs = [(p, args.out / p.relative_to(args.src).with_suffix(".txt")) for p in pdfs]

    ok = empty = failed = 0
    numbered = 0
    with ProcessPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = [pool.submit(convert_one, j) for j in jobs]
        with tqdm(total=len(futures), desc="Converting", unit="file") as bar:
            for fut in as_completed(futures):
                _, n, status = fut.result()
                if status == "ok":
                    ok += 1
                elif status == "empty":
                    empty += 1
                else:
                    failed += 1
                    print(f"\n{status}")
                bar.update(1)

    print(f"\nconverted {ok} · empty {empty} · failed {failed} -> {args.out}")


if __name__ == "__main__":
    main()
