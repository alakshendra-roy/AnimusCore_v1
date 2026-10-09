#!/usr/bin/env python3
"""Static zero-allocation audit of the ring hot paths.

Compiles nothing itself: it reads an object file built from
tests/hotpath_audit_probe.cpp, disassembles it with GNU objdump, and walks the
call graph from every `animus_probe_*` symbol. The audit FAILS if any path from
a probe reaches an allocator or a C++ exception-throw entry point:

    malloc / calloc / realloc / free / posix_memalign / aligned_alloc / memalign
    operator new / new[] / delete / delete[]  (every Itanium-mangled overload)
    _aligned_malloc / _aligned_free           (Windows CRT)
    __cxa_allocate_exception / __cxa_throw / _Unwind_*

and it lists every EXTERNAL symbol the hot paths do reference (expected: memcpy
and friends) so the evidence is the full set of callees, not just a clean bill.

Negative control: `animus_negctl_*` symbols allocate deliberately. The audit
must flag every one of them; if it does not, the audit cannot see allocations
and exits non-zero rather than reporting a meaningless "clean".

Usage:
    scripts/audit_hotpath_symbols.py --object build/hotpath_audit_probe.o [--objdump objdump] [--json out.json]

Works on ELF (Linux, g++/clang) and COFF (MinGW g++) objects. MSVC objects are
not covered (dumpbin's format differs); on MSVC the runtime layers in
tests/eval_harness.cpp and tests/soak_shm_mpmc.cpp are the evidence.
Python 3.8+, standard library only.
"""
import argparse
import json
import re
import subprocess
import sys

# Itanium-mangled operator new / new[] / delete / delete[] are matched by PREFIX
# (_Znw, _Zna, _Zdl, _Zda), not enumerated: the size parameter mangles as `m` on
# LP64 Linux but `y` on LLP64 Windows, and the aligned / nothrow overloads add
# further suffixes -- a list of full names silently misses whichever one a
# platform happens to emit.
DENY = re.compile(
    r"^(?:_?malloc|_?calloc|_?realloc|_?free|posix_memalign|aligned_alloc|memalign|valloc|_aligned_malloc|_aligned_free|"
    r"_Z(?:nw|na|dl|da).*|"
    r"__cxa_allocate_exception|__cxa_throw|__cxa_rethrow|__cxa_begin_catch|_Unwind_.*|__gxx_personality.*|"
    r"__wrap_.*|__real_.*)(?:@.*)?$"
)
ALLOWED_EXTERNAL = {"memcpy", "memmove", "memset", "memcmp", "__stack_chk_fail", "_mm_pause"}

FUNC_RE = re.compile(r"^[0-9a-fA-F]+ <(?P<name>[^>]+)>:\s*$")
INSN_RE = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?P<mnem>[a-z][a-z0-9.]*)\s*(?P<ops>.*)$")
TARGET_RE = re.compile(r"<(?P<sym>[^>+]+)(?:\+0x[0-9a-fA-F]+)?>")
RELOC_RE = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?:R_[A-Z0-9_]+|IMAGE_REL_[A-Z0-9_]+)\s+(?P<sym>\S+)")
CALLISH = {"call", "callq", "jmp", "jmpq", "bl", "b", "blr", "br", "tail"}


def disassemble(objdump, path):
    out = subprocess.run([objdump, "-dr", "--no-show-raw-insn", path], check=True, capture_output=True, text=True).stdout
    funcs = {}
    cur = None
    for line in out.splitlines():
        m = FUNC_RE.match(line)
        if m:
            cur = m.group("name")
            funcs.setdefault(cur, set())
            continue
        if cur is None:
            continue
        r = RELOC_RE.match(line)
        if r:
            sym = re.split(r"[+-]0x", r.group("sym"))[0]
            if not sym.startswith(".") and not sym.startswith("__imp_"):
                funcs[cur].add(sym)
            elif sym.startswith("__imp_"):
                funcs[cur].add(sym[len("__imp_"):])
            continue
        i = INSN_RE.match(line)
        if i and i.group("mnem") in CALLISH:
            t = TARGET_RE.search(i.group("ops"))
            if t:
                funcs[cur].add(t.group("sym"))
    return funcs


def reach(funcs, root):
    """All symbols reachable from `root` through defined functions."""
    seen, stack = set(), [root]
    while stack:
        f = stack.pop()
        for callee in funcs.get(f, ()):
            if callee not in seen:
                seen.add(callee)
                if callee in funcs:  # defined in this object: follow it
                    stack.append(callee)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--object", required=True)
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--json")
    args = ap.parse_args()

    funcs = disassemble(args.objdump, args.object)
    probes = sorted(f for f in funcs if f.startswith("animus_probe_"))
    negctls = sorted(f for f in funcs if f.startswith("animus_negctl_"))
    if not probes:
        print("audit: no animus_probe_* symbols found in the object (was it built from tests/hotpath_audit_probe.cpp?)", file=sys.stderr)
        return 2

    # Record the object's file name, not where it happened to be built: the JSON is evidence a
    # client reads, and a build directory under someone's home is not part of what was audited.
    report = {"object": re.split(r"[\\/]", args.object)[-1], "probes": {}, "negative_controls": {}}
    failed = []
    print(f"Static hot-path allocation audit of {args.object}")
    print(f"  {len(probes)} hot-path symbols, {len(negctls)} negative controls\n")
    all_external = set()
    for p in probes:
        reached = reach(funcs, p)
        denied = sorted(s for s in reached if DENY.match(s))
        external = sorted(s for s in reached if s not in funcs and not DENY.match(s))
        all_external.update(external)
        internal = sorted(s for s in reached if s in funcs)
        unexpected = sorted(s for s in external if s not in ALLOWED_EXTERNAL)
        status = "FAIL" if denied else "ok"
        if denied:
            failed.append(p)
        print(f"  [{status:>4}] {p}")
        print(f"         out-of-line callees in this object: {len(internal)}   external symbols: {', '.join(external) or 'none'}")
        if denied:
            print(f"         ALLOCATOR / THROW REACHABLE: {', '.join(denied)}")
        if unexpected:
            print(f"         note: external symbols outside the expected set: {', '.join(unexpected)}")
        report["probes"][p] = {"allocator_reachable": denied, "external": external, "internal_callees": len(internal)}

    print("\n  Negative controls (these MUST be flagged):")
    blind = []
    for n in negctls:
        reached = reach(funcs, n)
        denied = sorted(s for s in reached if DENY.match(s))
        flagged = bool(denied)
        if not flagged:
            blind.append(n)
        print(f"  [{'FLAGGED' if flagged else 'MISSED':>7}] {n}  -> {', '.join(denied) or 'nothing'}")
        report["negative_controls"][n] = {"flagged": flagged, "via": denied}
    if not negctls:
        blind.append("(no negative controls present)")

    ok = not failed and not blind
    report["verdict"] = "PASS" if ok else "FAIL"
    report["external_symbols_referenced_by_hot_paths"] = sorted(all_external)
    print()
    if blind:
        print("AUDIT INVALID: the negative controls were not flagged -- the audit cannot see allocations:", ", ".join(blind))
    if failed:
        print("AUDIT FAILED: an allocator or exception entry point is reachable from:", ", ".join(failed))
    if ok:
        print(f"AUDIT PASSED: no allocator or throw path is reachable from any of {len(probes)} hot-path symbols; "
              f"all {len(negctls)} negative controls were flagged.")
        print(f"  External symbols referenced by the hot paths: {', '.join(sorted(all_external)) or 'none'}")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
