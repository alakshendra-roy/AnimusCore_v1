import os
import sys
import ctypes

def load_native_library():
    """Dynamically loads the compiled C++ dynamic library (.dll / .so)."""
    base_dir = os.path.dirname(os.path.abspath(__file__))
    
    if sys.platform.startswith("win32"):
        lib_name = "AnimusCore_v1.dll"
    elif sys.platform.startswith("linux"):
        lib_name = "libAnimusCore.so"
    elif sys.platform.startswith("darwin"):
        lib_name = "libAnimusCore.dylib"
    else:
        raise OSError(f"Unsupported platform: {sys.platform}")

    search_paths = [
        os.path.join(base_dir, lib_name),
        os.path.join(base_dir, "..", "x64", "Release", lib_name),
        os.path.join(base_dir, "..", lib_name),
    ]

    for path in search_paths:
        if os.path.exists(path):
            return ctypes.CDLL(os.path.abspath(path))
            
    raise FileNotFoundError(f"Could not locate native library {lib_name}. Ensure it is compiled in Release mode.")
