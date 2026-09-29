@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d C:\Users\Alaks\source\repos\AnimusCore_v1\animus_sandbox\build_x64
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DPython_EXECUTABLE=C:\Users\Alaks\source\repos\AnimusCore_v1\venv\Scripts\python.exe ..
if errorlevel 1 exit /b 1
ninja
