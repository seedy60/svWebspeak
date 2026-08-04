@echo off
rem softvoice-host must be 32-bit to match SVctl32.DLL (PE32/i386).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist %VCVARS% (
  echo ERROR: vcvarsall.bat not found at %VCVARS%
  exit /b 1
)
call %VCVARS% x86 >nul
cd /d "%~dp0"
cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS svwebspeak-host.c ^
   /link user32.lib winmm.lib ws2_32.lib advapi32.lib /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /out:svwebspeak-host.exe
if errorlevel 1 exit /b 1
del /q svwebspeak-host.obj 2>nul
echo Built svwebspeak-host.exe
