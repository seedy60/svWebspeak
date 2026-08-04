@echo off
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
call %VCVARS% x86 >nul
cd /d "%~dp0"
cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS svindex.c /link user32.lib winmm.lib advapi32.lib /out:svindex.exe
if errorlevel 1 exit /b 1
del /q svindex.obj 2>nul
echo Built svindex.exe
