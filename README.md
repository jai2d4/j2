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
    │   └── config.py         # typed .env loader
    ├── models/
    │   └── schemas.py        # Pydantic validation models
    ├── services/
    │   ├── metric_sieve.py   # Modules 2 & 3 — positional matrices as code
    │   ├── film_grading.py   # Module 1 — Gemini scouting prompt builder
    │   └── makeup_grade.py   # Module 6 — Profile & Makeup grade-down logic
    └── routers/              # (split routes here as the API grows)
```

## Run it
```bash
cp .env.example .env          # add your GEMINI_API_KEY + DB password
pip install -r requirements.txt
psql -U tru_admin -d tru_scouting -f db/init_schema.sql
uvicorn app.main:app --reload
```

## Endpoints
- `GET  /api/v1/health`
- `POST /api/v1/scout/metric-sieve`   — thresholds only, no film
- `POST /api/v1/scout/makeup-grade`   — Profile & Makeup rubric, grade-down across classifications
- `POST /api/v1/scout/analyze-film`   — Gemini native video upload
- `POST /api/v1/scout/truth-report`   — sieve + film combined
