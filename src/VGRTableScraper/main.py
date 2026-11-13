#!/usr/bin/env python3
"""VGR sermon source — text from table.branham.org, audio from branham.org.

A SEPARATE SOURCE from ``src/VGRScrapper`` (which pulls end-time-message.org PDFs). Nothing in
here touches the app's live ``%LOCALAPPDATA%/bps/the_table.json``: every byte lands under
``./data`` so the result can be reviewed before anything is wired into the app.

Two halves, two hosts:

* **books** — ``table.branham.org`` is an AngularJS page over a JSON REST API. One index call
  lists every sermon, one call per sermon returns the full text as HTML sections. The
  paragraph numbers come back printed inside the markup (``<span class="pn">12</span>``), so
  they are exact rather than re-derived from a PDF.
* **audio** — ``table.branham.org`` has no audio at all; the files live on ``branham.org``,
  behind the same year query its Message Audio page uses. Rows pair the sermon code with the
  PDF and M4A links to CloudFront.

Layout produced (all gitignored):

    data/books/index.json           the index as the site served it + a normalised list
    data/books/books.csv            code, year, title, minutes, paragraphs, chars, file
    data/books/table.json           schema-1 library (TheTableLibrary shape), for review
    data/books/txt/<year>/<code>_<Title>.txt    one sermon, numbered paragraphs
    data/books/raw/<code>.json      the untouched API reply (--keep-raw only)
    data/audio/<year>/<code>_<Title>.m4a        one sermon
    data/audio/manifest.csv         code, year, title, minutes, bytes, url, file

Run (project venv):

    .venv/Scripts/python.exe src/VGRTableScraper/main.py index
    .venv/Scripts/python.exe src/VGRTableScraper/main.py books  --limit 3
    .venv/Scripts/python.exe src/VGRTableScraper/main.py audio  --years 65 --limit 3
    .venv/Scripts/python.exe src/VGRTableScraper/main.py all

Every step resumes: a file that already exists with content is skipped unless ``--force``.
Rough size of the whole library: ~1,218 sermons of text (a few hundred MB if --keep-raw) and
~1,206 M4A files — measured at ~252 KB per minute of audio, about 27 GB in total.
"""

from __future__ import annotations

import argparse
import csv
import html
import json
import logging
import re
import signal
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path

import requests
from bs4 import BeautifulSoup
from requests.adapters import HTTPAdapter
from tqdm import tqdm
from urllib3.util.retry import Retry

############################################################
# CONFIG
############################################################

TABLE_BASE = "https://table.branham.org"
TABLE_INDEX_URL = "/rest/index/allSermons"
TABLE_SERMON_URL = "/rest/sermons/sermonRequest"

AUDIO_BASE = "https://branham.org"
AUDIO_PAGE_URL = AUDIO_BASE + "/en/MessageAudio"
AUDIO_YEAR_URL = AUDIO_BASE + "/branham/messageaudio.aspx/wmSearchByYear"

DATA_DIR = Path(__file__).resolve().parent / "data"
BOOKS_DIR = DATA_DIR / "books"
AUDIO_DIR = DATA_DIR / "audio"

REQUEST_TIMEOUT = 60
DEFAULT_DELAY = 0.4          # seconds between API calls / downloads
DEFAULT_WORKERS = 2

USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
              "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0 Safari/537.36")

logger = logging.getLogger("vgr_table")

STOP = threading.Event()
_MANIFEST_LOCK = threading.Lock()
_thread_state = threading.local()


############################################################
# SESSIONS (each host demands its own anti-CSRF headers)
############################################################


def _session(origin: str, page: str) -> requests.Session:
    """A session that passes the host's cross-site check and retries safely.

    Both hosts answer a bare POST with HTTP 400 'Cross-site request validation failed':
    they want Origin, Referer and X-Requested-With, exactly like their own JavaScript sends.
    """
    session = requests.Session()
    session.headers.update({
        "User-Agent": USER_AGENT,
        "Origin": origin,
        "Referer": page,
        "X-Requested-With": "XMLHttpRequest",
        "Content-Type": "application/json; charset=utf-8",
        "Accept": "application/json, text/javascript, */*; q=0.01",
    })
    retry = Retry(total=3, backoff_factor=1.0, status_forcelist=(429, 500, 502, 503, 504),
                  allowed_methods=frozenset(("GET", "POST")), raise_on_status=False)
    adapter = HTTPAdapter(max_retries=retry, pool_maxsize=8)
    session.mount("https://", adapter)
    return session


