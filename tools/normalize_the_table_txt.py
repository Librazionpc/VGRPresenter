"""Normalize The Table's source .txt sermons to clean printed paragraphs.

The PDF converter (vgr_pdf_to_txt.py) emits one block per PDF paragraph, but
page/column breaks split paragraphs: the continuation block carries no printed
number. This script rewrites each file so that

  * the leading title block (matching the file name) is dropped;
  * an unnumbered block is merged (with a space) into the previous paragraph;
  * leading unnumbered blocks become paragraph 1 when the first printed
    number is 2, or fold into the printed 1 when one exists;
  * sentence-leading digits ("1933 was the year...") are never taken for a
    paragraph number: a printed number is small (<= 500) and never runs
    backwards within a sermon;
  * sermons with no printed numbers at all get sequential 1..N.

The result is idempotent: running it twice changes nothing. The app's
the_table.json is rebuilt from these files by build_the_table_json.py, so
both stay in agreement. Run:
    .venv/Scripts/python.exe tools/normalize_the_table_txt.py
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src" / "downloads_vgr_txt"

_NUM = re.compile(r'^(\d{1,3})\s+(.*)$', re.S)


def process_file(txt: Path) -> bool:
    raw = txt.read_text(encoding='utf-8')
    norm = raw.replace('\r\n', '\n')
    blocks = [b.strip() for b in re.split(r'\n\s*\n', norm) if b.strip()]
    if not blocks:
        return False

    # Drop the leading title block only when it is a SHORT line matching the
    # file-name title. A content paragraph can QUOTE the title ("HEBREWS,
    # CHAPTER SEVEN 2 Bless you!...") — without the length guard the whole
    # sermon matched and was dropped (data loss, caught on 1957's Hebrews
    # series). Never drop the only block either.
    stem_norm = re.sub(r'[^a-z0-9]', '', re.sub(r'^\d{2}_', '', txt.stem).lower())
    b0_norm = re.sub(r'[^a-z0-9]', '', blocks[0].lower())
    if (len(blocks) > 1 and len(blocks[0]) <= 120 and b0_norm
            and (b0_norm in stem_norm or stem_norm in b0_norm)):
        blocks = blocks[1:]

    # Merge into printed paragraphs (same rules as build_the_table_json.py).
    # A sermon with NO printed numbers at all (the Q&A Hebrews series) keeps
    # its blocks as sequential paragraphs — merging everything into one block
    # would destroy its structure.
    if not any(_NUM.match(b) for b in blocks):
        paras: list[tuple[int, str]] = [(i, b) for i, b in enumerate(blocks, 1)]
    else:
        paras = []
        last_num = 0
        for b in blocks:
            m = _NUM.match(b)
            if m and 1 <= int(m.group(1)) <= 500 and int(m.group(1)) >= last_num:
                last_num = int(m.group(1))
                paras.append((last_num, m.group(2).strip()))
            elif paras:
                n, t = paras[-1]
                paras[-1] = (n, t + ' ' + b)
            else:
                paras.append((0, b))

        if paras and all(n == 0 for n, _ in paras):
            paras = [(i, t) for i, (n, t) in enumerate(paras, 1)]
        if len(paras) > 1 and paras[0][0] == 0:
            if paras[1][0] == 1:
                paras[1] = (1, paras[0][1] + ' ' + paras[1][1])
                del paras[0]
            else:
                paras[0] = (1, paras[0][1])

    # SAFETY: never write an empty/structure-less result over real content —
    # a bug here must cost the original file, not the library.
    if not paras or not any(t for _, t in paras):
        return False

    out = '\n\n'.join(f"{n} {t}" if n else t for n, t in paras) + '\n'
    if out == raw:
        return False
    txt.write_text(out, encoding='utf-8', newline='\n')
    return True


def main() -> None:
    changed = 0
    files = sorted(SRC.glob('*/*.txt'))
    for txt in files:
        if process_file(txt):
            changed += 1
    print(f"files: {len(files)} · rewritten: {changed}")


if __name__ == "__main__":
    sys.exit(main())
