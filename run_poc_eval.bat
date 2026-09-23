@echo off
setlocal enabledelayedexpansion

rem AnimusCore POC evaluation harness -- Windows entry point.
rem
rem Compiles poc_eval\poc_eval_harness.cpp (a self-contained benchmark
rem against AnimusCore_v1\animus.hpp, the core single-header engine -- no
rem other repository headers, no third-party dependency) with clang-cl or
rem cl.exe (MSVC 2019+), then runs a 10-second POC soak test and prints the
rem scorecard.
rem
rem Usage:
rem   run_poc_eval.bat [duration_seconds] [mpmc_producer_threads] [pin_base_core]
rem
rem All arguments are optional; defaults are 10 seconds, max(2,
rem logical_cores/2) producer threads, and no core pinning. Pass
rem pin_base_core (e.g. `2`) to pin every producer/consumer thread to a
rem distinct logical core via animus::sys::pin_current_thread_to_core_exclusive
rem for a pinned-vs-unpinned comparison -- if given, mpmc_producer_threads
rem must also be given (it is positional arg 2).

set "SCRIPT_DIR=%~dp0"
set "SRC=%SCRIPT_DIR%poc_eval\poc_eval_harness.cpp"
set "OUT=%SCRIPT_DIR%poc_eval\poc_eval_harness.exe"
set "DURATION=%~1"
if "%DURATION%"=="" set "DURATION=10"
set "PRODUCERS=%~2"
set "PIN_BASE_CORE=%~3"
if not "%PIN_BASE_CORE%"=="" if "%PRODUCERS%"=="" (
    echo error: pin_base_core given as arg 3 but mpmc_producer_threads ^(arg 2^) was not. 1>&2
    echo Usage: run_poc_eval.bat [duration_seconds] [mpmc_producer_threads] [pin_base_core] 1>&2
    exit /b 1
)

rem Prefer clang-cl if present (matches the compiler this harness is
rem developed/validated against elsewhere in the repo -- see bench\); fall
rem back to cl.exe (MSVC 2019+), locating it via vswhere.exe if it is not
rem already on PATH (i.e. this is not already a Developer Command Prompt).
where clang-cl >nul 2>nul
if not errorlevel 1 (
    echo Using compiler: clang-cl
    clang-cl /O2 /std:c++17 /EHsc /W3 "%SRC%" /Fe:"%OUT%"
    if errorlevel 1 exit /b 1
    goto :run
)

where cl.exe >nul 2>nul
if not errorlevel 1 goto :have_cl

echo cl.exe not on PATH -- locating a Visual Studio 2019+ install via vswhere...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo error: neither clang-cl nor cl.exe found, and vswhere.exe is missing. 1>&2
    echo Install Visual Studio 2019+ ^(Desktop C++ workload^) or LLVM for Windows. 1>&2
    exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if "%VSINSTALL%"=="" (
    echo error: vswhere found no Visual Studio install with the C++ toolset. 1>&2
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl.exe >nul 2>nul
if errorlevel 1 (
    echo error: cl.exe still not found after vcvars64.bat. 1>&2
    exit /b 1
)

:have_cl
echo Using compiler: cl.exe
cl.exe /O2 /std:c++17 /EHsc /W3 /nologo "%SRC%" /Fe:"%OUT%" /Fo:"%SCRIPT_DIR%poc_eval\"
if errorlevel 1 exit /b 1

:run
echo -^> %OUT%
echo.
echo Running %DURATION%s POC soak test...
echo.

if not "%PIN_BASE_CORE%"=="" (
    "%OUT%" %DURATION% %PRODUCERS% %PIN_BASE_CORE%
) else if not "%PRODUCERS%"=="" (
    "%OUT%" %DURATION% %PRODUCERS%
) else (
    "%OUT%" %DURATION%
)

endlocal
