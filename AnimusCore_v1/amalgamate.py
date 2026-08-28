#!/usr/bin/env python3
"""Generates animus_release.hpp: a single-header amalgamation of the four
Animus Core headers (animus.hpp, animus_security.hpp, animus_transport.hpp,
animus_cluster.hpp) for commercial distribution -- a client integrating
Animus Core drops in one file instead of vendoring four with an inter-file
include dependency to get right.

This is a build step, not a hand-maintained artifact: animus_release.hpp is
regenerated from the real source headers every time this script runs, so
the four originals stay the single source of truth and can never drift from
what ships. Re-run after any change to the source headers:

    python amalgamate.py

Layout of the generated file (in dependency order, matching the existing
#include chain animus_cluster.hpp -> animus_transport.hpp ->
animus_security.hpp -> animus.hpp):
  1. animus.hpp          -- portable C++17, no platform dependency
  2. animus_security.hpp -- portable C++17, no platform dependency
  3. animus_transport.hpp -- Windows + MSVC only (Schannel/SSPI)
  4. animus_cluster.hpp   -- Windows + MSVC only (built on #3)

Sections 3-4 are wrapped in `#if defined(_WIN32) && defined(_MSC_VER)` in
the generated file (rather than left to their own `#error` guards) so a
single #include of animus_release.hpp is still valid on any translation
unit that only needs the portable core engine and RBAC/multi-tenancy
layer -- it just silently loses access to animus::transport /
animus::cluster there, rather than failing the whole include. The guard
checks _MSC_VER, not just _WIN32: sections 3-4's certificate loading uses
an MSVC-specific std::ifstream(std::wstring, ...) constructor overload that
MinGW/libstdc++ does not provide, so a `_WIN32`-only guard would still fail
to compile under `g++` on Windows (verified -- see amalgamate.py's own
verification notes in BENCHMARKS.md's Phase 10 section) even though MinGW
does define _WIN32.
"""
import datetime
import re
from pathlib import Path

_HERE = Path(__file__).parent

# (source file, human label, portable-across-platforms?)
_SOURCES = [
    ("animus.hpp", "Phase 1-7: Core Engine, Ring Buffer, Rule Engine, Broker/Execution Interop", True),
    ("animus_security.hpp", "Phase 8: RBAC + Multi-Tenant Isolation", True),
    ("animus_transport.hpp", "Phase 8: mTLS / TLS 1.3 Transport (Schannel)", False),
    ("animus_cluster.hpp", "Phase 9: Distributed Cloud Orchestration & Clustering (Raft-lite over mTLS)", False),
]

_OUTPUT = _HERE / "animus_release.hpp"

# Matches a local `#include "animus...hpp"` line -- e.g. "animus.hpp" itself
# (animus_security.hpp includes it) as well as "animus_security.hpp" /
# "animus_transport.hpp" -- these become redundant once the target is
# inlined below them. Left in place, `#pragma once` alone would NOT save us
# here: it dedupes a given *file on disk*, but the amalgamation inlines that
# file's content directly (no #include of it happens at all for the first
# occurrence), so an un-stripped `#include "animus.hpp"` later in the same
# translation unit re-opens and re-parses the real animus.hpp from disk --
# a genuine redefinition, not a no-op. So these lines must be stripped, not
# just relied upon to self-guard.
_LOCAL_INCLUDE_RE = re.compile(r'^\s*#include\s+"animus[a-zA-Z_]*\.hpp"\s*$')
# The per-file platform guard (`#if !defined(_WIN32) / #error ... / #endif`)
# is stripped from sections 3-4 since the amalgamation wraps them in its own
# outer `#if defined(_WIN32)` instead (see module docstring) -- left in
# place it would be dead but harmless code; stripped, the generated file's
# platform-guard logic lives in exactly one place.
_PLATFORM_GUARD_RE = re.compile(
    r'#if !defined\(_WIN32\)\s*\n#error[^\n]*\n#endif\s*\n', re.MULTILINE
)


def _strip_pragma_once(text: str) -> str:
    lines = text.splitlines(keepends=True)
    out = []
    removed = False
    for line in lines:
        if not removed and line.strip() == "#pragma once":
            removed = True
            continue
        out.append(line)
    return "".join(out)


def _strip_local_includes(text: str) -> str:
    return "\n".join(
        line for line in text.splitlines() if not _LOCAL_INCLUDE_RE.match(line)
    ) + "\n"


def _strip_platform_guard(text: str) -> str:
    return _PLATFORM_GUARD_RE.sub("", text, count=1)


def build() -> str:
    parts = [
        "#pragma once\n",
        "// =========================================================================\n",
        "// animus_release.hpp -- Animus Core v1.0 single-header release\n",
        "//\n",
        "// GENERATED FILE -- do not edit directly. Produced by amalgamate.py from\n",
        "// the four source headers below; re-run `python amalgamate.py` after any\n",
        "// change to those originals and commit the regenerated output alongside.\n",
        f"// Generated: {datetime.date.today().isoformat()}\n",
        "//\n",
        "// Sections:\n",
    ]
    for fname, label, portable in _SOURCES:
        tag = "portable" if portable else "Windows-only"
        parts.append(f"//   - {fname} ({tag}) -- {label}\n")
    parts.append(
        "//\n"
        "// The Windows-only sections (mTLS transport, Raft-lite clustering) are\n"
        "// wrapped in `#if defined(_WIN32)` below, so a non-Windows translation\n"
        "// unit can still #include this single file and use the portable core\n"
        "// engine + RBAC/multi-tenancy layer; animus::transport / animus::cluster\n"
        "// are simply unavailable there, matching what the standalone headers'\n"
        "// own `#error` guards already document.\n"
        "// =========================================================================\n\n"
    )

    for fname, label, portable in _SOURCES:
        src = (_HERE / fname).read_text(encoding="utf-8")
        src = _strip_pragma_once(src)
        src = _strip_local_includes(src)
        if not portable:
            src = _strip_platform_guard(src)

        banner = (
            "\n// -------------------------------------------------------------------------\n"
            f"// {fname} -- {label}\n"
            "// -------------------------------------------------------------------------\n"
        )
        parts.append(banner)
        if not portable:
            # _MSC_VER, not just _WIN32 -- see module docstring: this
            # section's certificate loading uses an MSVC-only
            # std::ifstream(std::wstring, ...) overload that MinGW/libstdc++
            # does not have, so MinGW g++ on Windows (which does define
            # _WIN32) would otherwise still fail to compile this section.
            parts.append("#if defined(_WIN32) && defined(_MSC_VER)\n")
            parts.append(src)
            parts.append(f"#endif // defined(_WIN32) && defined(_MSC_VER) -- end of {fname}\n")
        else:
            parts.append(src)

    return "".join(parts)


def main() -> None:
    content = build()
    _OUTPUT.write_text(content, encoding="utf-8", newline="\n")
    line_count = content.count("\n") + 1
    print(f"[amalgamate.py] wrote {_OUTPUT} ({line_count} lines)")


if __name__ == "__main__":
    main()
