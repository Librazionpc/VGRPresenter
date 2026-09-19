import json
import logging
import re
from pathlib import Path
from dataclasses import dataclass, field
from datetime import datetime
from xml.etree import ElementTree as ET

import fitz  # pymupdf
from tqdm import tqdm
from colorlog import ColoredFormatter


# ============================================================
# CONFIGURATION
# ============================================================

PDF_ROOT = Path("downloads")

OUTPUT_DIR = Path("xml")

LOG_DIR = Path("logs")


OUTPUT_FILE = OUTPUT_DIR / "branham_library.xml"

OUTPUT_JSON = OUTPUT_DIR / "branham_library.json"

OUTPUT_PRESENTATION_XML = OUTPUT_DIR / "branham_sermons.xml"

SLIDES_DIR = OUTPUT_DIR / "slides"

SERMONS_DIR = OUTPUT_DIR / "sermons"


SPEAKER = "William Marrion Branham"

XML_VERSION = "1.0"


# Presentation-XML bible attributes (matches xml/kjv.xml)

BIBLE_ABBREV = "WMB"

BIBLE_NAME = "William Marrion Branham Sermons"


# Create folders

OUTPUT_DIR.mkdir(
    exist_ok=True
)

LOG_DIR.mkdir(
    exist_ok=True
)

SLIDES_DIR.mkdir(
    exist_ok=True
)

SERMONS_DIR.mkdir(
    exist_ok=True
)


# ============================================================
# LOGGER
# ============================================================

def setup_logger():

    logger = logging.getLogger(
        "PDF_XML_CONVERTER"
    )

    logger.setLevel(
        logging.INFO
    )

    logger.handlers.clear()


    formatter = ColoredFormatter(
        "%(log_color)s%(asctime)s | %(levelname)-8s | %(message)s",
        datefmt="%H:%M:%S",
    )


    console = logging.StreamHandler()

    console.setFormatter(
        formatter
    )


    file_handler = logging.FileHandler(
        LOG_DIR / "converter.log",
        encoding="utf8"
    )


    file_handler.setFormatter(
        logging.Formatter(
            "%(asctime)s | %(levelname)s | %(message)s"
        )
    )


    logger.addHandler(
        console
    )

    logger.addHandler(
        file_handler
    )


    return logger



logger = setup_logger()



# ============================================================
# DATA MODELS
# ============================================================


@dataclass
class Paragraph:

    number: int

    marker: str

    text: str



@dataclass
class Sermon:

    id: str

    code: str

    title: str

    year: int

    month: int

    day: int

    service: str

    speaker: str = SPEAKER

    paragraphs: list[Paragraph] = field(
        default_factory=list
    )


# ============================================================
# REGEX DEFINITIONS
# ============================================================


# Example:

# 63-0728M_Christ_Is_The_Mystery.pdf

FILENAME_PATTERN = re.compile(
    r"(?P<code>\d{2}[-_]\d{4})(?P<service>[MES]?)_?(?P<title>.*)",
    re.IGNORECASE
)



# Paragraph markers

# E-1
# E-2
# E-3

PARAGRAPH_PATTERN = re.compile(
    r"(E-\d+)(.*?)(?=E-\d+|\Z)",
    re.DOTALL
)



logger.info(
    "PDF XML Converter initialized"
)

# ============================================================
# FILENAME PARSER
# ============================================================

def parse_filename(pdf: Path):

    name = pdf.stem

    match = FILENAME_PATTERN.match(name)


    if not match:

        raise ValueError(
            f"Cannot parse filename: {name}"
        )


    code = match.group(
        "code"
    )

    service = match.group(
        "service"
    ).upper()


    title = match.group(
        "title"
    )


    title = title.replace(
        "_",
        " "
    ).strip()


    yy = int(
        code[:2]
    )

    month = int(
        code[3:5]
    )

    day = int(
        code[5:7]
    )


    year = (
        1900 + yy
        if yy >= 40
        else 2000 + yy
    )


    sermon_id = (
        code
        +
        service
    )


    return {

        "id": sermon_id,

        "code": code,

        "title": title,

        "year": year,

        "month": month,

        "day": day,

        "service": service,

    }



