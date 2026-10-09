#!/usr/bin/env python3
"""Record the machine a verification run was measured on, as JSON.

Everything is read from the OS at run time, not typed in: CPU model, core and
thread counts, RAM, OS and kernel, whether the OS is a guest (WSL2 / VM), the
active clock source and power plan, the git commit under test, and a content
digest of the code under test. A latency figure without this record cannot be
compared or reproduced.

The digest is what lets a report prove that every evidence bundle was measured
on the SAME source tree even when that tree is not (yet) a commit: it hashes
the headers, the harnesses and the build files, with line endings normalised so
a Windows and a Linux checkout of the same tree agree.

Usage:  scripts/collect_environment.py [--out environment.json] [--tag NAME]
Python 3.8+, standard library only. Works on Linux (incl. WSL2) and Windows.
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import platform
import re
import subprocess
import sys

# The code under test: every C++ source and header CMake compiles, the build files, and the audit script.
DIGEST_GLOBS = ("include/**/*.hpp", "tests/*.cpp", "tests/*.hpp", "AnimusCore_v1/*.cpp", "AnimusCore_v1/*.hpp",
                "adapters/**/*.cpp", "adapters/**/*.hpp", "adapters/**/*.h", "benchmarks/*.cpp",
                "CMakeLists.txt", "Makefile", "scripts/audit_hotpath_symbols.py")
NUL = bytes([0])
CRLF = bytes([13, 10])
LF = bytes([10])


def run(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30, **kw).stdout.strip()
    except Exception:  # noqa: BLE001 -- best effort, absence is recorded as null
        return None


def read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def powershell(expr):
    return run(["powershell", "-NoProfile", "-Command", expr])


def linux_info():
    cpuinfo = read("/proc/cpuinfo") or ""
    model = re.search(r"^model name\s*:\s*(.+)$", cpuinfo, re.M)
    flags = re.search(r"^flags\s*:\s*(.+)$", cpuinfo, re.M)
    flagset = set(flags.group(1).split()) if flags else set()
    lscpu = run(["lscpu"]) or ""

    def lscpu_field(name):
        m = re.search(r"^%s:\s*(.+)$" % re.escape(name), lscpu, re.M)
        return m.group(1).strip() if m else None

    meminfo = read("/proc/meminfo") or ""
    mem = re.search(r"^MemTotal:\s*(\d+) kB", meminfo, re.M)
    version = read("/proc/version") or ""
    osr = read("/etc/os-release") or ""
    pretty = re.search(r'^PRETTY_NAME="?(.*?)"?$', osr, re.M)
    thp = read("/sys/kernel/mm/transparent_hugepage/enabled")
    thp_mode = re.search(r"\[(\w+)\]", thp) if thp else None
    return {
        "os": "Linux",
        "os_release": pretty.group(1) if pretty else None,
        "kernel": platform.release(),
        "is_wsl": "microsoft" in version.lower(),
        "hypervisor": lscpu_field("Hypervisor vendor"),
        "virtualization_type": lscpu_field("Virtualization type"),
        "cpu_model": model.group(1).strip() if model else None,
        "logical_cpus": os.cpu_count(),
        "threads_per_core": lscpu_field("Thread(s) per core"),
        "cores_per_socket": lscpu_field("Core(s) per socket"),
        "sockets": lscpu_field("Socket(s)"),
        "l1d": lscpu_field("L1d cache"),
        "l2": lscpu_field("L2 cache"),
        "l3": lscpu_field("L3 cache"),
        "ram_gib": round(int(mem.group(1)) / 1024 / 1024, 1) if mem else None,
        "tsc_flags": sorted(f for f in flagset if f in ("constant_tsc", "nonstop_tsc", "tsc_known_freq", "tsc_reliable", "rdtscp")),
        "clocksource": (read("/sys/devices/system/clocksource/clocksource0/current_clocksource") or "").strip() or None,
        "transparent_hugepage": thp_mode.group(1) if thp_mode else None,
        "dev_shm": (run(["df", "-h", "/dev/shm"]) or "").splitlines()[-1:] or None,
    }


def windows_info():
    cpu = powershell("Get-CimInstance Win32_Processor | Select-Object -First 1 Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed | ConvertTo-Json -Compress")
    cs = powershell("Get-CimInstance Win32_ComputerSystem | Select-Object -First 1 TotalPhysicalMemory,Model,HypervisorPresent | ConvertTo-Json -Compress")
    osi = powershell("Get-CimInstance Win32_OperatingSystem | Select-Object -First 1 Caption,Version,BuildNumber | ConvertTo-Json -Compress")
    plan = run(["powercfg", "/getactivescheme"])
    out = {"os": "Windows", "kernel": platform.version(), "is_wsl": False}
    try:
        c = json.loads(cpu)
        out.update(cpu_model=c.get("Name", "").strip(), physical_cores=c.get("NumberOfCores"),
                   logical_cpus=c.get("NumberOfLogicalProcessors"), max_clock_mhz=c.get("MaxClockSpeed"))
    except (TypeError, ValueError):
        out.update(cpu_model=platform.processor(), logical_cpus=os.cpu_count())
    try:
        s = json.loads(cs)
        out.update(ram_gib=round(int(s["TotalPhysicalMemory"]) / 1024 ** 3, 1), machine_model=s.get("Model"),
                   hypervisor_present=s.get("HypervisorPresent"))
    except (TypeError, ValueError, KeyError):
        pass
    try:
        o = json.loads(osi)
        out.update(os_release="%s (build %s)" % (o.get("Caption", "").strip(), o.get("BuildNumber")))
    except (TypeError, ValueError):
        out.update(os_release=platform.platform())
    out["power_plan"] = plan.split("(", 1)[-1].rstrip(")") if plan else None
    return out


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source_digest():
    repo = repo_root()
    files = sorted({os.path.relpath(p, repo).replace(os.sep, "/") for g in DIGEST_GLOBS for p in glob.glob(os.path.join(repo, g), recursive=True)
                    if os.path.isfile(p)})
    h = hashlib.sha256()
    for rel in files:
        with open(os.path.join(repo, rel), "rb") as f:
            data = f.read().replace(CRLF, LF)  # the checkout's line endings must not change the digest
        h.update(rel.encode("utf-8") + NUL + data + NUL)
    return {"sha256": h.hexdigest(), "files": len(files)}


def git_info():
    repo = repo_root()
    head = run(["git", "-C", repo, "rev-parse", "--short=12", "HEAD"])
    if not head:                      # not a git checkout (a release kit, an unpacked archive): say so rather than claim a clean tree
        return {"commit": None, "working_tree_dirty": None}
    dirty = run(["git", "-C", repo, "status", "--porcelain"])
    return {"commit": head, "working_tree_dirty": bool(dirty) if dirty is not None else None}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out")
    ap.add_argument("--tag", default="")
    args = ap.parse_args()
    info = windows_info() if sys.platform.startswith("win") else linux_info()
    info.update(
        tag=args.tag,
        collected_utc=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        python=platform.python_version(),
        git=git_info(),
        source=source_digest(),
    )
    text = json.dumps(info, indent=2)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
