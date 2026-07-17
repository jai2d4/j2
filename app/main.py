"""
TRU Scouting Engine — FastAPI entrypoint.

Module 1  → /api/v1/scout/analyze-film   (Gemini native video ingestion)
Modules 2/3 → app.services.metric_sieve   (hard-coded positional matrices)
Module 4  → /api/v1/scout/truth-report   (combined film + sieve output)
Module 5  → db/init_schema.sql           (relational threshold matrix)
"""
import asyncio
import json
import os
from pathlib import Path
from uuid import UUID

import asyncpg
from fastapi import Depends, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from google import genai
from google.genai import types

from typing import Optional

from app.core.config import get_settings
from app.core.db import get_pool, lifespan
from app.models.schemas import (
    Athlete,
    AthleteCreate,
    EvaluationOut,
    GradeDown,
    MakeupGrades,
    Position,
    SieveResult,
    WebUpdate,
)
from app.services import repository
from app.services.film_grading import build_scouting_prompt
from app.services.improvement_plan import generate_improvement_plan
from app.services.makeup_grade import average_makeup_grade, grade_down
from app.services.metric_sieve import run_sieve
from app.services.web_search import find_athlete_updates, parse_published_date

# Sieve check names -> the AthleteCreate/web-search metric key they represent,
# so evaluation history and web-discovered numbers land on the same series.
_METRIC_KEY_MAP = {"forty_yard_dash": "forty_s", "pro_agility_shuttle": "shuttle_s"}

settings = get_settings()
app = FastAPI(title="TRU_Scouting_Engine_Backend", version="1.0.0", lifespan=lifespan)

# Allow the local demo panel (file:// or localhost) to call the API
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # tighten to your frontend origin in production
    allow_methods=["*"],
    allow_headers=["*"],
)

# Native Google GenAI client
ai_client = genai.Client(api_key=settings.GEMINI_API_KEY)

Path(settings.UPLOAD_TMP_DIR).mkdir(parents=True, exist_ok=True)

ALLOWED_EXTS = (".mp4", ".mov", ".avi")

# Gemini processes uploaded video server-side before it's usable in a prompt —
# poll until it leaves PROCESSING instead of using it immediately.
FILE_POLL_INTERVAL_S = 2
FILE_POLL_MAX_ATTEMPTS = 30


@app.get("/api/v1/health")
async def health():
    return {"status": "ok", "model": settings.GEMINI_MODEL, "env": settings.APP_ENV}


@app.post("/api/v1/scout/metric-sieve", response_model=SieveResult)
async def metric_sieve(athlete: AthleteCreate):
    """Modules 2 & 3: run laser-metric-first structural validation against
    the positional logic matrix. No film required."""
    return run_sieve(
        athlete.position,
        height_in=athlete.height_in,
        weight_lbs=athlete.weight_lbs,
        forty_s=athlete.forty_s,
        shuttle_s=athlete.shuttle_s,
        bench_lbs=athlete.bench_lbs,
        squat_lbs=athlete.squat_lbs,
        gpa=athlete.gpa,
        sat=athlete.sat,
        act=athlete.act,
    )


@app.post("/api/v1/scout/makeup-grade", response_model=GradeDown)
async def makeup_grade(grades: MakeupGrades):
    """Module 6: Profile & Makeup rubric — grades Size, Athletic Ability, Play
    History, Play Style, and Character against the P4 standard, then shows
    what that same overall grade equates to at each easier classification
    level (Group of 5, FCS, D2/D3/NAIA/JUCO)."""
    overall = average_makeup_grade(grades)
    if overall is None:
        raise HTTPException(status_code=422, detail="At least one category must be graded.")
    return grade_down(overall)


@app.post("/api/v1/scout/analyze-film")
async def analyze_player_film(
    file: UploadFile = File(...),
    position: Position = Form(Position.DB),
):
    """Module 1: native Gemini video ingestion → structured film grades."""
    if not file.filename or not file.filename.lower().endswith(ALLOWED_EXTS):
        raise HTTPException(status_code=400, detail="Invalid video format.")

    video_path = os.path.join(settings.UPLOAD_TMP_DIR, os.path.basename(file.filename))
    try:
        data = await file.read()
        if len(data) > settings.MAX_UPLOAD_MB * 1024 * 1024:
            raise HTTPException(status_code=413, detail="File exceeds upload limit.")
        with open(video_path, "wb") as buffer:
            buffer.write(data)

        video_file = ai_client.files.upload(file=video_path)
        for _ in range(FILE_POLL_MAX_ATTEMPTS):
            if video_file.state.name != "PROCESSING":
                break
            await asyncio.sleep(FILE_POLL_INTERVAL_S)
            video_file = ai_client.files.get(name=video_file.name)
        if video_file.state.name == "FAILED":
            raise HTTPException(status_code=502, detail="Gemini failed to process the uploaded video.")
        if video_file.state.name != "ACTIVE":
            raise HTTPException(
                status_code=504, detail="Timed out waiting for Gemini to process the uploaded video."
            )

        response = ai_client.models.generate_content(
            model=settings.GEMINI_MODEL,
            contents=[video_file, build_scouting_prompt(position)],
            config=types.GenerateContentConfig(response_mime_type="application/json"),
        )

        try:
            analysis = json.loads(response.text)
        except (json.JSONDecodeError, TypeError):
            analysis = {"raw": response.text}

        return {
            "success": True,
            "position": position.value,
            "analysis": analysis,
            "film_grades": analysis.get("film_grades", {}) if isinstance(analysis, dict) else {},
            "flags": analysis.get("flags", []) if isinstance(analysis, dict) else [],
            "gemini_file_id": video_file.name,
        }

    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
    finally:
        if os.path.exists(video_path):
            os.remove(video_path)


