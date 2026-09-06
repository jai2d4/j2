@echo off
REM Starts Axiome, the control plane. Prints an admin key on first boot.
cd /d "%~dp0axiome"
if not exist .venv (py -m venv .venv || python -m venv .venv)
.venv\Scripts\python -m pip install -q -r requirements-dev.txt
echo.
echo Axiome -^> http://127.0.0.1:8200
echo.
.venv\Scripts\python -m uvicorn app.main:app --port 8200 --reload
pause