def table_session() -> requests.Session:
    """One session per thread (requests.Session is not shared-safe under a pool)."""
    session = getattr(_thread_state, "table", None)
    if session is None:
        session = _session(TABLE_BASE, TABLE_BASE + "/")
        _thread_state.table = session
    return session


def audio_session() -> requests.Session:
    session = getattr(_thread_state, "audio", None)
    if session is None:
        session = _session(AUDIO_BASE, AUDIO_PAGE_URL)
        _thread_state.audio = session
    return session


############################################################
# SMALL HELPERS
############################################################


def slug(text: str) -> str:
    """'Wedding Ceremony' -> 'Wedding_Ceremony' (the folder-import naming the app expects)."""
    text = html.unescape(text or "")
    text = re.sub(r"[^\w\s-]", "", text, flags=re.UNICODE)
    text = re.sub(r"[\s-]+", "_", text).strip("_")
    return text or "Untitled"


def clean_text(text: str) -> str:
    """The site's spans carry their own spacing; collapse runs, drop the decorative glyphs."""
    text = (text.replace("\u00a0", " ")
                .replace("\u200b", "")
                .replace("\uf6e1", " ")   # the 'eagle' marker that stands in for paragraph 1
                .replace("\uf0e0", " "))
    return re.sub(r"\s+", " ", text).strip()


def year_of(code: str) -> str:
    """'65-0116X' -> '1965'; anything unexpected -> 'Unfiled'."""
    head = code.split("-", 1)[0]
    return f"19{head}" if head.isdigit() and len(head) == 2 else "Unfiled"


def chapter_title(code: str, title: str) -> str:
    """'65-0117' + 'A Paradox' -> '0117 A Paradox' (the app's mmdd-style chapter title)."""
    part = code.split("-", 1)[1] if "-" in code else ""
    digits = re.match(r"(\d{3,4})", part)
    return f"{digits.group(1)} {title}".strip() if digits else title


def year_values(spec: str | None) -> list[str]:
    """'47-65' -> ['47'..'65']; '65,64' -> ['65','64']; None -> every year the site lists."""
    if not spec:
        return [f"{y}" for y in range(47, 66)]
    out: list[str] = []
    for chunk in spec.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        if "-" in chunk:
            lo, hi = (p.strip() for p in chunk.split("-", 1))
            if lo.isdigit() and hi.isdigit():
                out.extend(f"{y}" for y in range(int(lo), int(hi) + 1))
                continue
        out.append(chunk)
    return out


############################################################
# TEXT (table.branham.org REST API)
############################################################


def table_post(session: requests.Session, path: str, body: dict) -> dict:
    """POST one API call and insist on Status=Successful before returning the payload."""
    response = session.post(TABLE_BASE + path, json=body, timeout=REQUEST_TIMEOUT)
    response.raise_for_status()
    payload = response.json()
    if str(payload.get("Status", "")).lower() != "successful":
        reason = payload.get("ErrorReason") or {}
        raise RuntimeError(f"{path}: {reason.get('FriendlyErrorMessage') or 'failed'} "
                           f"({(reason.get('TechnicalErrorMessage') or '').splitlines()[0][:160]})")
    return payload


def fetch_index(language: str) -> list[dict]:
    """Every sermon the site knows: identity id, date code, title, minutes."""
    payload = table_post(table_session(), TABLE_INDEX_URL, {"Language": language})
    rows = (payload.get("Result") or {}).get("Sermons") or []
    entries = []
    for row in rows:
        code = str(row.get("p") or "").strip()
        if not code:
            continue
        entries.append({
            "identity": row.get("i"),
            "code": code,
            "title": str(row.get("t") or "").strip(),
            "minutes": row.get("m"),
            "content_type": row.get("ct"),
            "year": year_of(code),
        })
    return entries


def fetch_sermon(identity, language: str) -> dict:
    """One sermon's full text.  SermonProductIdentityId is the index's 'i'; SermonRecordId is
    accepted by the endpoint but resolves to 'Cannot found sermon by filter' for most codes."""
    payload = table_post(table_session(), TABLE_SERMON_URL, {
        "Language": language,
        "SermonProductIdentityId": identity,
        "GetAllContent": True,
    })
    return payload.get("Result") or {}