@app.post("/api/v1/scout/truth-report")
async def truth_report(
    file: UploadFile = File(...),
    position: Position = Form(Position.DB),
    athlete_json: str = Form(..., description="AthleteCreate payload as JSON string"),
    makeup_json: Optional[str] = Form(None, description="MakeupGrades payload as JSON string"),
    pool: asyncpg.Pool = Depends(get_pool),
):
    """Module 4: Truth Report Panel — combines the metric sieve (hard laser
    thresholds), the Profile & Makeup grade-down, and Gemini film analysis
    into one user-facing evaluation, and persists the athlete, film upload,
    and evaluation so the Truth Report can be looked up again later."""
    try:
        athlete = AthleteCreate(**json.loads(athlete_json))
    except Exception as e:
        raise HTTPException(status_code=422, detail=f"Bad athlete payload: {e}")

    makeup_grades_input: Optional[MakeupGrades] = None
    makeup: Optional[GradeDown] = None
    if makeup_json:
        try:
            makeup_grades_input = MakeupGrades(**json.loads(makeup_json))
        except Exception as e:
            raise HTTPException(status_code=422, detail=f"Bad makeup payload: {e}")
        overall = average_makeup_grade(makeup_grades_input)
        if overall is not None:
            makeup = grade_down(overall)

    sieve = await metric_sieve(athlete)
    film = await analyze_player_film(file=file, position=position)
    try:
        improvement_plan = generate_improvement_plan(
            ai_client, settings.GEMINI_MODEL, position, sieve, film["film_grades"], film["flags"]
        )
    except Exception as e:
        # Non-critical — the sieve/film results and persistence below are still valuable
        # even if this one extra Gemini call fails (e.g. rate limit).
        improvement_plan = {"raw": f"Improvement plan unavailable: {e}"}

    athlete_record = await repository.create_athlete(pool, athlete)
    film_record = await repository.create_film_upload(
        pool,
        athlete_id=athlete_record["id"],
        filename=file.filename,
        gemini_file_id=film.get("gemini_file_id"),
        mime_type=file.content_type,
        status="analyzed" if film.get("success") else "failed",
    )
    evaluation_record = await repository.create_evaluation(
        pool,
        athlete_id=athlete_record["id"],
        film_id=film_record["id"],
        position_evaluated=position,
        projected_tier=sieve.tier.value,
        physical_projection=film["analysis"] if isinstance(film["analysis"], dict) else None,
        metric_sieve_results=sieve.model_dump(),
        qualifying_tiers=[t.value for t in sieve.qualifying_tiers],
        is_game_changer=sieve.is_game_changer,
        game_changer_reason=sieve.game_changer_reason,
        makeup_grades=makeup_grades_input.model_dump() if makeup_grades_input else None,
        makeup_grade_down=makeup.model_dump() if makeup else None,
        improvement_plan=improvement_plan,
        model_used=settings.GEMINI_MODEL,
    )

    return {
        "success": True,
        "athlete_id": str(athlete_record["id"]),
        "evaluation_id": str(evaluation_record["id"]),
        "athlete": f"{athlete.first_name} {athlete.last_name}",
        "position": position.value,
        "projected_tier": sieve.tier.value,
        "qualifying_tiers": [t.value for t in sieve.qualifying_tiers],
        "hard_metrics_passed": sieve.hard_metrics_passed,
        "is_game_changer": sieve.is_game_changer,
        "game_changer_reason": sieve.game_changer_reason,
        "metric_sieve": sieve.model_dump(),
        "makeup_grade": makeup.model_dump() if makeup else None,
        "film_analysis": film["analysis"],
        "film_grades": film["film_grades"],
        "film_flags": film["flags"],
        "improvement_plan": improvement_plan,
    }