# ============================================================
# TEXT CLEANER
# ============================================================

def clean_text(text: str):

    # remove hidden characters

    text = text.replace(
        "\ufeff",
        ""
    )


    text = text.replace(
        "\u00a0",
        " "
    )


    # normalize spaces

    text = re.sub(
        r"[ \t]+",
        " ",
        text
    )


    # normalize newlines

    text = re.sub(
        r"\n{3,}",
        "\n\n",
        text
    )


    return text.strip()



# ============================================================
# PDF TEXT EXTRACTION
# ============================================================

def extract_pdf_text(pdf: Path):

    logger.info(
        f"Extracting {pdf.name}"
    )


    document = fitz.open(
        pdf
    )


    pages = []


    for page in document:

        text = page.get_text(
            "text"
        )

        if text:

            pages.append(
                text
            )


    document.close()


    full_text = "\n".join(
        pages
    )


    return clean_text(
        full_text
    )



# ============================================================
# PARAGRAPH PARSER
# ============================================================

def parse_paragraphs(text: str):

    paragraphs = []


    matches = list(
        PARAGRAPH_PATTERN.finditer(
            text
        )
    )


    # Some PDFs may not contain E-1 markers

    if not matches:


        paragraphs.append(

            Paragraph(

                number=1,

                marker="",

                text=text

            )

        )


        return paragraphs



    for match in matches:


        marker = match.group(
            1
        )


        body = match.group(
            2
        )


        number = int(
            marker.replace(
                "E-",
                ""
            )
        )


        body = clean_text(
            body
        )


        if not body:
            continue



        paragraphs.append(

            Paragraph(

                number=number,

                marker=marker,

                text=body

            )

        )


    return paragraphs



# ============================================================
# BUILD SERMON OBJECT
# ============================================================

def process_pdf(pdf: Path):


    info = parse_filename(
        pdf
    )


    text = extract_pdf_text(
        pdf
    )


    paragraphs = parse_paragraphs(
        text
    )


    sermon = Sermon(

        id=info["id"],

        code=info["code"],

        title=info["title"],

        year=info["year"],

        month=info["month"],

        day=info["day"],

        service=info["service"],

        paragraphs=paragraphs,

    )


    logger.info(
        f"{sermon.id} -> {len(paragraphs)} paragraphs"
    )


    return sermon


# ============================================================
# XML HELPERS
# ============================================================


def xml_escape(text):

    if not text:
        return ""

    return (
        text
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )



def indent_xml(element, level=0):

    space = "\n" + level * "    "

    if len(element):

        if not element.text or not element.text.strip():

            element.text = space + "    "


        for child in element:

            indent_xml(
                child,
                level + 1
            )


        if not element[-1].tail or not element[-1].tail.strip():

            element[-1].tail = space


    elif level:

        if not element.tail or not element.tail.strip():

            element.tail = space



# ============================================================
# CREATE SERMON XML NODE
# ============================================================


def sermon_to_xml(parent, sermon: Sermon):


    sermon_node = ET.SubElement(
        parent,
        "sermon",
        {
            "id": sermon.id,

            "year": str(sermon.year),

            "code": sermon.code,
        }
    )



    # --------------------------------------------------------
    # Metadata
    # --------------------------------------------------------


    metadata = ET.SubElement(
        sermon_node,
        "metadata"
    )


    ET.SubElement(
        metadata,
        "title"
    ).text = sermon.title



    ET.SubElement(
        metadata,
        "speaker"
    ).text = sermon.speaker



    date = ET.SubElement(
        metadata,
        "date"
    )


    ET.SubElement(
        date,
        "year"
    ).text = str(
        sermon.year
    )


    ET.SubElement(
        date,
        "month"
    ).text = str(
        sermon.month
    )


    ET.SubElement(
        date,
        "day"
    ).text = str(
        sermon.day
    )


    ET.SubElement(
        metadata,
        "service"
    ).text = sermon.service



    # --------------------------------------------------------
    # Paragraphs
    # --------------------------------------------------------


    content = ET.SubElement(
        sermon_node,
        "content"
    )


    for paragraph in sermon.paragraphs:


        node = ET.SubElement(

            content,

            "paragraph",

            {
                "number": str(
                    paragraph.number
                ),

                "marker": paragraph.marker
            }

        )


        node.text = paragraph.text



    return sermon_node



