@echo off
setlocal
cd /d "%~dp0"

if not exist ".venv\Scripts\python.exe" (
  echo Run setup_trugrade.bat first.
  exit /b 1
)

start "TruGrade Backend" /min "%~dp0.venv\Scripts\python.exe" -m uvicorn backend.main:app --host 127.0.0.1 --port 8000
start "TruGrade Frontend" /min cmd /k "cd /d ""%~dp0frontend"" && python -m http.server 5173"

timeout /t 2 /nobreak >nul
start "" "http://127.0.0.1:8000"

echo TruGrade is starting at http://127.0.0.1:8000
endlocal