def paragraphs_of(result: dict) -> list[tuple[int, str]]:
    """[(printed number, text)] in reading order.

    The reply is cut into page-sized Sections, and the markup uses THREE kinds of <p>:

    * ``class="first_par"`` — the sermon's paragraph 1 (no number: a decorative eagle glyph
      stands in for it);
    * ``class="normal_pn"`` — a new printed paragraph, its number in ``<span class="pn">N</span>``;
    * ``class="normal"`` alone — a CONTINUATION of the paragraph above.  The web edition
      breaks one printed paragraph across several <p>s, so numbering these separately invented
      paragraphs that do not exist in the paper edition and then lost their text when the
      schema deduplicated the invented numbers.

    Keeping the PRINTED numbers (never renumbering 1..N) is what makes a citation match the
    book: ``47-0412 … 3`` has to land on the paragraph printed as 3.

    A few short sermons print NO numbers at all (e.g. 47-1100X 'Fellowship', 20 <p>s and not a
    single ``pn``). For those the paper has no numbers to honour, so — exactly like the app's
    PDF importer does for unnumbered sermons — each <p> becomes its own paragraph numbered
    1..N, which keeps it addressable instead of collapsing into one unquotable verse.
    """
    collected: list[tuple[set[str], int | None, str]] = []
    has_numbers = False
    for section in result.get("Sections") or []:
        soup = BeautifulSoup(section.get("Content") or "", "html.parser")
        for block in soup.find_all("p"):
            classes = set(block.get("class") or [])
            number: int | None = None
            marker = block.find("span", class_="pn")
            if marker is not None:
                digits = marker.get_text(strip=True)
                if digits.isdigit():
                    number = int(digits)
                    has_numbers = True
                marker.extract()
            for glyph in block.find_all("span", class_="eagle"):
                glyph.extract()
            text = clean_text(block.get_text(" ", strip=True))
            if text:
                collected.append((classes, number, text))

    sequential = not has_numbers          # unnumbered sermon: one paragraph per <p>, 1..N
    paragraphs: list[list] = []           # [number, text]
    index_of: dict[int, int] = {}         # printed number -> position (merges true repeats)
    expected = 1
    for classes, number, text in collected:
        starts_paragraph = sequential or number is not None or bool(
            classes & {"first_par", "normal_pn"})

        if not starts_paragraph and paragraphs:
            last = paragraphs[-1]
            last[1] = clean_text(f"{last[1]} {text}")
            continue

        if number is None:
            number = expected
        if number in index_of:
            # the source repeated a number: keep the words by merging, never drop them
            at = index_of[number]
            paragraphs[at][1] = clean_text(f"{paragraphs[at][1]} {text}")
            continue
        index_of[number] = len(paragraphs)
        paragraphs.append([number, text])
        expected = max(expected, number + 1)
    return [(int(number), str(text)) for number, text in paragraphs]


def write_sermon_text(target: Path, result: dict, paragraphs: list[tuple[int, str]]) -> None:
    """The .txt the folder import already parses: a title block, then one numbered block per
    paragraph, separated by a blank line (unnumbered continuations are already merged here)."""
    code = str(result.get("DateCode") or "").strip()
    title = str(result.get("Title") or "").strip()
    body = [f"{code}  {title}", ""]
    for number, text in paragraphs:
        body.append(f"{number} {text}")
        body.append("")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("\n".join(body).rstrip() + "\n", encoding="utf-8")


def build_table_json(sermons: list[dict]) -> dict:
    """Schema 1 — the shape TheTableLibrary::Open reads (see tools/build_the_table_json.py).

    book = year (id 'Y1965'), chapter = sermon ('0117 A Paradox'), verse number = the printed
    paragraph number, verse heading = the chapter title.
    """
    books: dict[str, dict] = {}
    for sermon in sorted(sermons, key=lambda s: (s["year"], s["code"])):
        year = sermon["year"]
        book = books.setdefault(year, {
            "id": f"Y{year}" if year.isdigit() else year,
            "name": year,
            "order": 0,
            "chapters": [],
        })
        heading = chapter_title(sermon["code"], sermon["title"])
        seen: set[int] = set()
        verses = []
        for number, text in sermon["paragraphs"]:
            if number in seen:
                logger.warning("%s: duplicate paragraph number %s dropped", sermon["code"], number)
                continue
            seen.add(number)
            verses.append({"number": number, "text": text, "heading": heading})
        book["chapters"].append({
            "number": len(book["chapters"]) + 1,
            "title": heading,
            "verses": verses,
        })
    ordered = []
    for order, year in enumerate(sorted(books), start=1):
        books[year]["order"] = order
        ordered.append(books[year])
    return {"schema": 1, "books": ordered}


