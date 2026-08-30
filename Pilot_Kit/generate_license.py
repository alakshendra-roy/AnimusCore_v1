#!/usr/bin/env python3
"""Animus Core Pilot Kit -- 30-day hardware-locked evaluation license generator.

Issues one RSA-2048-signed, hardware-locked .lic file good for exactly
EVAL_PERIOD_DAYS from the moment it's signed, for a specific customer
machine's hardware fingerprint (MachineGuid + primary NIC MAC, SHA-256 --
see animus::compute_fingerprint in AnimusCore_v1/animus_engine.cpp).

This is a VENDOR-SIDE tool, not something a pilot customer runs. It must
be run from a full clone of this repository that holds the private
signing key (AnimusCore_v1/license_tools/private/license_private.blob,
gitignored -- generate one with
AnimusCore_v1/license_tools/generate_license_keypair.ps1 if this is a
fresh vendor machine). A pilot customer only ever receives the resulting
.lic file, never this script's private key or the license_tools/private/
directory.

Zero-dependency (stdlib only: argparse, shutil, subprocess, sys, pathlib),
consistent with this SDK's zero-third-party-dependency design (see
CLAUDE.md). Deliberately does NOT reimplement RSA-2048 signing in Python
-- that already exists, is already exercised in production (see
BENCHMARKS.md's licensing phases), and is Windows-only for the same
reason animus_verify_license itself is (BCrypt/CNG, MachineGuid, a real
MAC address). This script shells out to the existing, tested
AnimusCore_v1/license_tools/sign_license.ps1 rather than duplicating that
security-sensitive logic -- the same approach scripts/generate_license.py
already takes for the general-purpose (non-pilot-specific) case.

Usage:
    # Issue a 30-day evaluation license for a named pilot customer machine:
    python Pilot_Kit/generate_license.py --out acme_corp_pilot.lic \\
        --fingerprint <64 hex chars from the customer's machine>

    # Issue a 30-day evaluation license for THIS machine (local testing only):
    python Pilot_Kit/generate_license.py --out local_eval.lic
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent
_SIGN_SCRIPT = _REPO_ROOT / "AnimusCore_v1" / "license_tools" / "sign_license.ps1"

EVAL_PERIOD_DAYS = 30
DEFAULT_MAX_CORES = 4  # a deliberately modest default entitlement for an evaluation license


def _find_powershell() -> str:
    for candidate in ("powershell", "pwsh"):
        found = shutil.which(candidate)
        if found:
            return found
    raise SystemExit(
        "generate_license.py: no PowerShell found on PATH (tried 'powershell', 'pwsh') -- "
        "license signing is Windows-only, same as animus_verify_license itself "
        "(see AnimusCore_v1/QUICKSTART.md guide 5)."
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True, help="output .lic file path to hand to the pilot customer")
    parser.add_argument(
        "--fingerprint", default="",
        help=(
            "64 hex chars -- the customer machine's hardware fingerprint. Omit only to "
            "issue a license for THIS machine (local testing); a real pilot deployment "
            "should always pass the customer's own fingerprint."
        ),
    )
    parser.add_argument(
        "--max-cores", type=int, default=DEFAULT_MAX_CORES,
        help=(
            f"entitled logical core count for CPU-pinning features (default: {DEFAULT_MAX_CORES}, "
            "a deliberately modest evaluation-tier entitlement -- production licenses can "
            "grant more via scripts/generate_license.py's unrestricted --max-cores)"
        ),
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()

    if not args.fingerprint:
        print(
            "WARNING: no --fingerprint given -- issuing a license for THIS machine, not a "
            "pilot customer's. Pass --fingerprint <hex> collected from the customer's "
            "machine (see AnimusCore_v1/QUICKSTART.md guide 5) for a real pilot deployment.",
            file=sys.stderr,
        )

    if not _SIGN_SCRIPT.exists():
        raise SystemExit(
            f"generate_license.py: expected script not found at {_SIGN_SCRIPT} -- run this "
            "from a full clone of the Animus Core repository, not a standalone copy of "
            "Pilot_Kit/ alone."
        )

    powershell = _find_powershell()
    ps1_args = [
        "-OutFile", args.out,
        "-MaxCores", str(args.max_cores),
        "-ExpiresInDays", str(EVAL_PERIOD_DAYS),
    ]
    if args.fingerprint:
        ps1_args += ["-FingerprintHex", args.fingerprint]

    cmd = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(_SIGN_SCRIPT)] + ps1_args
    result = subprocess.run(cmd)

    if result.returncode == 0:
        print(f"\nIssued a {EVAL_PERIOD_DAYS}-day evaluation license: {args.out}")
        print(
            "Send only this .lic file to the pilot customer -- never the private key "
            "or the license_tools/private/ directory."
        )
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
