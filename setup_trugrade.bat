@echo off
setlocal
cd /d "%~dp0"

where python >nul 2>nul || (
  echo Python 3.11 or newer is required.
  exit /b 1
)

if not exist ".tmp" mkdir ".tmp"
set "TEMP=%CD%\.tmp"
set "TMP=%CD%\.tmp"

if not exist ".venv\Scripts\python.exe" python -m venv .venv
call ".venv\Scripts\activate.bat"
python -m pip install --upgrade pip
python -m pip install -r requirements-dev.txt

if not exist ".env" copy ".env.example" ".env" >nul

rem The Phase 1 standalone frontend has no third-party npm dependencies.

echo TruGrade Phase 1 setup complete.
endlocal
