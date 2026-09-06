#!/usr/bin/env bash
# Starts Livephoria, setting up a virtualenv the first time.
set -e
cd "$(dirname "$0")"
[ -d .venv ] || python3 -m venv .venv
./.venv/bin/pip install -q -r requirements-dev.txt
[ -f livephoria.db ] || ./.venv/bin/python scripts/seed_demo.py
echo "Livephoria -> http://127.0.0.1:8000"
exec ./.venv/bin/uvicorn app.main:app --port 8000 --reload