@app.get("/api/v1/athletes", response_model=list[Athlete])
async def list_athletes(pool: asyncpg.Pool = Depends(get_pool)):
    """Every athlete profile created so far, most recent first."""
    rows = await repository.list_athletes(pool)
    return [dict(r) for r in rows]


@app.get("/api/v1/athletes/{athlete_id}", response_model=Athlete)
async def get_athlete(athlete_id: UUID, pool: asyncpg.Pool = Depends(get_pool)):
    row = await repository.get_athlete(pool, athlete_id)
    if row is None:
        raise HTTPException(status_code=404, detail="Athlete not found.")
    return dict(row)


@app.delete("/api/v1/athletes/{athlete_id}", status_code=204)
async def delete_athlete(athlete_id: UUID, pool: asyncpg.Pool = Depends(get_pool)):
    """Deletes the athlete and cascades to their film uploads and evaluations."""
    deleted = await repository.delete_athlete(pool, athlete_id)
    if not deleted:
        raise HTTPException(status_code=404, detail="Athlete not found.")


@app.get("/api/v1/athletes/{athlete_id}/evaluations", response_model=list[EvaluationOut])
async def list_athlete_evaluations(athlete_id: UUID, pool: asyncpg.Pool = Depends(get_pool)):
    """Full evaluation history for one athlete — every Truth Report run against them."""
    rows = await repository.list_evaluations_for_athlete(pool, athlete_id)
    return [dict(r) for r in rows]


@app.get("/api/v1/evaluations/{evaluation_id}", response_model=EvaluationOut)
async def get_evaluation(evaluation_id: UUID, pool: asyncpg.Pool = Depends(get_pool)):
    row = await repository.get_evaluation(pool, evaluation_id)
    if row is None:
        raise HTTPException(status_code=404, detail="Evaluation not found.")
    return dict(row)


@app.post("/api/v1/athletes/{athlete_id}/search-updates", response_model=list[WebUpdate])
async def search_athlete_updates(athlete_id: UUID, pool: asyncpg.Pool = Depends(get_pool)):
    """Module 8: searches the web via Gemini for news/updated metrics on this
    athlete and saves whatever it finds for the progression chart."""
    athlete = await repository.get_athlete(pool, athlete_id)
    if athlete is None:
        raise HTTPException(status_code=404, detail="Athlete not found.")

    try:
        result = find_athlete_updates(
            ai_client, settings.GEMINI_MODEL,
            first_name=athlete["first_name"], last_name=athlete["last_name"],
            position=athlete["position"], school=athlete["school"], grad_year=athlete["grad_year"],
        )
    except Exception as e:
        raise HTTPException(status_code=502, detail=f"Web search failed: {e}")

    saved = []
    for update in result["updates"]:
        if not isinstance(update, dict):
            continue
        record = await repository.create_web_update(
            pool,
            athlete_id=athlete_id,
            source_title=update.get("title"),
            source_url=update.get("url"),
            published_at=parse_published_date(update.get("published_date")),
            summary=update.get("summary") or "No summary provided.",
            discovered_metrics=update.get("metrics") if isinstance(update.get("metrics"), dict) else None,
        )
        saved.append(dict(record))
    return saved


@app.get("/api/v1/athletes/{athlete_id}/updates", response_model=list[WebUpdate])
async def list_athlete_updates(athlete_id: UUID, pool: asyncpg.Pool = Depends(get_pool)):
    """Every web-discovered update saved for this athlete, most recent first."""
    rows = await repository.list_web_updates_for_athlete(pool, athlete_id)
    return [dict(r) for r in rows]


@app.get("/api/v1/athletes/{athlete_id}/progression")
async def get_athlete_progression(athlete_id: UUID, pool: asyncpg.Pool = Depends(get_pool)):
    """Per-metric time series combining every Truth Report's combine numbers
    with any web-discovered metric updates, for the progression chart."""
    evaluations = [dict(r) for r in await repository.list_evaluations_for_athlete(pool, athlete_id)]
    updates = [dict(r) for r in await repository.list_web_updates_for_athlete(pool, athlete_id)]

    series: dict[str, list[dict]] = {}
    for ev in evaluations:
        for check in (ev.get("metric_sieve_results") or {}).get("checks", []):
            value = check.get("athlete_value")
            if value is None:
                continue
            key = _METRIC_KEY_MAP.get(check["metric"], check["metric"])
            series.setdefault(key, []).append({
                "date": ev["created_at"].isoformat(), "value": value,
                "source": "evaluation", "label": "Truth Report",
            })
    for upd in updates:
        for key, value in (upd.get("discovered_metrics") or {}).items():
            if value is None:
                continue
            series.setdefault(key, []).append({
                "date": upd["created_at"].isoformat(), "value": value,
                "source": "web", "label": upd.get("source_title") or "Web update",
            })
    for points in series.values():
        points.sort(key=lambda p: p["date"])
    return series
