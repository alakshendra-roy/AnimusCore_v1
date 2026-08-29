#!/usr/bin/env python3
"""Offline RSA-2048 license key generation / signing CLI for Animus Core's
proprietary-edition license enforcement (animus_verify_license /
animus_check_license_status, AnimusCore_v1/animus_engine.cpp).

Zero-dependency wrapper (stdlib only: argparse, subprocess, sys, pathlib) --
this does NOT reimplement RSA-2048 keygen/signing in Python. That
cryptography already exists, is already exercised in production, and is
Windows-only for the same reason animus_verify_license itself is (BCrypt/
CNG, MachineGuid, a real MAC address -- see QUICKSTART.md guide 5):
    AnimusCore_v1/license_tools/generate_license_keypair.ps1  (.NET RSACng)
    AnimusCore_v1/license_tools/sign_license.ps1

Reimplementing that in pure-Python stdlib would mean hand-rolling RSA-2048
key generation and PKCS1/SHA-256 signing without a real crypto library --
this SDK's zero-third-party-dependency rule (see pyproject.toml's
`dependencies = []`) rules out reaching for `cryptography`/`pycryptodome`
-- a large, security-sensitive duplicate implementation for output the
existing PowerShell tool already produces correctly, verified against
animus_verify_license's own BCryptVerifySignature call. This script exists
so "generate/sign/check a license" has one documented, scriptable Python
entry point instead of two parallel license generators that could drift
out of sync -- `keypair`/`sign` below shell out to the scripts above;
`status` is the one genuinely new capability, calling straight into the
native check via animus.bindings.

Usage:
    python scripts/generate_license.py keypair
    python scripts/generate_license.py sign --out customer.lic --max-cores 4
    python scripts/generate_license.py sign --out customer.lic --max-cores 8 \\
        --fingerprint <64 hex chars> --expires-in-days 365
    python scripts/generate_license.py status customer.lic
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent
_LICENSE_TOOLS_DIR = _REPO_ROOT / "AnimusCore_v1" / "license_tools"
_KEYPAIR_SCRIPT = _LICENSE_TOOLS_DIR / "generate_license_keypair.ps1"
_SIGN_SCRIPT = _LICENSE_TOOLS_DIR / "sign_license.ps1"


def _find_powershell() -> str:
    for candidate in ("powershell", "pwsh"):
        found = shutil.which(candidate)
        if found:
            return found
    raise SystemExit(
        "generate_license.py: no PowerShell found on PATH (tried 'powershell', 'pwsh') -- "
        "this tool is Windows-only, same as animus_verify_license itself (see QUICKSTART.md guide 5)."
    )


def _run_ps1(script_path: Path, args: list) -> int:
    if not script_path.exists():
        raise SystemExit(f"generate_license.py: expected script not found at {script_path}")
    powershell = _find_powershell()
    cmd = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script_path)] + args
    result = subprocess.run(cmd)
    return result.returncode


def cmd_keypair(_args: argparse.Namespace) -> int:
    """Generates a fresh RSA-2048 keypair. WARNING: regenerating the keypair
    invalidates every previously-issued license, since they were signed
    against the old public key -- see generate_license_keypair.ps1's own
    docstring. Only do this once per deployment lineage, not per license.
    """
    return _run_ps1(_KEYPAIR_SCRIPT, [])


def cmd_sign(args: argparse.Namespace) -> int:
    """Issues one signed license file. Must be run on the machine holding
    license_tools/private/license_private.blob (normally the vendor's own
    machine, not a customer's) -- see sign_license.ps1's own docstring.
    """
    ps1_args = ["-OutFile", args.out, "-MaxCores", str(args.max_cores)]
    if args.fingerprint:
        ps1_args += ["-FingerprintHex", args.fingerprint]
    if args.expires_in_days:
        ps1_args += ["-ExpiresInDays", str(args.expires_in_days)]
    return _run_ps1(_SIGN_SCRIPT, ps1_args)


def cmd_status(args: argparse.Namespace) -> int:
    """Checks a license file against this build's public key and this
    machine's fingerprint via the real native check (animus.bindings) --
    the same code path animus_verify_license itself uses, not a
    reimplementation. Prints the specific LicenseStatus and exits 0 only
    for VALID (nonzero for every failure reason), so this composes cleanly
    in a deployment script's own success/failure check.
    """
    sys.path.insert(0, str(_REPO_ROOT))
    from animus.bindings import AnimusBindings, LicenseStatus  # noqa: E402  (path must be set up first)

    status = AnimusBindings().check_license_status(args.license_path)
    print(f"{args.license_path}: {status.name}")
    return 0 if status == LicenseStatus.VALID else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    p_keypair = subparsers.add_parser("keypair", help="generate a fresh RSA-2048 signing keypair")
    p_keypair.set_defaults(func=cmd_keypair)

    p_sign = subparsers.add_parser("sign", help="issue one signed license file")
    p_sign.add_argument("--out", required=True, help="output .lic file path")
    p_sign.add_argument("--max-cores", type=int, required=True, help="entitled core count")
    p_sign.add_argument("--fingerprint", default="", help="64 hex chars; omit to license this machine")
    p_sign.add_argument("--expires-in-days", type=int, default=0, help="0 (default) = no expiry")
    p_sign.set_defaults(func=cmd_sign)

    p_status = subparsers.add_parser("status", help="check a license file's verification status")
    p_status.add_argument("license_path", help="path to a .lic file")
    p_status.set_defaults(func=cmd_status)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
