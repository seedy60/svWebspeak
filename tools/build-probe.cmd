@echo off
rem Build svprobe as a 32-bit binary - it must match SVctl32.DLL (PE32/i386).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist %VCVARS% (
  echo ERROR: vcvarsall.bat not found at %VCVARS%
  exit /b 1
)
call %VCVARS% x86 >nul
if errorlevel 1 (
  echo ERROR: failed to initialise the x86 build environment.
  exit /b 1
)
cd /d "%~dp0"
cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS svprobe.c /link user32.lib winmm.lib advapi32.lib /out:svprobe.exe
if errorlevel 1 exit /b 1
del /q svprobe.obj 2>nul
echo Built svprobe.exe
