import csv
import logging
import os
import re
import signal
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urljoin, urlparse

import requests
from bs4 import BeautifulSoup
from colorlog import ColoredFormatter
from requests.adapters import HTTPAdapter
from tqdm import tqdm
from urllib3.util.retry import Retry

############################################################
# CONFIG
############################################################

BASE_URL = "https://end-time-message.org"
START_URL = BASE_URL + "/wm-branham-sermons-pdf"

DOWNLOAD_DIR = Path("downloads")

LOG_DIR = Path("logs")

LOG_FILE = LOG_DIR / "scraper.log"

CSV_FILE = DOWNLOAD_DIR / "metadata.csv"

MAX_DOWNLOAD_WORKERS = 10

REQUEST_TIMEOUT = 60

HEADERS = {
    "User-Agent":
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/138.0 Safari/537.36"
}

############################################################
# CREATE DIRECTORIES
############################################################

DOWNLOAD_DIR.mkdir(exist_ok=True)

LOG_DIR.mkdir(exist_ok=True)

############################################################
# LOGGER
############################################################

logger = logging.getLogger("branham")

logger.setLevel(logging.INFO)

formatter = ColoredFormatter(
    "%(log_color)s%(asctime)s | %(levelname)-8s | %(message)s",
    datefmt="%H:%M:%S"
)

console = logging.StreamHandler()

console.setFormatter(formatter)

file_handler = logging.FileHandler(LOG_FILE)

file_handler.setFormatter(
    logging.Formatter(
        "%(asctime)s %(levelname)s %(message)s"
    )
)

logger.addHandler(console)

logger.addHandler(file_handler)

############################################################
# SESSION
############################################################

session = requests.Session()

retry = Retry(
    total=5,
    backoff_factor=2,
    status_forcelist=[
        429,
        500,
        502,
        503,
        504
    ],
)

adapter = HTTPAdapter(
    max_retries=retry
)

session.mount("https://", adapter)

session.mount("http://", adapter)

session.headers.update(HEADERS)

############################################################
# CTRL+C
############################################################

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

    year: str

    title: str

    download_url: str

############################################################
# GLOBAL STORAGE
############################################################

year_pages = {}

sermons = []

seen_urls = set()

metadata_lock = threading.Lock()

############################################################
# HELPERS
############################################################

def request(url: str):

    logger.info(f"GET {url}")

    r = session.get(
        url,
        timeout=REQUEST_TIMEOUT,
    )

    r.raise_for_status()

    return r

############################################################

def slug(text):

    text = re.sub(r"[\\\\/:*?\"<>|]", "", text)

    text = re.sub(r"\s+", "_", text)

    return text.strip("_")

############################################################

def year_folder(year):

    folder = DOWNLOAD_DIR / slug(year)

    folder.mkdir(
        parents=True,
        exist_ok=True,
    )

    return folder

############################################################

def save_metadata(sermon, filename):

    exists = CSV_FILE.exists()

    with metadata_lock:

        with open(
            CSV_FILE,
            "a",
            newline="",
            encoding="utf8"
        ) as f:

            writer = csv.writer(f)

            if not exists:

                writer.writerow(
                    [
                        "year",
                        "title",
                        "url",
                        "file",
                    ]
                )

            writer.writerow(
                [
                    sermon.year,
                    sermon.title,
                    sermon.download_url,
                    filename,
                ]
            )

############################################################

def filename_for(sermon):

    return year_folder(
        sermon.year
    ) / f"{slug(sermon.title)}.pdf"
    
    
    
############################################################
# DISCOVER YEAR PAGES
############################################################

def discover_year_pages():

    logger.info("")
    logger.info("=" * 70)
    logger.info("DISCOVERING YEAR PAGES")
    logger.info("=" * 70)

    response = request(START_URL)

    soup = BeautifulSoup(response.text, "lxml")

    count = 0

    for a in soup.find_all("a", href=True):

        href = a["href"].strip()

        text = a.get_text(" ", strip=True)

        if "William Branham Sermons" not in text:
            continue

        if "PDF" not in text:
            continue

        url = urljoin(BASE_URL, href)

        year = (
            text.replace("William Branham Sermons", "")
                .replace("PDF", "")
                .strip()
        )

        if year in year_pages:
            continue

        year_pages[year] = url

        count += 1

        logger.info(f"[YEAR] {year}")

    logger.info("")
    logger.info(f"Found {count} Year Pages")
    
    
    ############################################################
