"""setuptools entry point for the animus-core wheel.

Static metadata lives in pyproject.toml (PEP 621); this file adds the one
piece pyproject.toml can't express declaratively -- copying the compiled
native engine library from its MSBuild/CMake output directory into the
animus/ package directory before the build, so `pip wheel .` / `pip
install .` produce an artifact that carries the native binary inside the
wheel instead of depending on a sibling source checkout
(animus.bindings.load_native_library already checks the package directory
first for exactly this reason).

Two binaries can satisfy this (see animus/bindings.py's identical search
order): the portable AnimusNative build (CMakeLists.txt, any platform) is
preferred over the legacy MSVC-only AnimusCore_v1 build
(AnimusCore_v1.vcxproj) when both are present.
"""
import shutil
import sys
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py

_REPO_ROOT = Path(__file__).parent
_PACKAGE_DIR = _REPO_ROOT / "animus"

# Each platform lists (lib_name, [candidate build-output paths]) pairs in
# preference order: AnimusNative (CMake) first, AnimusCore_v1 (vcxproj,
# Windows-only) as the legacy fallback.
_NATIVE_LIB_CANDIDATES = {
    "win32": [
        ("AnimusNative.dll", [
            _REPO_ROOT / "build" / "AnimusNative.dll",
            _REPO_ROOT / "build" / "Release" / "AnimusNative.dll",
        ]),
        ("AnimusCore_v1.dll", [
            _REPO_ROOT / "AnimusCore_v1" / "x64" / "Release" / "AnimusCore_v1.dll",
            _REPO_ROOT / "x64" / "Release" / "AnimusCore_v1.dll",
        ]),
    ],
    "linux": [
        ("libanimus_native.so", [
            _REPO_ROOT / "build" / "libanimus_native.so",
        ]),
        ("libAnimusCore.so", [
            _REPO_ROOT / "AnimusCore_v1" / "libAnimusCore.so",
            _REPO_ROOT / "libAnimusCore.so",
        ]),
    ],
    "darwin": [
        ("libanimus_native.dylib", [
            _REPO_ROOT / "build" / "libanimus_native.dylib",
        ]),
        ("libAnimusCore.dylib", [
            _REPO_ROOT / "AnimusCore_v1" / "libAnimusCore.dylib",
            _REPO_ROOT / "libAnimusCore.dylib",
        ]),
    ],
}


class build_py(_build_py):
    """Stages the compiled native library into animus/ before packaging."""

    def run(self) -> None:
        self._stage_native_library()
        super().run()

    def _stage_native_library(self) -> None:
        if sys.platform.startswith("win32"):
            platform_key = "win32"
        elif sys.platform.startswith("linux"):
            platform_key = "linux"
        elif sys.platform.startswith("darwin"):
            platform_key = "darwin"
        else:
            return

        for lib_name, candidates in _NATIVE_LIB_CANDIDATES[platform_key]:
            dest = _PACKAGE_DIR / lib_name
            if dest.exists():
                return
            for candidate in candidates:
                if candidate.exists():
                    shutil.copy2(candidate, dest)
                    print(f"[setup.py] Staged native library: {candidate} -> {dest}")
                    return

        print(
            f"[setup.py] WARNING: no native library (AnimusNative.* or "
            f"AnimusCore_v1.*) found in any known build-output location; "
            f"the wheel will be built without one, and animus.AnimusBindings "
            f"will fall back to the pure-Python engine at import time. Build "
            f"CMakeLists.txt or AnimusCore_v1.slnx first for a "
            f"native-accelerated wheel."
        )


setup(cmdclass={"build_py": build_py})
