# VGRTableScraper — the second VGR source

The text and the audio of Brother Branham's sermons, pulled straight from VGR's own sites.
It is a **separate source** from `src/VGRScrapper/` (which scrapes end-time-message.org PDFs):
different hosts, different artefacts, and its own `data/` tree, so the two can be compared
before either is wired into the app.

Nothing here reads or writes `%LOCALAPPDATA%/bps/the_table.json`. Everything lands under
`data/` and is gitignored; review it first.

## Where the data comes from

| Half | Host | Call |
|---|---|---|
| books (text) | `table.branham.org` | `POST /rest/index/allSermons`, then `POST /rest/sermons/sermonRequest` |
| audio | `branham.org` | `POST /branham/messageaudio.aspx/wmSearchByYear` |

`table.branham.org` is the web Table: text only — its whole bundle contains no audio at all.
The M4A files live on `branham.org`'s Message Audio page, in the same year query its own
JavaScript calls, and are served from CloudFront.

Both hosts refuse a bare POST with `400 Cross-site request validation failed`; the script
sends `Origin`, `Referer` and `X-Requested-With`, exactly like their own pages do.

## Run it

```bash
.venv/Scripts/python.exe src/VGRTableScraper/main.py index        # just the sermon list
.venv/Scripts/python.exe src/VGRTableScraper/main.py books        # every sermon's text
.venv/Scripts/python.exe src/VGRTableScraper/main.py audio        # every sermon's M4A
.venv/Scripts/python.exe src/VGRTableScraper/main.py all          # both
```

Useful flags:

```bash
--limit 3            stop after N items (smoke test)
--years 65,64        audio only from these years ('47-65' also works)
--workers 2          concurrent requests (default 2 for books, 3 for audio)
--delay 0.4          seconds between API calls
--force              re-fetch files that already exist
--keep-raw           also keep each API reply (~250 KB per sermon)
-v                   debug logging
```

Every step **resumes**: a file that exists with content is skipped, so an interrupted run is
continued by running the same command again. `Ctrl+C` stops after the files in flight.

## What you get

```
data/books/index.json           the index as served + a normalised list
data/books/books.csv            code, year, title, minutes, paragraphs, file
data/books/table.json           schema-1 library, the shape TheTableLibrary reads
data/books/txt/<year>/<code>_<Title>.txt     one sermon, numbered paragraphs
data/books/raw/<code>.json      untouched API reply (--keep-raw only)
data/books/failures.txt         only if something failed, one line per sermon
data/audio/<year>/<code>_<Title>.m4a         one sermon
data/audio/manifest.csv         code, year, title, minutes, bytes, url, file
```

### Paragraph numbers are the printed ones

The API cuts a sermon into page-sized `Sections`, and each `<p>` carries its printed number in
`<span class="pn">N</span>` — except paragraph 1, which is marked by a decorative eagle glyph
instead. The script keeps the printed numbers (never renumbers 1..N), because the app quotes
them: a citation reading `47-0412 … 3` must land on the paragraph the paper edition prints
as 3.

### Sizes, measured

- text: 1,218 sermons in the index; each `.txt` is a few tens of KB
- audio: 1,206 files across 1947–1965, measured at **~252 KB per minute of audio** — the
  whole library is **≈27 GB** (113,029 minutes)

## Notes

- These are VGR's copyrighted recordings and transcripts. This is a personal downloader for
  local study; don't redistribute what it fetches. Keep the delay/worker defaults modest —
  these are someone else's servers.
- Only `m4a` is offered by the website (a `.opus` next to it is 404). The Table's Dynamic
  Audio features need Opus, which VGR ships through the app and the micro SD card instead.
- `wmSearchBySeries` / `wmSearchByMinutes` / `wmSetLanguage` exist on the same page method and
  are the hooks for series audio (the Church Ages book) and translations, if that is wanted
  later.