# ============================================================
# CREATE XML LIBRARY
# ============================================================


def create_library_xml(sermons):


    logger.info(
        "Creating XML library"
    )


    root = ET.Element(

        "branham_library",

        {
            "version": XML_VERSION,

            "generated": datetime.utcnow().isoformat()
        }

    )



    # --------------------------------------------------------
    # Library information
    # --------------------------------------------------------


    info = ET.SubElement(
        root,
        "library_info"
    )


    ET.SubElement(
        info,
        "speaker"
    ).text = SPEAKER



    ET.SubElement(
        info,
        "total_sermons"
    ).text = str(
        len(sermons)
    )



    # --------------------------------------------------------
    # Add sermons
    # --------------------------------------------------------


    for sermon in tqdm(
        sermons,
        desc="Building XML"
    ):

        sermon_to_xml(
            root,
            sermon
        )



    indent_xml(
        root
    )



    tree = ET.ElementTree(
        root
    )


    tree.write(

        OUTPUT_FILE,

        encoding="utf-8",

        xml_declaration=True

    )


    logger.info(
        f"XML saved -> {OUTPUT_FILE}"
    )


    return OUTPUT_FILE


# ============================================================
# SERMON TO DICT (shared by JSON exports)
# ============================================================


def sermon_to_dict(sermon: Sermon):


    return {

        "id": sermon.id,

        "code": sermon.code,

        "title": sermon.title,

        "speaker": sermon.speaker,

        "date": {

            "year": sermon.year,

            "month": sermon.month,

            "day": sermon.day,

        },

        "service": sermon.service,

        "paragraphs": [

            {

                "number": paragraph.number,

                "marker": paragraph.marker,

                "text": paragraph.text,

            }

            for paragraph in sermon.paragraphs

        ],

    }


# ============================================================
# CREATE JSON LIBRARY
# ============================================================


def create_library_json(sermons):


    logger.info(
        "Creating JSON library"
    )


    library = {

        "branham_library": {

            "version": XML_VERSION,

            "generated": datetime.utcnow().isoformat(),

            "library_info": {

                "speaker": SPEAKER,

                "total_sermons": len(sermons),

            },

            "sermons": [

                sermon_to_dict(
                    sermon
                )

                for sermon in tqdm(
                    sermons,
                    desc="Building JSON"
                )

            ],

        }

    }


    with open(
        OUTPUT_JSON,
        "w",
        encoding="utf8"
    ) as f:

        json.dump(
            library,
            f,
            indent=2,
            ensure_ascii=False,
        )


    logger.info(
        f"JSON saved -> {OUTPUT_JSON}"
    )


    return OUTPUT_JSON


# ============================================================
# CREATE SLIDES JSON (one file per sermon, 1 paragraph = 1 slide)
# ============================================================


def create_slides_json(sermons):


    logger.info(
        "Creating slides JSON"
    )


    # Clear stale files from a previous run so an unchanged/removed
    # sermon never leaves an old slide behind.

    for stale in SLIDES_DIR.glob(
        "*.json"
    ):

        stale.unlink(
            missing_ok=True
        )


    # Duplicate sermon ids (same code+service, different titles) would
    # overwrite each other's slide file, so disambiguate with a suffix.

    seen = {}


    written = 0


    for sermon in tqdm(
        sermons,
        desc="Building slides"
    ):


        count = seen.get(
            sermon.id,
            0
        )

        seen[sermon.id] = count + 1


        suffix = (
            ""
            if count == 0
            else f"_{count + 1}"
        )


        slides = {

            "sermon": {

                "id": sermon.id,

                "code": sermon.code,

                "title": sermon.title,

                "speaker": sermon.speaker,

                "date": {

                    "year": sermon.year,

                    "month": sermon.month,

                    "day": sermon.day,

                },

                "service": sermon.service,

            },

            "slides": [

                {

                    "number": paragraph.number,

                    "marker": paragraph.marker,

                    "text": paragraph.text,

                }

                for paragraph in sermon.paragraphs

            ],

        }


        slide_file = SLIDES_DIR / f"{sermon.id}{suffix}.json"


        with open(
            slide_file,
            "w",
            encoding="utf8"
        ) as f:

            json.dump(
                slides,
                f,
                indent=2,
                ensure_ascii=False,
            )


        written += 1


    logger.info(
        f"Slides JSON saved -> {SLIDES_DIR} ({written} sermons)"
    )


    return SLIDES_DIR


