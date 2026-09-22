"""Download the COMPLETE sermon PDFs from branham.org (Voice of God Recordings).

Unlike end-time-message.org (main.py), branham.org serves VGR's own typeset,
complete transcripts straight from their CloudFront repo. The site is an
ASP.NET page that answers year queries with a JSON page-method:

    POST /branham/messageaudio.aspx/wmSearchByYear
    body: {"formVars": [{"name": "year", "value": "65"}]}
    -> {"d": [resultsHtml, totalCount, pdfBlockHtml, m4aBlockHtml]}

Each row inside resultsHtml carries the sermon code ("65-0116X"), the title
("Wedding Ceremony"), the location, the duration and a direct
d2w09gj4mqt5u.cloudfront.net/repo/....pdf link.

Files land in year folders named the way the app's The Table importer
already parses them (1965/65_0116X_Wedding_Ceremony.pdf — 4-digit year
folder, yy_ prefix), so the folder import needs no renaming.

Run with the project venv:
    .venv/Scripts/python.exe src/VGRScrapper/branham_org_scraper.py
    ... --years 65,64          # only these years
    ... --max 3                # at most N files per year (smoke test)
    ... --out src/downloads_vgr --workers 6
"""

import argparse
import csv
import html
import logging
import re
import signal
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import unquote

import requests
from bs4 import BeautifulSoup
from colorlog import ColoredFormatter
from requests.adapters import HTTPAdapter
from tqdm import tqdm
from urllib3.util.retry import Retry

############################################################
# CONFIG
############################################################

BASE_URL = "https://branham.org"
PAGE_URL = BASE_URL + "/en/messageaudio"
METHOD_URL = BASE_URL + "/branham/messageaudio.aspx/wmSearchByYear"

DOWNLOAD_DIR = Path(__file__).resolve().parents[2] / "src" / "downloads_vgr"
LOG_DIR = Path("logs")
LOG_FILE = LOG_DIR / "branham_org.log"
CSV_FILE = DOWNLOAD_DIR / "metadata_vgr.csv"

MAX_DOWNLOAD_WORKERS = 6
REQUEST_TIMEOUT = 60

HEADERS = {
    "User-Agent":
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/138.0 Safari/537.36",
    "Content-Type": "application/json; charset=utf-8",
    "X-Requested-With": "XMLHttpRequest",
    "Referer": PAGE_URL,
    "Origin": BASE_URL,
}

logger = logging.getLogger("branham_org")

############################################################
# LOGGER / SESSION / STOP
############################################################

LOG_DIR.mkdir(exist_ok=True)
logger.setLevel(logging.INFO)
_console = logging.StreamHandler()
_console.setFormatter(ColoredFormatter(
    "%(log_color)s%(asctime)s | %(levelname)-8s | %(message)s", datefmt="%H:%M:%S"))
_file = logging.FileHandler(LOG_FILE)
_file.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
logger.addHandler(_console)
logger.addHandler(_file)

session = requests.Session()
session.mount("https://", HTTPAdapter(max_retries=Retry(
    total=5, backoff_factor=2, status_forcelist=[429, 500, 502, 503, 504])))
session.headers.update(HEADERS)

STOP = False


def stop_handler(sig, frame):
    global STOP
    STOP = True
    logger.warning("Stopping... waiting for workers...")


signal.signal(signal.SIGINT, stop_handler)

############################################################
# DATA MODEL
############################################################


@dataclass(slots=True)
class Sermon:
    code: str        # "65-0116X"
    year: str        # "1965"
    title: str       # "Wedding Ceremony"
    location: str    # "Tucson AZ" (may be empty)
    download_url: str

    @property
    def file_name(self) -> str:
        # 65-0116X Wedding Ceremony -> 65_0116X_Wedding_Ceremony.pdf
        # (the yy_ prefix the app's placement parser expects)
        stem = f"{self.code.replace('-', '_')}_{slug(self.title)}"
        return slug(stem) + ".pdf"

    @property
    def rel_path(self) -> Path:
        return Path(self.year) / self.file_name


############################################################
# HELPERS
############################################################


def slug(text: str) -> str:
    text = html.unescape(text)
    text = re.sub(r"[\\/:*?\"<>|]", "", text)
    text = re.sub(r"\s+", "_", text)
    return text.strip("_")


