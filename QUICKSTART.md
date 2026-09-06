# Running this on your PC

Two apps live here:

- **`/`** — Livephoria, the creator platform (port 8000)
- **`/axiome`** — Axiome, the control plane that watches it (port 8200)

Each runs on its own; Axiome is optional. You need **Python 3.11 or newer** and
nothing else — no Node, no database to install, no build step.

## Windows

Double-click **`start-axiome.bat`**, then **`start-livephoria.bat`**. Each opens a
terminal, sets itself up the first time, and starts.

Or from PowerShell:

```powershell
py -m venv .venv
.venv\Scripts\pip install -r requirements-dev.txt
.venv\Scripts\python scripts\seed_demo.py     # optional demo data
.venv\Scripts\uvicorn app.main:app --port 8000
```

## macOS / Linux

```bash
./start-axiome.sh          # terminal 1
./start-livephoria.sh      # terminal 2
```

Or by hand:

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements-dev.txt
python scripts/seed_demo.py                   # optional demo data
uvicorn app.main:app --port 8000
```

## Connecting the two

1. Start Axiome and open <http://127.0.0.1:8200>. It prints an admin key in its
   terminal on first boot — paste that into the unlock screen.
2. Start Livephoria with a control key, so Axiome is allowed to drive it:

   - Windows: `set AXIOME_CONTROL_KEY=pick-a-secret` before starting it
   - macOS/Linux: `AXIOME_CONTROL_KEY=pick-a-secret uvicorn app.main:app --port 8000`

   Or put `AXIOME_CONTROL_KEY=pick-a-secret` in a `.env` file next to
   `requirements.txt` — `start-livephoria` reads it.
3. In Axiome click **+ Add app**:
   - Name: `Livephoria`
   - Base URL: `http://127.0.0.1:8000`
   - Control key: the same secret
4. **Test connection** first — it says exactly what it found. Then **Add app**.

Livephoria now appears on the board and stays current, because Axiome polls it.
That is already a working connection.

To also have Livephoria push to Axiome (instant updates, plus events like urgent
moderation reports), copy the two lines Axiome shows you into Livephoria's `.env`
and restart it:

```
AXIOME_BASE_URL=http://127.0.0.1:8200
AXIOME_APP_KEY=axk_...
```

## Adding your other apps

Any app is connectable if it exposes three routes — an open `/api/v1/health`, and
`status` plus `metrics` under a control path behind a shared secret. `axiome/README.md`
gives the exact shapes, and Livephoria's `app/routers/control.py` is a worked
example of about 140 lines you can copy.

Apps that expose nothing yet still show up as *unreachable* once added, so you can
register everything now and wire each one up as you get to it.