def run_books(args: argparse.Namespace) -> None:
    BOOKS_DIR.mkdir(parents=True, exist_ok=True)
    entries = fetch_index(args.language)
    logger.info("index: %d sermons (%s)", len(entries), args.language)
    (BOOKS_DIR / "index.json").write_text(
        json.dumps({"language": args.language, "fetched": time.strftime("%Y-%m-%dT%H:%M:%S"),
                    "sermons": entries}, indent=1, ensure_ascii=False),
        encoding="utf-8")

    if args.limit:
        entries = entries[:args.limit]

    txt_root = BOOKS_DIR / "txt"
    raw_root = BOOKS_DIR / "raw"
    done: list[dict] = []
    failures: list[str] = []
    counters = {"written": 0, "skipped": 0, "failed": 0}

    def one(entry: dict) -> dict | None:
        if STOP.is_set():
            return None
        path = txt_root / entry["year"] / f"{entry['code'].replace('-', '_')}_{slug(entry['title'])}.txt"
        if path.exists() and path.stat().st_size > 0 and not args.force:
            return {"entry": entry, "path": path, "paragraphs": None, "skipped": True}
        result = fetch_sermon(entry["identity"], args.language)
        paragraphs = paragraphs_of(result)
        if not paragraphs:
            raise RuntimeError("no paragraphs in the reply")
        write_sermon_text(path, result, paragraphs)
        if args.keep_raw:
            raw_root.mkdir(parents=True, exist_ok=True)
            (raw_root / f"{entry['code']}.json").write_text(
                json.dumps(result, ensure_ascii=False), encoding="utf-8")
        time.sleep(args.delay)
        return {"entry": entry, "path": path, "paragraphs": paragraphs, "skipped": False}

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(one, entry): entry for entry in entries}
        with tqdm(total=len(futures), desc="Books", unit="sermon") as progress:
            for future in as_completed(futures):
                entry = futures[future]
                progress.update(1)
                try:
                    outcome = future.result()
                except Exception as exc:  # noqa: BLE001 - one bad sermon must not end the run
                    counters["failed"] += 1
                    failures.append(f"{entry['code']}\t{entry['title']}\t{exc}")
                    logger.error("FAILED %s %s: %s", entry["code"], entry["title"], exc)
                    continue
                if outcome is None:
                    continue
                if outcome["skipped"]:
                    counters["skipped"] += 1
                else:
                    counters["written"] += 1
                done.append({
                    "code": entry["code"],
                    "year": entry["year"],
                    "title": entry["title"],
                    "minutes": entry["minutes"],
                    "paragraphs": (len(outcome["paragraphs"])
                                   if outcome["paragraphs"] is not None else ""),
                    "file": str(outcome["path"].relative_to(BOOKS_DIR)).replace("\\", "/"),
                })

    # CSV manifest: only the rows we can describe fully (fresh or skipped-with-text).
    with open(BOOKS_DIR / "books.csv", "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=["code", "year", "title", "minutes",
                                                    "paragraphs", "file"])
        writer.writeheader()
        for row in sorted(done, key=lambda r: r["code"]):
            writer.writerow(row)

    # The schema-1 library is built from the .txt files on disk, so a resumed run still emits
    # a complete table.json instead of only what this invocation fetched.
    sermons = []
    for txt in sorted(txt_root.glob("*/*.txt")):
        try:
            sermons.append({"code": _code_of_path(txt), "title": _title_of_path(txt),
                            "year": txt.parent.name, "paragraphs": _paragraphs_of_txt(txt)})
        except Exception as exc:  # noqa: BLE001
            logger.warning("table.json: skipping %s (%s)", txt.name, exc)
    if sermons:
        (BOOKS_DIR / "table.json").write_text(
            json.dumps(build_table_json(sermons), ensure_ascii=False), encoding="utf-8")
        logger.info("table.json: %d chapters over %d year(s)",
                    sum(len(b["chapters"]) for b in build_table_json(sermons)["books"]),
                    len({s["year"] for s in sermons}))

    if failures:
        (BOOKS_DIR / "failures.txt").write_text("\n".join(failures) + "\n", encoding="utf-8")
    logger.info("books: %d written, %d already present, %d failed",
                counters["written"], counters["skipped"], counters["failed"])


