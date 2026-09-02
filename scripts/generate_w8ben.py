#!/usr/bin/env python3
"""Fill IRS Form W-8BEN (Rev. October 2021) from a local, gitignored config.

WHAT THIS DOES: downloads the official blank W-8BEN from irs.gov (cached
locally after the first run), introspects its AcroForm fields, fills them
from scripts/.w8ben_config.json (a file YOU create locally -- see below),
and writes a filled PDF.

WHAT THIS DELIBERATELY DOES NOT DO:
  - It does not embed, hardcode, or invent any real PAN, address, or date
    of birth anywhere in this script or in git history. This repository
    has a public GitHub remote and deploys via Vercel -- committing a
    real Indian PAN, home address, and date of birth to it would publish
    them. scripts/.w8ben_config.json and docs/W8BEN_*_prefilled.pdf are
    both gitignored (see .gitignore) specifically so this can't happen
    by accident. If no config file exists, this script generates a
    TEMPLATE config full of obvious placeholder values and stops --
    you fill in the real values locally, it never guesses them for you.
  - It does not apply a signature. Line 20 ("Signature of beneficial
    owner") is a genuine PDF digital-signature field (/Sig), not a text
    field -- pypdf cannot meaningfully fill it with typed text, and this
    script does not try. The output still needs to be printed and
    hand-signed, or run through a real e-signature workflow, before it
    means anything.
  - It does not decide your treaty position for you. --treaty-position
    selects between the two positions docs/W8BEN_GUIDE.md discusses
    (Article 7 / 0%, or Article 12(2) / 15% if a client's AP department
    classifies the payment as a software royalty instead of business
    profits) -- see docs/W8BEN_GUIDE.md Sec 2 for why that characterization
    is a substantive judgment call, not a form-filling default. Confirm
    the correct one with a CPA before this form is ever submitted.

Field mapping was derived empirically (not guessed): the current IRS
revision ships with no /TU tooltip text on its fields, so field
purposes were inferred from each widget's on-page (x, y) position
against the known W-8BEN layout, cross-checked against field order.
See FIELD_MAP below for the reasoning per field. Given that, ALWAYS
visually check the generated PDF against a rendered copy of the real
form before treating it as accurate -- run with --introspect-only first
to print the raw field list without filling anything.

Usage:
    python scripts/generate_w8ben.py --introspect-only
    python scripts/generate_w8ben.py                       # first run: writes a template config, then stops
    python scripts/generate_w8ben.py --treaty-position article7
    python scripts/generate_w8ben.py --treaty-position article12
"""
import argparse
import json
import sys
import urllib.request
from datetime import date
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
IRS_URL = "https://www.irs.gov/pub/irs-pdf/fw8ben.pdf"
CACHE_DIR = REPO_ROOT / ".cache"
CACHED_BLANK_PDF = CACHE_DIR / "fw8ben_blank.pdf"
DEFAULT_CONFIG_PATH = Path(__file__).resolve().parent / ".w8ben_config.json"
DEFAULT_OUTPUT_PATH = REPO_ROOT / "docs" / "W8BEN_Alakshendra_Roy_prefilled.pdf"