def get(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def save_metadata(sermon: Sermon, filename: str) -> None:
    exists = CSV_FILE.exists()
    with threading.Lock():
        with open(CSV_FILE, "a", newline="", encoding="utf8") as f:
            writer = csv.writer(f)
            if not exists:
                writer.writerow(["code", "year", "title", "location", "url", "file"])
            writer.writerow([sermon.code, sermon.year, sermon.title,
                             sermon.location, sermon.download_url, filename])


############################################################
# DISCOVER THE YEAR LIST (from the page's ddlyears dropdown)
############################################################


def discover_years() -> dict[str, str]:
    """{ '65': '1965', '64': '1964', ... } straight from the page markup."""
    r = session.get(PAGE_URL, timeout=REQUEST_TIMEOUT)
    r.raise_for_status()
    soup = BeautifulSoup(r.text, "lxml")
    select = soup.find("select", id="ddlyears")
    if select is None:
        raise RuntimeError("ddlyears select not found — page layout changed?")
    years = {}
    for option in select.find_all("option"):
        value = (option.get("value") or "").strip()
        label = option.get_text(strip=True)
        if value.isdigit() and re.fullmatch(r"\d{4}", label):
            years[value] = label
    logger.info(f"Found {len(years)} years: "
                f"{min(years.values(), default='?')}..{max(years.values(), default='?')}")
    return years


############################################################
# QUERY ONE YEAR (the page-method the site's own JS calls)
############################################################


def parse_rows(results_html: str) -> list[Sermon]:
    """Each result row is a .messagebox div: code, title, location, PDF link."""
    sermons: list[Sermon] = []
    for block in results_html.split('class="messagebox"')[1:]:
        code_m = re.search(r'class="prodtext">\s*([0-9]{2}-[0-9]{4}[A-Za-z]?)\s*<', block)
        title_m = re.search(r'class="prodtexttitle">\s*([^<]+?)\s*<', block)
        pdf_m = re.search(r'href="(https://[^"]+\.pdf)"[^>]*title="download PDF file"', block)
        if not (code_m and title_m and pdf_m):
            continue
        code = html.unescape(code_m.group(1)).strip()
        title = html.unescape(title_m.group(1)).strip()
        loc_m = re.search(r'class="prodtext2">\s*([^<]*[A-Za-z][^<]*?)\s*</div>', block)
        location = html.unescape(loc_m.group(1)).strip() if loc_m else ""
        yy = code.split("-", 1)[0]
        sermons.append(Sermon(
            code=code,
            year=f"19{yy}" if yy.isdigit() and len(yy) == 2 else "Unfiled",
            title=title,
            location=location,
            download_url=html.unescape(pdf_m.group(1)),
        ))
    return sermons


def fetch_year(value: str) -> list[Sermon]:
    r = session.post(
        METHOD_URL,
        json={"formVars": [{"name": "year", "value": value}]},
        timeout=REQUEST_TIMEOUT,
    )
    r.raise_for_status()
    payload = r.json()["d"]
    logger.info(f"Year {value}: server reports {payload[1]} messages")
    sermons = parse_rows(payload[0])
    logger.info(f"Year {value}: parsed {len(sermons)} PDF rows")
    return sermons


############################################################
# DOWNLOAD
############################################################


def download_sermon(sermon: Sermon, out_dir: Path) -> str:
    if STOP:
        return "stopped"
    target = get(out_dir / sermon.rel_path)
    if target.exists() and target.stat().st_size > 0:
        return "skipped"
    try:
        response = session.get(sermon.download_url, stream=True,
                               timeout=REQUEST_TIMEOUT, allow_redirects=True)
        response.raise_for_status()
        tmp = target.with_suffix(".part")
        with open(tmp, "wb") as f:
            for chunk in response.iter_content(1024 * 128):
                if chunk:
                    f.write(chunk)
        tmp.replace(target)
        save_metadata(sermon, str(target))
        return "downloaded"
    except Exception as e:  # noqa: BLE001 - log and keep the batch going
        logger.error(f"FAILED {sermon.code} {sermon.title}: {e}")
        (target.parent / (target.stem + ".failed")).unlink(missing_ok=True)
        return "failed"


def download_all(sermons: list[Sermon], out_dir: Path) -> None:
    counts = {"downloaded": 0, "skipped": 0, "failed": 0, "stopped": 0}
    with ThreadPoolExecutor(MAX_DOWNLOAD_WORKERS) as executor:
        futures = [executor.submit(download_sermon, s, out_dir) for s in sermons]
        with tqdm(total=len(futures), desc="Downloading", unit="file") as progress:
            for future in as_completed(futures):
                counts[future.result()] += 1
                progress.update(1)
    logger.info(f"Downloaded {counts['downloaded']} · "
                f"already present {counts['skipped']} · "
                f"failed {counts['failed']} · "
                f"stopped {counts['stopped']}")


############################################################
# MAIN
############################################################


def main() -> None:
    global MAX_DOWNLOAD_WORKERS
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=DOWNLOAD_DIR,
                        help=f"output root (default {DOWNLOAD_DIR})")
    parser.add_argument("--years", default="",
                        help="comma-separated 2-digit year filter, e.g. 65,64")
    parser.add_argument("--max", type=int, default=0,
                        help="cap files per year (0 = no cap; smoke tests)")
    parser.add_argument("--workers", type=int, default=MAX_DOWNLOAD_WORKERS)
    parser.add_argument("--list-only", action="store_true",
                        help="discover and print, download nothing")
    args = parser.parse_args()
    MAX_DOWNLOAD_WORKERS = max(1, args.workers)

    start = time.time()
    years = discover_years()
    if args.years:
        keep = {y.strip() for y in args.years.split(",") if y.strip()}
        years = {k: v for k, v in years.items() if k in keep}

    sermons: list[Sermon] = []
    for value in sorted(years, reverse=True):
        if STOP:
            break
        found = fetch_year(value)
        if args.max:
            found = found[: args.max]
        sermons.extend(found)

    logger.info("=" * 70)
    logger.info(f"TOTAL: {len(sermons)} sermons across {len(years)} years")
    logger.info("=" * 70)

    if args.list_only:
        for s in sermons[:20]:
            logger.info(f"{s.code:<10} {s.year} {s.title[:50]:<50} {s.location}")
        return

    download_all(sermons, args.out)
    logger.info(f"Finished in {time.time() - start:.1f}s")


if __name__ == "__main__":
    main()