# PARSE A SINGLE YEAR PAGE
############################################################
def parse_year_page(year, url):

    logger.info("")
    logger.info("-" * 60)
    logger.info(f"Scanning {year}")
    logger.info("-" * 60)

    response = request(url)

    soup = BeautifulSoup(response.text, "lxml")

    downloads = soup.select("a.eflpro_download")

    logger.info(f"Found {len(downloads)} download links")

    found = 0

    for anchor in downloads:

        href = anchor.get("href")

        if not href:
            continue

        href = urljoin(BASE_URL, href)

        # Skip duplicates
        if href in seen_urls:
            continue

        seen_urls.add(href)

        # ---------------------------------------------------
        # Find the title from the surrounding row
        # ---------------------------------------------------

        title = None

        # Method 1 - parent <li>
        row = anchor.find_parent("li")

        if row:
            text = row.get_text(" ", strip=True)
            text = text.replace("Download", "").strip()

            if text:
                title = text

        # Method 2 - parent <tr>
        if not title:

            row = anchor.find_parent("tr")

            if row:

                cells = row.find_all("td")

                if cells:

                    text = cells[0].get_text(" ", strip=True)

                    if text:
                        title = text

        # Method 3 - previous sibling
        if not title:

            prev = anchor.find_previous()

            if prev:

                text = prev.get_text(" ", strip=True)

                if text:
                    title = text

        # Method 4 - fallback
        if not title:
            title = f"Unknown_{found+1}"

        sermon = Sermon(
            year=year,
            title=title,
            download_url=href,
        )

        sermons.append(sermon)

        found += 1

        logger.info(f"[FOUND] {title}")

    logger.info("")
    logger.info(f"Collected {found} sermons")
    
    ############################################################
# SCAN ALL YEARS
############################################################

def collect_sermons():

    logger.info("")
    logger.info("=" * 70)
    logger.info("COLLECTING SERMONS")
    logger.info("=" * 70)

    for year in sorted(year_pages.keys(), reverse=True):

        if STOP:
            break

        parse_year_page(
            year,
            year_pages[year],
        )

    logger.info("")
    logger.info("=" * 70)
    logger.info(f"TOTAL SERMONS : {len(sermons)}")
    logger.info("=" * 70)
    
############################################################
# DOWNLOAD A SERMON
############################################################

def download_sermon(sermon: Sermon):

    if STOP:
        return

    filename = filename_for(sermon)

    # Skip if already downloaded
    if filename.exists():
        logger.info(f"SKIP {filename.name}")
        return

    try:

        logger.info(f"Downloading {sermon.title}")

        response = session.get(
            sermon.download_url,
            stream=True,
            timeout=REQUEST_TIMEOUT,
            allow_redirects=True,
        )

        response.raise_for_status()

        with open(filename, "wb") as f:

            for chunk in response.iter_content(1024 * 128):

                if chunk:
                    f.write(chunk)

        save_metadata(
            sermon,
            str(filename),
        )

        logger.info(f"DONE {filename.name}")

    except Exception as e:

        logger.error(f"FAILED {sermon.title}")
        logger.error(e)

############################################################
# DOWNLOAD ALL SERMONS
############################################################

def download_all():

    logger.info("")
    logger.info("=" * 70)
    logger.info("DOWNLOADING SERMONS")
    logger.info("=" * 70)

    with ThreadPoolExecutor(MAX_DOWNLOAD_WORKERS) as executor:

        futures = [
            executor.submit(download_sermon, sermon)
            for sermon in sermons
        ]

        with tqdm(
            total=len(futures),
            desc="Downloading",
            unit="file",
        ) as progress:

            for future in as_completed(futures):

                future.result()

                progress.update(1)
def main():

    start = time.time()

    discover_year_pages()

    collect_sermons()

    download_all()

    elapsed = time.time() - start

    logger.info("")
    logger.info("=" * 70)
    logger.info("FINISHED")
    logger.info("=" * 70)
    logger.info(f"Years     : {len(year_pages)}")
    logger.info(f"Sermons   : {len(sermons)}")
    logger.info(f"Time      : {elapsed:.1f}s")
    logger.info("=" * 70)


if __name__ == "__main__":
    main()