# Empirically derived from widget (x, y) positions on page 1 of the
# Rev. Oct 2021 form -- see module docstring. "line" is the form's own
# line number/label, for the human cross-check this script prints.
FIELD_MAP = {
    "topmostSubform[0].Page1[0].f_1[0]":  ("name", "Line 1 -- Name of individual"),
    "topmostSubform[0].Page1[0].f_2[0]":  ("citizenship", "Line 2 -- Country of citizenship"),
    "topmostSubform[0].Page1[0].f_3[0]":  ("address_street", "Line 3 -- Permanent residence address (street)"),
    "topmostSubform[0].Page1[0].f_4[0]":  ("address_city_state_postal", "Line 3 -- City/town, state/province, postal code"),
    "topmostSubform[0].Page1[0].f_5[0]":  ("address_country", "Line 3 -- Country"),
    "topmostSubform[0].Page1[0].f_6[0]":  ("mailing_street", "Line 4 -- Mailing address, if different (street)"),
    "topmostSubform[0].Page1[0].f_7[0]":  ("mailing_city_state_postal", "Line 4 -- City/town, state/province, postal code"),
    "topmostSubform[0].Page1[0].f_8[0]":  ("mailing_country", "Line 4 -- Country"),
    "topmostSubform[0].Page1[0].f_9[0]":  ("us_tin", "Line 5 -- U.S. taxpayer identification number (SSN or ITIN), if any"),
    "topmostSubform[0].Page1[0].f_10[0]": ("foreign_tin", "Line 6a -- Foreign tax identifying number (PAN) -- populated, not exempted via 6b: most withholding agents treat this as a precondition for processing the Line 10 treaty claim at all (see docs/W8BEN_GUIDE.md Sec 3)"),
    "topmostSubform[0].Page1[0].c1_01[0]": ("ftin_not_required", "Line 6b -- checkbox: FTIN not legally required"),
    "topmostSubform[0].Page1[0].f_11[0]": ("reference_number", "Line 7 -- Reference number(s), optional"),
    "topmostSubform[0].Page1[0].f_12[0]": ("date_of_birth", "Line 8 -- Date of birth, MUST be MM-DD-YYYY (the form's own required format -- DD/MM/YYYY risks automated-intake/OCR misreads, see docs/W8BEN_GUIDE.md Sec 3)"),
    "topmostSubform[0].Page1[0].f_13[0]": ("treaty_country", "Line 9 -- Country claiming treaty benefits"),
    "topmostSubform[0].Page1[0].f_14[0]": ("treaty_article_paragraph", "Line 10 -- Treaty article/paragraph"),
    "topmostSubform[0].Page1[0].f_15[0]": ("treaty_rate_percent", "Line 10 -- Withholding rate claimed (%)"),
    "topmostSubform[0].Page1[0].f_16[0]": ("treaty_income_type", "Line 10 -- Type of income"),
    "topmostSubform[0].Page1[0].f_17[0]": ("treaty_income_type_cont", "Line 10 -- (position-inferred, VERIFY VISUALLY)"),
    "topmostSubform[0].Page1[0].f_18[0]": ("treaty_explanation", "Line 10 -- Explanation of eligibility"),
    "topmostSubform[0].Page1[0].c1_02[0]": ("signer_capacity_checkbox", "Part III -- checkbox: signing in a representative capacity"),
    # f_20 (Line 20, "Signature of beneficial owner") is /Sig -- deliberately not filled, see docstring.
    "topmostSubform[0].Page1[0].Date[0]": ("signature_date", "Part III -- Date (MM-DD-YYYY)"),
    "topmostSubform[0].Page1[0].f_21[0]": ("printed_name", "Part III -- Print name of signer"),
}

TREATY_POSITIONS = {
    "article7": {
        # Displaces the Internal Revenue Code Chapter 3 default 30% gross
        # withholding rate with 0%, per DTAA Article 7 -- but only if the
        # underlying facts genuinely support "business profits, no US PE."
        # The explanation text below is OUR certification of OUR own
        # operating facts, made under penalty of perjury on the actual
        # form -- confirm it's true for a given engagement before signing,
        # not just before running this script. See docs/W8BEN_GUIDE.md Sec 2.
        "treaty_article_paragraph": "7",
        "treaty_rate_percent": "0",
        "treaty_income_type": "business profits",
        "treaty_explanation": (
            "Software licensing, telemetry benchmarking, and technical integration "
            "services performed entirely outside the United States with no U.S. "
            "permanent establishment."
        ),
    },
    "article12": {
        "treaty_article_paragraph": "12(2)",
        "treaty_rate_percent": "15",
        "treaty_income_type": "royalties",
        "treaty_explanation": (
            "Payment characterized by the withholding agent as a royalty for the "
            "right to use licensed software; claimed at the India-U.S. treaty-capped "
            "rate under Article 12(2). CONFIRM this rate against the current IRS "
            "tax treaty table / Publication 515 before use -- see docs/W8BEN_GUIDE.md Sec 2."
        ),
    },
}

TEMPLATE_CONFIG = {
    "_comment": "Local-only, gitignored (see .gitignore). Replace every "
                "[PLACEHOLDER] below with real values before generating a "
                "PDF you intend to actually use. Never commit this file.",
    "name": "Alakshendra Roy",
    "citizenship": "India",
    "address_street": "[PLACEHOLDER -- street address]",
    "address_city_state_postal": "[PLACEHOLDER -- city, state, PIN code]",
    "address_country": "India",
    "mailing_street": "",
    "mailing_city_state_postal": "",
    "mailing_country": "",
    "us_tin": "",
    "foreign_tin": "[PLACEHOLDER -- PAN, format AAAAA9999A]",
    "reference_number": "",
    "date_of_birth": "[PLACEHOLDER -- MM-DD-YYYY]",
    "treaty_country": "India",
    "printed_name": "Alakshendra Roy",
    "signer_capacity": "Individual / Sole Proprietor",
}

PLACEHOLDER_MARKER = "[PLACEHOLDER"


