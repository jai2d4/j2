<p align="center"><img src="assets/trugrade-logo.jpg" alt="TruGrade" width="360"></p>

# TRU Scouting Engine

## Structure
```
tru-scouting-engine/
├── .env.example              # env mapping (copy to .env)
├── requirements.txt
├── db/
│   └── init_schema.sql       # Module 5 — PostgreSQL schema + seeded position matrix
├── scripts/                  # (migrations / batch jobs)
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