# ============================================================
# CREATE PRESENTATION XML (KJV/bible style, xml/kjv.xml format)
# ============================================================


def create_presentation_xml(sermons):


    logger.info(
        "Creating presentation XML (bible format)"
    )


    root = ET.Element(

        "bible",

        {
            "abbrev": BIBLE_ABBREV,

            "name": BIBLE_NAME,
        }

    )


    for sermon in tqdm(
        sermons,
        desc="Building presentation XML"
    ):


        book = ET.SubElement(

            root,

            "book",

            {
                "num": sermon.id,
            }

        )


        chapter = ET.SubElement(

            book,

            "chapter",

            {
                "num": "1",
            }

        )


        for paragraph in sermon.paragraphs:


            verse = ET.SubElement(

                chapter,

                "verse",

                {
                    "num": str(
                        paragraph.number
                    ),
                }

            )


            verse.text = paragraph.text


    indent_xml(
        root
    )


    tree = ET.ElementTree(
        root
    )


    # No XML declaration on purpose: the reference xml/kjv.xml starts
    # directly with <bible ...>, and some presentation software rejects
    # files that begin with a declaration.

    tree.write(

        OUTPUT_PRESENTATION_XML,

        encoding="utf-8",

        xml_declaration=False

    )


    logger.info(
        f"Presentation XML saved -> {OUTPUT_PRESENTATION_XML}"
    )


    return OUTPUT_PRESENTATION_XML


# ============================================================
# BSML SERIALIZER (Believers Sermon XML, one file per sermon)
# ============================================================


def xml_attr_escape(text):

    # Same escaping as text content, plus quotes for double-quoted
    # attributes.

    return (
        xml_escape(
            text
        )
        .replace('"', "&quot;")
    )



def sermon_to_bsml(sermon: Sermon):


    # The BSML id uses a dash (54-0825); parsed codes may contain an
    # underscore (54_0825), so normalize for a canonical id.

    code = sermon.code.replace(
        "_",
        "-"
    )


    lines = []


    lines.append(
        '<?xml version="1.0" encoding="UTF-8"?>'
    )

    lines.append(
        ""
    )

    lines.append(
        "<sermon"
    )

    lines.append(
        f'    id="{xml_attr_escape(code)}"'
    )

    lines.append(
        f'    year="{sermon.year}"'
    )

    lines.append(
        f'    month="{sermon.month:02d}"'
    )

    lines.append(
        f'    day="{sermon.day:02d}"'
    )

    lines.append(
        f'    service="{xml_attr_escape(sermon.service)}"'
    )

    lines.append(
        f'    title="{xml_attr_escape(sermon.title)}">'
    )

    lines.append(
        ""
    )


    for paragraph in sermon.paragraphs:


        lines.append(
            f'    <paragraph number="{paragraph.number}">'
        )


        # Collapse PDF line-wrap artifacts to a single clean line; the
        # presentation software splits long text into slides itself.

        body = re.sub(
            r"\s+",
            " ",
            paragraph.text
        ).strip()


        lines.append(
            f"        {xml_escape(body)}"
        )


        lines.append(
            "    </paragraph>"
        )

        lines.append(
            ""
        )


    lines.append(
        "</sermon>"
    )


    return "\n".join(
        lines
    ) + "\n"


# ============================================================
# CREATE BSML FILES (one file per sermon)
# ============================================================


