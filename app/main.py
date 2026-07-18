"""
TRU Scouting Engine — FastAPI entrypoint.

Module 1  → /api/v1/scout/analyze-film   (Gemini native video ingestion)
Modules 2/3 → app.services.metric_sieve   (hard-coded positional matrices)
Module 4  → /api/v1/scout/truth-report   (combined film + sieve output)
Module 5  → db/init_schema.sql           (relational threshold matrix)
"""
import json
import os
from pathlib import Path

from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from google import genai
from google.genai import types

from typing import Optional

from app.core.config import get_settings
from app.models.schemas import AthleteCreate, GradeDown, MakeupGrades, Position, SieveResult
from app.services.film_grading import build_scouting_prompt
from app.services.makeup_grade import average_makeup_grade, grade_down
from app.services.metric_sieve import run_sieve

settings = get_settings()
app = FastAPI(title="TRU_Scouting_Engine_Backend", version="1.0.0")

# Allow the local demo panel (file:// or localhost) to call the API
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],  # tighten to your frontend origin in production
    allow_methods=["*"],
    allow_headers=["*"],
)

# Native Google GenAI client — reads GEMINI_API_KEY from environment
ai_client = genai.Client()

Path(settings.UPLOAD_TMP_DIR).mkdir(parents=True, exist_ok=True)

ALLOWED_EXTS = (".mp4", ".mov", ".avi")
YOUTUBE_HOSTS = ("youtube.com", "www.youtube.com", "m.youtube.com", "youtu.be")


def _is_youtube_url(url: str) -> bool:
    from urllib.parse import urlparse
    host = urlparse(url).hostname or ""
    return host.lower() in YOUTUBE_HOSTS


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
    file: Optional[UploadFile] = File(None),
    youtube_url: Optional[str] = Form(None),
    position: Position = Form(Position.DB),
):
    """Module 1: native Gemini video ingestion → structured film grades.
    Accepts either an uploaded file OR a YouTube URL (game film posted
    online) — Gemini fetches YouTube URLs natively, no download needed."""
    if bool(file) == bool(youtube_url):
        raise HTTPException(
            status_code=400,
            detail="Provide exactly one of: a video file upload, or a youtube_url.",
        )

    if youtube_url:
        if not _is_youtube_url(youtube_url):
            raise HTTPException(status_code=400, detail="Not a valid YouTube URL.")
        video_part = types.Part(file_data=types.FileData(file_uri=youtube_url))
        return await _run_film_analysis(video_part, position)

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
        return await _run_film_analysis(video_file, position)
    finally:
        if os.path.exists(video_path):
            os.remove(video_path)


async def _run_film_analysis(video_content, position: Position) -> dict:
    try:
        response = ai_client.models.generate_content(
            model=settings.GEMINI_MODEL,
            contents=[video_content, build_scouting_prompt(position)],
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
        }

    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/v1/scout/truth-report")
async def truth_report(
    position: Position = Form(Position.DB),
    athlete_json: str = Form(..., description="AthleteCreate payload as JSON string"),
    makeup_json: Optional[str] = Form(None, description="MakeupGrades payload as JSON string"),
    file: Optional[UploadFile] = File(None),
    youtube_url: Optional[str] = Form(None),
):
    """Module 4: Truth Report Panel — combines the metric sieve (hard laser
    thresholds), the Profile & Makeup grade-down, and Gemini film analysis
    into one user-facing evaluation."""
    try:
        athlete = AthleteCreate(**json.loads(athlete_json))
    except Exception as e:
        raise HTTPException(status_code=422, detail=f"Bad athlete payload: {e}")

    makeup: Optional[GradeDown] = None
    if makeup_json:
        try:
            overall = average_makeup_grade(MakeupGrades(**json.loads(makeup_json)))
        except Exception as e:
            raise HTTPException(status_code=422, detail=f"Bad makeup payload: {e}")
        if overall is not None:
            makeup = grade_down(overall)

    sieve = await metric_sieve(athlete)
    film = await analyze_player_film(file=file, youtube_url=youtube_url, position=position)

    return {
        "success": True,
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
    }