TXT_NAME = re.compile(r"^(\d{2})_(\d{3,4}[A-Za-z]?)_(.*)$")


def _code_of_path(path: Path) -> str:
    match = TXT_NAME.match(path.stem)
    if not match:
        raise ValueError("unrecognised file name")
    return f"{match.group(1)}-{match.group(2)}"


def _title_of_path(path: Path) -> str:
    match = TXT_NAME.match(path.stem)
    return match.group(3).replace("_", " ") if match else path.stem


def _paragraphs_of_txt(path: Path) -> list[tuple[int, str]]:
    """Re-read a .txt this script wrote: the first block is the title, then 'N text' blocks."""
    blocks = [b.strip() for b in path.read_text(encoding="utf-8").split("\n\n") if b.strip()]
    paragraphs: list[tuple[int, str]] = []
    for block in blocks[1:]:
        match = re.match(r"^(\d{1,3})\s+(.*)$", block, re.S)
        if match:
            paragraphs.append((int(match.group(1)), clean_text(match.group(2))))
    return paragraphs


############################################################
# AUDIO (branham.org Message Audio)
############################################################


@dataclass
class AudioRow:
    code: str
    year: str
    title: str
    location: str
    minutes: int | None
    url: str

    @property
    def file_name(self) -> str:
        return f"{self.code.replace('-', '_')}_{slug(self.title)}.m4a"


def parse_audio_rows(results_html: str) -> list[AudioRow]:
    """One .messagebox block per sermon: code, title, place, minutes, and its M4A link."""
    rows: list[AudioRow] = []
    for block in results_html.split('class="messagebox"')[1:]:
        code_match = re.search(r'class="prodtext">\s*(?:<span[^>]*>)?\s*'
                               r'(\d{2}-\d{4}[A-Za-z]?)', block)
        url_match = re.search(r'href="(https://[^"]+\.m4a)"', block)
        if not (code_match and url_match):
            continue
        title_match = re.search(r'class="prodtexttitle">\s*([^<]+?)\s*<', block)
        values = [html.unescape(re.sub(r"<[^>]+>", "", v)).strip()
                  for v in re.findall(r'class="prodtext2">(.*?)</div>', block, re.S)]
        minutes = next((int(re.match(r"(\d+)\s*min", v).group(1))
                        for v in values if re.match(r"\d+\s*min", v)), None)
        location = next((v for v in values if v and not re.match(r"\d+\s*min", v)), "")
        code = html.unescape(code_match.group(1)).strip()
        rows.append(AudioRow(
            code=code,
            year=year_of(code),
            title=clean_text(title_match.group(1)) if title_match else "",
            location=location,
            minutes=minutes,
            url=html.unescape(url_match.group(1)),
        ))
    return rows


def fetch_audio_year(value: str) -> list[AudioRow]:
    response = audio_session().post(AUDIO_YEAR_URL,
                                    json={"formVars": [{"name": "year", "value": value}]},
                                    timeout=REQUEST_TIMEOUT)
    response.raise_for_status()
    blocks = response.json().get("d") or []
    results_html = blocks[0] if blocks and isinstance(blocks[0], str) else ""
    rows = parse_audio_rows(results_html)
    logger.info("audio %s: %d file(s)", value, len(rows))
    return rows


def save_manifest(row: AudioRow, target: Path) -> None:
    with _MANIFEST_LOCK:
        handle = AUDIO_DIR / "manifest.csv"
        new = not handle.exists()
        with open(handle, "a", newline="", encoding="utf-8") as handle:
            writer = csv.writer(handle)
            if new:
                writer.writerow(["code", "year", "title", "location", "minutes",
                                 "bytes", "url", "file"])
            writer.writerow([row.code, row.year, row.title, row.location, row.minutes or "",
                             target.stat().st_size,
                             row.url, str(target.relative_to(AUDIO_DIR)).replace("\\", "/")])


def download_audio(row: AudioRow) -> str:
    if STOP.is_set():
        return "stopped"
    target = AUDIO_DIR / row.year / row.file_name
    if target.exists() and target.stat().st_size > 0 and not _FORCE:
        return "skipped"
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_suffix(target.suffix + ".part")
    try:
        response = audio_session().get(row.url, stream=True, timeout=REQUEST_TIMEOUT)
        response.raise_for_status()
        with open(partial, "wb") as handle:
            for chunk in response.iter_content(1 << 17):
                if STOP.is_set():
                    raise KeyboardInterrupt
                if chunk:
                    handle.write(chunk)
        partial.replace(target)
        save_manifest(row, target)
        return "downloaded"
    except KeyboardInterrupt:
        partial.unlink(missing_ok=True)
        return "stopped"
    except Exception as exc:  # noqa: BLE001 - log and keep the batch going
        partial.unlink(missing_ok=True)
        logger.error("FAILED audio %s %s: %s", row.code, row.title, exc)
        return "failed"