def create_sermons_bsml(sermons):


    logger.info(
        "Creating BSML sermon files"
    )


    # Clear stale files from a previous run

    for stale in SERMONS_DIR.glob(
        "*.xml"
    ):

        stale.unlink(
            missing_ok=True
        )


    # Duplicate ids (same code+service, different titles) disambiguated
    # with a suffix, same as the slides export.

    seen = {}


    written = 0


    for sermon in tqdm(
        sermons,
        desc="Building BSML"
    ):


        count = seen.get(
            sermon.id,
            0
        )

        seen[sermon.id] = count + 1


        suffix = (
            ""
            if count == 0
            else f"_{count + 1}"
        )


        # Filename mirrors the dash-normalized content id (with the
        # service letter, which is not part of the id attribute).

        dash_id = sermon.code.replace(
            "_",
            "-"
        ) + sermon.service


        sermon_file = SERMONS_DIR / f"{dash_id}{suffix}.xml"


        sermon_file.write_text(
            sermon_to_bsml(
                sermon
            ),
            encoding="utf8"
        )


        written += 1


    logger.info(
        f"BSML saved -> {SERMONS_DIR} ({written} sermons)"
    )


    return SERMONS_DIR

# ============================================================
# MAIN PROCESSOR
# ============================================================


def main():


    start_time = datetime.now()



    logger.info("")
    logger.info("=" * 70)
    logger.info(
        "STARTING PDF TO XML / JSON / BSML CONVERSION"
    )
    logger.info("=" * 70)



    # --------------------------------------------------------
    # Find PDFs
    # --------------------------------------------------------


    pdf_files = sorted(
        PDF_ROOT.rglob(
            "*.pdf"
        )
    )


    logger.info(
        f"PDF files found: {len(pdf_files)}"
    )



    if not pdf_files:

        logger.error(
            "No PDF files found"
        )

        return



    sermons = []

    failed = []



    # --------------------------------------------------------
    # Process PDFs
    # --------------------------------------------------------


    for pdf in tqdm(
        pdf_files,
        desc="Processing PDFs"
    ):


        try:


            sermon = process_pdf(
                pdf
            )


            sermons.append(
                sermon
            )


        except Exception as e:


            failed.append(
                pdf.name
            )


            logger.exception(
                f"Failed processing {pdf.name}"
            )



    logger.info("")
    logger.info("=" * 70)

    logger.info(
        f"Successful sermons : {len(sermons)}"
    )

    logger.info(
        f"Failed sermons     : {len(failed)}"
    )

    logger.info("=" * 70)



    # --------------------------------------------------------
    # Create exports (XML, JSON, slides, presentation XML, BSML)
    # --------------------------------------------------------


    if sermons:


        create_library_xml(
            sermons
        )


        create_library_json(
            sermons
        )


        create_slides_json(
            sermons
        )


        create_presentation_xml(
            sermons
        )


        create_sermons_bsml(
            sermons
        )


    else:

        logger.error(
            "No sermons converted"
        )



    # --------------------------------------------------------
    # Failed report
    # --------------------------------------------------------


    if failed:


        failed_file = LOG_DIR / "failed_files.txt"


        with open(
            failed_file,
            "w",
            encoding="utf8"
        ) as f:


            for item in failed:

                f.write(
                    item + "\n"
                )


        logger.warning(
            f"Failed list saved -> {failed_file}"
        )



    elapsed = (
        datetime.now()
        -
        start_time
    )



    logger.info("")
    logger.info("=" * 70)

    logger.info(
        "CONVERSION COMPLETE"
    )

    logger.info(
        f"Time elapsed: {elapsed}"
    )

    logger.info(
        f"Output XML   : {OUTPUT_FILE}"
    )

    logger.info(
        f"Output JSON  : {OUTPUT_JSON}"
    )

    logger.info(
        f"Output slides: {SLIDES_DIR}"
    )

    logger.info(
        f"Output pres  : {OUTPUT_PRESENTATION_XML}"
    )

    logger.info(
        f"Output BSML  : {SERMONS_DIR}"
    )

    logger.info("=" * 70)




# ============================================================
# ENTRY POINT
# ============================================================


if __name__ == "__main__":
    main()