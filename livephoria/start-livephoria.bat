@echo off
REM Starts Livephoria, setting up a virtualenv the first time.
cd /d "%~dp0"
if not exist .venv (py -m venv .venv || python -m venv .venv)
.venv\Scripts\python -m pip install -q -r requirements-dev.txt
if not exist livephoria.db .venv\Scripts\python scripts\seed_demo.py
echo.
echo Livephoria -^> http://127.0.0.1:8000
echo.
.venv\Scripts\python -m uvicorn app.main:app --port 8000 --reload
pause
