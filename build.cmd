@echo off
rem Build the host and package the NVDA add-on.
setlocal
cd /d "%~dp0"

call host\build.cmd
if errorlevel 1 (
  echo ERROR: host build failed
  exit /b 1
)
copy /y host\svwebspeak-host.exe addon\synthDrivers\svWebspeak\ >nul

if not exist addon\synthDrivers\svWebspeak\SVctl32.DLL (
  echo.
  echo WARNING: SVctl32.DLL is missing from addon\synthDrivers\svWebspeak\
  echo The engine binaries are not redistributable and are not in this repo.
  echo Copy SVctl32.DLL, SVENG32.DLL and Svspan32.dll from your pwWebSpeak
  echo installation - see the README.
  echo.
)

python -c "import zipfile,os;out='svWebspeak.nvda-addon';z=zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED);[z.write(os.path.join(r,f),os.path.relpath(os.path.join(r,f),'addon')) for r,_,fs in os.walk('addon') for f in fs if not f.endswith('.pyc') and '__pycache__' not in r];z.close();print('packaged '+out)"
