<p align="center"><img src="assets/trugrade-logo.jpg" alt="TruGrade" width="360"></p>

# TRU Scouting Engine

## TruGrade V2 migration status

Phase 1 establishes a safe standalone `frontend/` and stable `backend/`
entrypoint without changing the current interface or API behavior. The
existing FastAPI implementation remains under `app/` during the phased
migration. No Phase 2 grading-engine changes are included yet.

Phase 2 adds the official deterministic film-grading core under
`backend/grading/`: validated event/evidence models, editable JSON rules for
all 13 TruGrade positions, unknown-safe aggregation, confidence kept separate
from grade, and verified-only prospect profile inputs. The current UI and
production API remain unchanged. The audit found no React `src/lib/engine.ts`
or fantasy scoring implementation in this repository, so nothing was moved or
fabricated; a recovery note is retained under `backend/legacy/`.

Phase 3 adds chunked local uploads at `POST /api/videos/upload`, a local video
catalog at `GET /api/videos`, background extraction jobs at
`POST /api/analysis/start/{video_id}`, and durable progress polling at
`GET /api/analysis/status/{job_id}`. OpenCV samples frames at configurable
`ANALYSIS_FPS` (10 by default) while retaining source frame numbers and exact
timestamps. Uploaded film remains local and the upload request never waits for
frame extraction.

Phase 4 adds a lazy, swappable Ultralytics YOLO adapter and a two-stage
ByteTrack-style tracker. Normal pretrained YOLO recognizes players (`person`)
and football candidates (`sports ball`); custom football models may additionally
label officials without changing downstream code. Detection and persistent
track results are stored as JSON under local storage, with centers, bounding
boxes, velocity, speed, direction, confidence, and original timestamps.

On Windows, run `setup_trugrade.bat` once and then `run_trugrade.bat`.

## Structure
```
tru-scouting-engine/
├── frontend/                 # current standalone interface and build checks
├── backend/                  # stable V2 entrypoint and phased modules
├── .env.example              # env mapping (copy to .env)
├── requirements.txt
├── Dockerfile                # container build for deployment (Render/Railway/Fly/anywhere)
├── render.yaml                # Render Blueprint — one-click web service + free Postgres
├── db/
│   └── init_schema.sql       # Module 5 — PostgreSQL schema + seeded position matrix (idempotent)
├── scripts/
│   └── init_db.py            # applies init_schema.sql on container boot — safe to re-run
├── tests/                    # pytest suite (mocked Gemini + DB; no live services needed)
└── app/
    ├── main.py               # Modules 1, 4 & 6 — Gemini ingestion + Truth Report + Makeup Grade routes
    ├── core/
    │   ├── config.py         # typed .env loader
    │   ├── auth.py           # X-API-Key gate — no-op unless API_KEY is set
    │   └── db.py             # async SQLAlchemy engine/session (Module 5)
    ├── models/
    │   ├── schemas.py        # Pydantic request/response models
    │   └── orm.py            # SQLAlchemy models mirroring db/init_schema.sql
    ├── services/
    │   ├── metric_sieve.py   # Modules 2 & 3 — positional matrices as code
    │   ├── film_grading.py   # Module 1 — Gemini scouting prompt builder
    │   └── makeup_grade.py   # Module 6 — Profile & Makeup grade-down logic
    └── routers/
        ├── athletes.py       # Module 5 — athlete roster CRUD
        └── evaluations.py    # Module 5 — evaluation (Truth Report) history
```

## Run it
```bash
cp .env.example .env          # add your GEMINI_API_KEY + DB password
pip install -r requirements.txt
psql -U tru_admin -d tru_scouting -f db/init_schema.sql
uvicorn app.main:app --reload
```

The app still runs with a cold or unreachable database — `metric-sieve`, `makeup-grade`, and `analyze-film` never touch Postgres. `truth-report` best-effort saves its result and degrades to `"persisted": false` rather than failing if the DB is down. The athlete/evaluation CRUD routes do require the database and return `503` if it's unreachable.

## Deploy it (get a real public URL)
This ships with a [Render](https://render.com) Blueprint (`render.yaml` + `Dockerfile`) — Render's free tier needs no credit card.

1. Push this repo to your own GitHub account (fork it, or just use this one if you own it).
2. In the [Render dashboard](https://dashboard.render.com/blueprints), click **New > Blueprint** and point it at the repo. Render reads `render.yaml` and provisions:
   - a free PostgreSQL database, schema-migrated automatically on first boot (`scripts/init_db.py` runs before `uvicorn` starts — safe to re-run on every restart)
   - a free web service running the app via `Dockerfile`, wired to that database
3. Render will prompt for one value it can't infer: **`GEMINI_API_KEY`** — paste your Gemini key.
4. Optionally set **`API_KEY`** too, to lock the app behind a shared secret before sharing the URL (see Auth below). Leave it blank to keep the app open.
5. Deploy. Render gives you a `https://tru-scouting-engine-xxxx.onrender.com` URL — open it directly in a browser and you get the Truth Report panel itself (the app serves its own frontend at `/`), talking to the real API on the same origin. No separate hosting for the UI, no local file to open.

The free tier spins down after 15 minutes idle and takes ~30–60s to wake back up on the next request — normal for free hosting, not a bug. Upgrading the web service's plan in Render removes that.

No Render account yet? Sign up is free at [render.com](https://render.com) — that account (and clicking through the Blueprint prompts above) is the one step that has to happen on your end; nothing about it requires code changes.

### Alternative: Replit
A `.replit` file is included for **Import from GitHub → Deploy** on [replit.com](https://replit.com):

1. Create a Postgres database (Replit's built-in Postgres, or any external one — e.g. [Neon](https://neon.tech)) and copy its connection string.
2. In your Repl's **Secrets**, set `GEMINI_API_KEY` and `DATABASE_URL` (the connection string from step 1 — `postgres://user:pass@host:port/dbname` works as-is, no reformatting needed). `API_KEY` is optional, same as above.
3. Click **Deploy** (Autoscale). The `.replit` config installs `requirements.txt`, applies the schema on boot, and starts the app on Replit's assigned port.

`DATABASE_URL` and the individual `POSTGRES_*` variables both work — set whichever your host hands you, not both.

## Auth
Leave `API_KEY` unset in `.env` for local dev — every route is open. Set it before exposing the app anywhere else: every `/api/v1/scout/*`, `/api/v1/athletes`, and `/api/v1/evaluations` route then requires a matching `X-API-Key` header. `/api/v1/health` always stays open for uptime checks.

## Tests
```bash
pip install -r requirements-dev.txt
pytest
```
CI (`.github/workflows/ci.yml`) runs the same suite on every push/PR.

## Endpoints
- `GET  /api/v1/health` — always open, no auth
- `POST /api/v1/scout/metric-sieve` — thresholds only, no film
- `POST /api/v1/scout/makeup-grade` — Profile & Makeup rubric, grade-down across classifications
- `POST /api/v1/scout/analyze-film` — Gemini native video upload OR a YouTube URL (`youtube_url` form field, mutually exclusive with `file`); pass `player_identifier` (e.g. `"white jersey #12"`) to isolate one player on multi-player footage
- `POST /api/v1/scout/truth-report` — sieve + film combined; film may be an upload or a `youtube_url`; best-effort persists the athlete + evaluation; pass `athlete_id` to link to an existing athlete instead of creating a new one
- `POST /api/v1/athletes` / `GET /api/v1/athletes` / `GET /api/v1/athletes/{id}` — roster CRUD
- `GET  /api/v1/evaluations` (optional `?athlete_id=`) / `GET /api/v1/evaluations/{id}` — Truth Report history
