#!/usr/bin/env bash
# Starts Axiome, the control plane. Prints an admin key on first boot.
set -e
cd "$(dirname "$0")/axiome"
[ -d .venv ] || python3 -m venv .venv
./.venv/bin/pip install -q -r requirements-dev.txt
echo "Axiome -> http://127.0.0.1:8200"
exec ./.venv/bin/uvicorn app.main:app --port 8200 --reload
