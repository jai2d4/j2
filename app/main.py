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

from app.core.config import get_settings
from app.models.schemas import AthleteCreate, Position, SieveResult
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


def _build_scouting_prompt(position: str) -> str:
    return f"""
    You are an elite collegiate personnel director evaluating a prospect's film clip.
    The athlete plays the position of: {position}.
    Analyze the film and return a structured JSON object containing:
    1. physical_projection (frame estimation, length, operational weight)
    2. explosive_traits (hip fluidity, vertical break-on-ball, acceleration windows)
    3. mechanics_grade (scale 1-10 based on standard competitive technique)
    4. situational_attributes (on-field spatial awareness, contact balance)
    Return ONLY the JSON object, no prose.
    """


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

        response = ai_client.models.generate_content(
            model=settings.GEMINI_MODEL,
            contents=[video_file, _build_scouting_prompt(position.value)],
            config=types.GenerateContentConfig(response_mime_type="application/json"),
        )

        try:
            analysis = json.loads(response.text)
        except (json.JSONDecodeError, TypeError):
            analysis = {"raw": response.text}

        return {"success": True, "position": position.value, "analysis": analysis}

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
):
    """Module 4: Truth Report Panel — combines the metric sieve (hard laser
    thresholds) with Gemini film analysis into one user-facing evaluation."""
    try:
        athlete = AthleteCreate(**json.loads(athlete_json))
    except Exception as e:
        raise HTTPException(status_code=422, detail=f"Bad athlete payload: {e}")

    sieve = await metric_sieve(athlete)
    film = await analyze_player_film(file=file, position=position)

    return {
        "success": True,
        "athlete": f"{athlete.first_name} {athlete.last_name}",
        "position": position.value,
        "projected_tier": sieve.tier.value,
        "hard_metrics_passed": sieve.hard_metrics_passed,
        "is_game_changer": sieve.is_game_changer,
        "game_changer_reason": sieve.game_changer_reason,
        "metric_sieve": sieve.model_dump(),
        "film_analysis": film["analysis"],
    }