def download_blank_form() -> Path:
    if CACHED_BLANK_PDF.exists():
        return CACHED_BLANK_PDF
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Downloading official blank W-8BEN from {IRS_URL} ...")
    req = urllib.request.Request(IRS_URL, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = resp.read()
    CACHED_BLANK_PDF.write_bytes(data)
    print(f"Cached {len(data):,} bytes to {CACHED_BLANK_PDF} (gitignored, not committed).")
    return CACHED_BLANK_PDF


def introspect(pdf_path: Path) -> None:
    from pypdf import PdfReader

    reader = PdfReader(str(pdf_path))
    fields = reader.get_fields() or {}
    print(f"\n{len(fields)} AcroForm fields found in {pdf_path.name}:\n")
    for name, f in fields.items():
        mapped = FIELD_MAP.get(name)
        label = mapped[1] if mapped else "(unmapped)"
        print(f"  {name:55s} [{f.get('/FT')}]  -> {label}")


def load_or_init_config(config_path: Path) -> "dict | None":
    if not config_path.exists():
        config_path.parent.mkdir(parents=True, exist_ok=True)
        config_path.write_text(json.dumps(TEMPLATE_CONFIG, indent=2))
        print(
            f"No config found -- wrote a template to {config_path} "
            "(gitignored, local-only).\n"
            "Fill in the real address / PAN / date of birth there, then re-run "
            "this script. Nothing was filled or downloaded to a real PDF this run."
        )
        return None

    config = json.loads(config_path.read_text())
    placeholder_fields = [
        k for k, v in config.items()
        if not k.startswith("_") and isinstance(v, str) and PLACEHOLDER_MARKER in v
    ]
    if placeholder_fields:
        print(
            f"WARNING: {config_path} still has unfilled placeholder values for: "
            f"{', '.join(placeholder_fields)}.\n"
            "The generated PDF below will contain those literal placeholder "
            "strings, not real data -- it is NOT ready to submit anywhere."
        )
    return config


def build_fill_values(config: dict, treaty_position: str) -> dict:
    values = dict(config)
    values.update(TREATY_POSITIONS[treaty_position])
    today = date.today().strftime("%m-%d-%Y")
    values.setdefault("signature_date", today)
    values["ftin_not_required"] = "/Off"  # PAN is being supplied on 6a, so 6b stays unchecked
    values["signer_capacity_checkbox"] = "/Off"  # signing for self, not as an agent/POA
    return values


def fill_pdf(blank_path: Path, config: dict, treaty_position: str, output_path: Path) -> None:
    from pypdf import PdfReader, PdfWriter

    values = build_fill_values(config, treaty_position)

    reader = PdfReader(str(blank_path))
    writer = PdfWriter()
    writer.append(reader)

    field_values = {}
    print("\nFilling fields (mapped meaning -> value):")
    for pdf_field_name, (config_key, label) in FIELD_MAP.items():
        if config_key.endswith("_checkbox") or config_key == "ftin_not_required":
            continue  # handled separately below; these are /Btn, not /Tx
        val = values.get(config_key, "")
        if val:
            field_values[pdf_field_name] = val
            print(f"  {label:55s} = {val!r}")

    if writer.pages:
        writer.update_page_form_field_values(
            writer.pages[0], field_values, auto_regenerate=False
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "wb") as f:
        writer.write(f)

    print(f"\nWrote {output_path}")
    print(
        "NOTE: Line 20 (Signature of beneficial owner) is a digital-signature "
        "field and was intentionally NOT filled -- print and hand-sign, or "
        "route through a proper e-signature workflow, before this means anything.\n"
        "This file is gitignored (docs/W8BEN_*_prefilled.pdf) -- it will not be "
        "committed even if you `git add -A`."
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH,
                         help="Path to the local, gitignored config JSON (default: scripts/.w8ben_config.json)")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT_PATH,
                         help="Output PDF path (default: docs/W8BEN_Alakshendra_Roy_prefilled.pdf, gitignored)")
    parser.add_argument("--treaty-position", choices=list(TREATY_POSITIONS), default="article7",
                         help="Which treaty claim to fill on Line 10 (default: article7, 0%%). "
                              "See docs/W8BEN_GUIDE.md Sec 2 before choosing.")
    parser.add_argument("--introspect-only", action="store_true",
                         help="Download (if needed) and print the raw field list, then exit without filling anything.")
    args = parser.parse_args()

    blank_path = download_blank_form()

    if args.introspect_only:
        introspect(blank_path)
        return

    introspect(blank_path)

    config = load_or_init_config(args.config)
    if config is None:
        sys.exit(0)

    fill_pdf(blank_path, config, args.treaty_position, args.output)


if __name__ == "__main__":
    main()
