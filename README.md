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
└── app/
    ├── main.py               # Modules 1, 4 & 6 — Gemini ingestion + Truth Report + Makeup Grade routes
    ├── core/
    │   ├── config.py         # typed .env loader
    │   └── db.py             # asyncpg pool, lifespan-managed
    ├── models/
    │   └── schemas.py        # Pydantic validation models
    ├── services/
    │   ├── metric_sieve.py   # Modules 2 & 3 — positional matrices as code
    │   ├── film_grading.py   # Module 1 — Gemini scouting prompt builder
    │   ├── makeup_grade.py   # Module 6 — Profile & Makeup grade-down logic
    │   └── repository.py     # DB reads/writes for athletes, film uploads, evaluations
    └── routers/              # (split routes here as the API grows)
```

## Run it
```bash
cp .env.example .env          # add your GEMINI_API_KEY + DATABASE_URL
pip install -r requirements.txt
psql "$DATABASE_URL" -f db/init_schema.sql    # or run init_schema.sql via any Postgres client
uvicorn app.main:app --reload
```

Any Postgres works, including a free hosted one (e.g. [Neon](https://neon.tech)) — just
set `DATABASE_URL` in `.env` to its connection string.

## Endpoints
- `GET  /api/v1/health`
- `POST /api/v1/scout/metric-sieve`   — thresholds only, no film, not persisted
- `POST /api/v1/scout/makeup-grade`   — Profile & Makeup rubric, grade-down across classifications, not persisted
- `POST /api/v1/scout/analyze-film`   — Gemini native video upload
- `POST /api/v1/scout/truth-report`   — sieve + film combined; saves the athlete, film upload, and evaluation
- `GET  /api/v1/athletes`   — every saved athlete, most recent first
- `GET  /api/v1/athletes/{id}`   — one athlete's profile
- `GET  /api/v1/athletes/{id}/evaluations`   — an athlete's full evaluation history
- `GET  /api/v1/evaluations/{id}`   — one evaluation in detail