_FORCE = False


def run_audio(args: argparse.Namespace) -> None:
    global _FORCE
    _FORCE = args.force
    AUDIO_DIR.mkdir(parents=True, exist_ok=True)
    rows: list[AudioRow] = []
    for value in year_values(args.years):
        if STOP.is_set():
            break
        try:
            rows.extend(fetch_audio_year(value))
        except Exception as exc:  # noqa: BLE001
            logger.error("audio year %s failed: %s", value, exc)
        time.sleep(args.delay)
    if args.limit:
        rows = rows[:args.limit]
    if not rows:
        logger.warning("audio: nothing to do")
        return

    counters = {"downloaded": 0, "skipped": 0, "failed": 0, "stopped": 0}
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(download_audio, row) for row in rows]
        with tqdm(total=len(futures), desc="Audio", unit="file") as progress:
            for future in as_completed(futures):
                counters[future.result()] += 1
                progress.update(1)
    logger.info("audio: %d downloaded, %d already present, %d failed, %d stopped",
                counters["downloaded"], counters["skipped"], counters["failed"],
                counters["stopped"])


############################################################
# CLI
############################################################


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="VGR sermon source: text from table.branham.org, audio from branham.org.")
    parser.add_argument("-v", "--verbose", action="store_true", help="debug logging")
    sub = parser.add_subparsers(dest="command", required=True)

    index = sub.add_parser("index", help="fetch only the sermon index")
    index.add_argument("--language", default="en")

    books = sub.add_parser("books", help="download every sermon's text")
    books.add_argument("--language", default="en")
    books.add_argument("--limit", type=int, default=0, help="stop after N sermons (smoke test)")
    books.add_argument("--workers", type=int, default=DEFAULT_WORKERS)
    books.add_argument("--delay", type=float, default=DEFAULT_DELAY)
    books.add_argument("--force", action="store_true", help="re-fetch files that already exist")
    books.add_argument("--keep-raw", action="store_true",
                       help="also save each API reply (large: ~250 KB per sermon)")

    audio = sub.add_parser("audio", help="download every sermon's M4A")
    audio.add_argument("--years", default=None, help="'47-65', '65,64', or a single year")
    audio.add_argument("--limit", type=int, default=0)
    audio.add_argument("--workers", type=int, default=3)
    audio.add_argument("--delay", type=float, default=0.5)
    audio.add_argument("--force", action="store_true")

    both = sub.add_parser("all", help="books, then audio")
    both.add_argument("--language", default="en")
    both.add_argument("--years", default=None)
    both.add_argument("--limit", type=int, default=0)
    both.add_argument("--workers", type=int, default=DEFAULT_WORKERS)
    both.add_argument("--delay", type=float, default=DEFAULT_DELAY)
    both.add_argument("--force", action="store_true")
    both.add_argument("--keep-raw", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-7s %(message)s",
                        datefmt="%H:%M:%S")

    def stop(signum, frame):  # noqa: ARG001 - signal handler signature
        logger.warning("stopping — finishing the files already in flight")
        STOP.set()

    signal.signal(signal.SIGINT, stop)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, stop)

    if args.command == "index":
        BOOKS_DIR.mkdir(parents=True, exist_ok=True)
        entries = fetch_index(args.language)
        (BOOKS_DIR / "index.json").write_text(
            json.dumps({"language": args.language,
                        "fetched": time.strftime("%Y-%m-%dT%H:%M:%S"),
                        "sermons": entries}, indent=1, ensure_ascii=False), encoding="utf-8")
        logger.info("%d sermons -> %s", len(entries), BOOKS_DIR / "index.json")
        return 0

    if args.command == "books":
        run_books(args)
        return 0

    if args.command == "audio":
        run_audio(args)
        return 0

    if args.command == "all":
        run_books(argparse.Namespace(**vars(args)))
        if not STOP.is_set():
            run_audio(argparse.Namespace(**vars(args)))
        return 0

    return 2


if __name__ == "__main__":
    sys.exit(main())
