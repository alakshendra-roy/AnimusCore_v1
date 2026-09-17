@echo off
setlocal

rem Standalone build script for the AnimusCore hot-path benchmark harness
rem (hotpath_bench.cpp). Zero external dependencies: compiles against the
rem single-header animus.hpp core (AnimusCore_v1\animus.hpp) plus the C++17
rem standard library and pthreads only. Requires g++ or clang++ on PATH
rem (e.g. MSYS2/MinGW-w64 or LLVM for Windows) -- not MSVC's cl.exe.

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"
set "SRC=%SCRIPT_DIR%hotpath_bench.cpp"
set "OUT=%SCRIPT_DIR%hotpath_bench.exe"

if not defined CXX (
    where g++ >nul 2>nul
    if not errorlevel 1 (
        set "CXX=g++"
    ) else (
        where clang++ >nul 2>nul
        if not errorlevel 1 (
            set "CXX=clang++"
        ) else (
            echo error: neither g++ nor clang++ found on PATH ^(set CXX explicitly^) 1>&2
            exit /b 1
        )
    )
)

echo Using compiler: %CXX%

"%CXX%" -O3 -std=c++17 -march=native -Wall -Wextra -pthread -I"%REPO_ROOT%\AnimusCore_v1" "%SRC%" -o "%OUT%"
if errorlevel 1 exit /b 1

echo -^> %OUT%
echo.
echo Run it:
echo   "%OUT%"

endlocal
