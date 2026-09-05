"""
TRU Scouting Engine — FastAPI entrypoint.

Module 1  → /api/v1/scout/analyze-film   (Gemini native video ingestion)
Modules 2/3 → app.services.metric_sieve   (hard-coded positional matrices)
Module 4  → /api/v1/scout/truth-report   (combined film + sieve output)
Module 5  → db/init_schema.sql, app.core.db, app.routers.{athletes,evaluations}
Module 6  → /api/v1/scout/makeup-grade   (Profile & Makeup grade-down)
"""
import asyncio
import json
import logging
import os
from pathlib import Path
from uuid import UUID, uuid4

from fastapi import BackgroundTasks, Depends, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse
from google import genai
from google.genai import types
from pydantic import BaseModel, Field

from typing import Optional

from app.core.auth import require_api_key
from app.core.config import get_settings
from app.core.db import best_effort_session
from app.models import orm
from app.models.schemas import AthleteCreate, GradeDown, MakeupGrades, Position, SieveResult
from app.routers import athletes, evaluations
from app.services.film_grading import build_scouting_prompt
from app.services.makeup_grade import average_makeup_grade, grade_down
from app.services.metric_sieve import run_sieve

logger = logging.getLogger("tru.main")
settings = get_settings()
app = FastAPI(title="TRU_Scouting_Engine_Backend", version="1.0.0")
app.include_router(athletes.router)
app.include_router(evaluations.router)

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
FILM_JOBS: dict[str, dict] = {}


class PlayerLookupRequest(BaseModel):
    player_name: str = Field(..., min_length=2, max_length=128)
    school: Optional[str] = Field(None, max_length=128)


def _grounding_sources(response) -> list[dict]:
    """Return only URLs Gemini actually used for Google Search grounding."""
    found: list[dict] = []
    seen: set[str] = set()
    try:
        chunks = response.candidates[0].grounding_metadata.grounding_chunks or []
        for chunk in chunks:
            web = getattr(chunk, "web", None)
            uri = getattr(web, "uri", None)
            if uri and uri not in seen:
                seen.add(uri)
                found.append({"title": getattr(web, "title", None) or uri, "url": uri})
    except (AttributeError, IndexError, TypeError):
        pass
    return found


@app.post("/api/v1/scout/player-lookup", dependencies=[Depends(require_api_key)])
async def player_lookup(request: PlayerLookupRequest):
    """Look up public, sourced player facts without filling unknowns by inference."""
    school_hint = f' at or associated with "{request.school}"' if request.school else ""
    prompt = f"""
    Search the public web for the football player named "{request.player_name}"{school_hint}.
    Resolve identity carefully. Do not combine people with similar names. Return JSON only.
    Use null for every value that is not explicitly published by a source you found; never
    estimate, infer, or calculate a measurement or GPA. School colors must be official or
    clearly published. Numeric height_in must be inches and weight_lbs must be pounds.
    Return exactly these keys: full_name, school, school_colors (array of color names),
    grad_year, state, jersey_number, height_in, weight_lbs, forty_s, shuttle_s,
    vertical_in, gpa, and verification_note. The note must say which facts were found and
    which were not publicly verified.
    """
    try:
        response = await asyncio.to_thread(
            ai_client.models.generate_content,
            model=settings.GEMINI_MODEL,
            contents=prompt,
            config=types.GenerateContentConfig(
                tools=[types.Tool(google_search=types.GoogleSearch())],
                response_mime_type="application/json",
            ),
        )
        raw = (response.text or "").strip()
        if raw.startswith("```"):
            raw = raw.split("\n", 1)[-1].rsplit("```", 1)[0].strip()
        profile = json.loads(raw)
        if not isinstance(profile, dict):
            raise ValueError("lookup returned an invalid profile")
        profile["sources"] = _grounding_sources(response)
        return profile
    except (json.JSONDecodeError, ValueError) as exc:
        raise HTTPException(status_code=502, detail=f"Player lookup returned invalid data: {exc}")
    except Exception as exc:
        logger.exception("player lookup failed")
        raise HTTPException(status_code=502, detail=f"Player lookup failed: {exc}")


def _is_youtube_url(url: str) -> bool:
    from urllib.parse import urlparse
    host = urlparse(url).hostname or ""
    return host.lower() in YOUTUBE_HOSTS


@app.get("/api/v1/health")
async def health():
    return {"status": "ok", "model": settings.GEMINI_MODEL, "env": settings.APP_ENV}


_DEMO_PATH = Path(__file__).resolve().parent.parent / "frontend_demo" / "truth_report_demo.html"


@app.get("/", response_class=HTMLResponse, include_in_schema=False)
async def truth_report_panel():
    """Serves the Truth Report demo panel from the app's own origin, so its
    fetch() calls resolve as same-origin — works identically on localhost
    and on the deployed URL, no separate static host needed."""
    return _DEMO_PATH.read_text()


@app.post("/api/v1/scout/metric-sieve", response_model=SieveResult, dependencies=[Depends(require_api_key)])
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


@app.post("/api/v1/scout/makeup-grade", response_model=GradeDown, dependencies=[Depends(require_api_key)])
async def makeup_grade(grades: MakeupGrades):
    """Module 6: Profile & Makeup rubric — grades Size, Athletic Ability, Play
    History, Play Style, and Character against the P4 standard, then shows
    what that same overall grade equates to at each easier classification
    level (Group of 5, FCS, D2/D3/NAIA/JUCO)."""
    overall = average_makeup_grade(grades)
    if overall is None:
        raise HTTPException(status_code=422, detail="At least one category must be graded.")
    return grade_down(overall)


@app.post("/api/v1/scout/analyze-film", dependencies=[Depends(require_api_key)])
async def analyze_player_film(
    file: Optional[UploadFile] = File(None),
    youtube_url: Optional[str] = Form(None),
    position: Position = Form(Position.DB),
    player_identifier: Optional[str] = Form(
        None,
        description="Who to grade when the film shows multiple players — "
                     "e.g. 'white jersey #12' or 'DB, green #5'.",
    ),
):
    """Module 1: native Gemini video ingestion → structured film grades.
    Accepts either an uploaded file OR a YouTube URL (game film posted
    online, including a cell-phone clip uploaded straight to YouTube) —
    Gemini fetches YouTube URLs natively, no download needed. On footage
    with multiple players (a highlight reel, a full game), pass
    player_identifier so grading locks onto the right athlete."""
    if bool(file) == bool(youtube_url):
        raise HTTPException(
            status_code=400,
            detail="Provide exactly one of: a video file upload, or a youtube_url.",
        )

    if youtube_url:
        if not _is_youtube_url(youtube_url):
            raise HTTPException(status_code=400, detail="Not a valid YouTube URL.")
        video_part = types.Part(file_data=types.FileData(file_uri=youtube_url))
        return await _run_film_analysis(video_part, position, player_identifier)

    if not file.filename or not file.filename.lower().endswith(ALLOWED_EXTS):
        raise HTTPException(status_code=400, detail="Invalid video format.")

    video_path = os.path.join(settings.UPLOAD_TMP_DIR, os.path.basename(file.filename))
    try:
        data = await file.read()
        if len(data) > settings.MAX_UPLOAD_MB * 1024 * 1024:
            raise HTTPException(status_code=413, detail="File exceeds upload limit.")
        with open(video_path, "wb") as buffer:
            buffer.write(data)

        video_file = await asyncio.to_thread(ai_client.files.upload, file=video_path)
        video_file = await _wait_for_gemini_file(video_file)
        return await _run_film_analysis(video_file, position, player_identifier)
    finally:
        if os.path.exists(video_path):
            os.remove(video_path)


def _file_state_name(video_file) -> Optional[str]:
    """Return the Files API state as a stable uppercase string."""
    state = getattr(video_file, "state", None)
    if state is None:
        return None
    name = getattr(state, "name", None)
    return str(name or state).upper()


async def _wait_for_gemini_file(video_file):
    """Wait until Gemini has processed an uploaded video before grading it."""
    deadline = asyncio.get_running_loop().time() + settings.GEMINI_FILE_PROCESSING_TIMEOUT_S
    while True:
        state = _file_state_name(video_file)
        if state == "ACTIVE":
            return video_file
        if state == "FAILED":
            raise HTTPException(status_code=502, detail="Gemini could not process this video file.")
        if asyncio.get_running_loop().time() >= deadline:
            raise HTTPException(status_code=504, detail="Gemini video processing timed out. Please try again.")
        await asyncio.sleep(settings.GEMINI_FILE_POLL_INTERVAL_S)
        video_file = await asyncio.to_thread(ai_client.files.get, name=video_file.name)


async def _process_film_job(
    job_id: str,
    position: Position,
    player_identifier: Optional[str],
    video_path: Optional[str] = None,
    youtube_url: Optional[str] = None,
) -> None:
    """Run long film grading after the upload request has returned."""
    try:
        FILM_JOBS[job_id] = {"status": "processing", "message": "Sending film to Gemini."}
        if video_path:
            video_file = await asyncio.to_thread(ai_client.files.upload, file=video_path)
            FILM_JOBS[job_id] = {"status": "processing", "message": "Gemini is processing the film."}
            video_content = await _wait_for_gemini_file(video_file)
        else:
            video_content = types.Part(file_data=types.FileData(file_uri=youtube_url))
        FILM_JOBS[job_id] = {"status": "processing", "message": "Gemini is grading the player."}
        result = await _run_film_analysis(video_content, position, player_identifier)
        FILM_JOBS[job_id] = {"status": "complete", "result": result}
    except HTTPException as exc:
        FILM_JOBS[job_id] = {"status": "failed", "error": str(exc.detail)}
    except Exception as exc:
        logger.exception("film job %s failed", job_id)
        FILM_JOBS[job_id] = {"status": "failed", "error": str(exc)}
    finally:
        if video_path and os.path.exists(video_path):
            os.remove(video_path)


@app.post("/api/v1/scout/analyze-film/jobs", dependencies=[Depends(require_api_key)])
async def create_film_job(
    background_tasks: BackgroundTasks,
    file: Optional[UploadFile] = File(None),
    youtube_url: Optional[str] = Form(None),
    position: Position = Form(Position.DB),
    player_identifier: Optional[str] = Form(None),
):
    """Accept a long film and grade it asynchronously so HTTP limits cannot cancel it."""
    if bool(file) == bool(youtube_url):
        raise HTTPException(status_code=400, detail="Provide exactly one of: a video file upload, or a youtube_url.")
    if youtube_url and not _is_youtube_url(youtube_url):
        raise HTTPException(status_code=400, detail="Not a valid YouTube URL.")

    job_id = str(uuid4())
    video_path = None
    if file:
        if not file.filename or not file.filename.lower().endswith(ALLOWED_EXTS):
            raise HTTPException(status_code=400, detail="Invalid video format.")
        suffix = Path(file.filename).suffix.lower()
        video_path = os.path.join(settings.UPLOAD_TMP_DIR, f"{job_id}{suffix}")
        written = 0
        try:
            with open(video_path, "wb") as buffer:
                while chunk := await file.read(1024 * 1024):
                    written += len(chunk)
                    if written > settings.MAX_UPLOAD_MB * 1024 * 1024:
                        raise HTTPException(status_code=413, detail="File exceeds upload limit.")
                    buffer.write(chunk)
        except Exception:
            if os.path.exists(video_path):
                os.remove(video_path)
            raise

    FILM_JOBS[job_id] = {"status": "queued", "message": "Film received; grading is queued."}
    background_tasks.add_task(
        _process_film_job, job_id, position, player_identifier, video_path, youtube_url
    )
    return {"job_id": job_id, "status": "queued"}


@app.get("/api/v1/scout/analyze-film/jobs/{job_id}", dependencies=[Depends(require_api_key)])
async def get_film_job(job_id: str):
    job = FILM_JOBS.get(job_id)
    if job is None:
        raise HTTPException(status_code=404, detail="Film grading job not found.")
    return {"job_id": job_id, **job}


async def _run_film_analysis(
    video_content, position: Position, player_identifier: Optional[str] = None,
) -> dict:
    try:
        response = await asyncio.to_thread(
            ai_client.models.generate_content,
            model=settings.GEMINI_MODEL,
            contents=[video_content, build_scouting_prompt(position, player_identifier)],
            config=types.GenerateContentConfig(response_mime_type="application/json"),
        )

        try:
            analysis = json.loads(response.text)
        except (json.JSONDecodeError, TypeError):
            analysis = {"raw": response.text}

        as_dict = analysis if isinstance(analysis, dict) else {}
        return {
            "success": True,
            "position": position.value,
            "player_identifier": player_identifier,
            "analysis": analysis,
            "player_identified": as_dict.get("player_identified"),
            "identification_note": as_dict.get("identification_note"),
            "film_grades": as_dict.get("film_grades", {}),
            "flags": as_dict.get("flags", []),
        }

    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/v1/scout/truth-report", dependencies=[Depends(require_api_key)])
async def truth_report(
    position: Position = Form(Position.DB),
    athlete_json: str = Form(..., description="AthleteCreate payload as JSON string"),
    makeup_json: Optional[str] = Form(None, description="MakeupGrades payload as JSON string"),
    file: Optional[UploadFile] = File(None),
    youtube_url: Optional[str] = Form(None),
    player_identifier: Optional[str] = Form(None),
    athlete_id: Optional[UUID] = Form(
        None, description="Link this report to an existing athlete instead of creating a new one.",
    ),
):
    """Module 4: Truth Report Panel — combines the metric sieve (hard laser
    thresholds), the Profile & Makeup grade-down, and Gemini film analysis
    into one user-facing evaluation. Best-effort persists the athlete (if
    new) and the evaluation to PostgreSQL — a DB outage degrades the report
    to "not saved" rather than failing the request."""
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
    film = await analyze_player_film(
        file=file, youtube_url=youtube_url, position=position,
        player_identifier=player_identifier,
    )

    saved_athlete_id, saved_evaluation_id = await _persist_evaluation(
        athlete=athlete, athlete_id=athlete_id, position=position, sieve=sieve,
        makeup_json=makeup_json, makeup=makeup, film=film,
    )

    return {
        "success": True,
        "athlete": f"{athlete.first_name} {athlete.last_name}",
        "athlete_id": saved_athlete_id,
        "evaluation_id": saved_evaluation_id,
        "persisted": saved_evaluation_id is not None,
        "position": position.value,
        "projected_tier": sieve.tier.value,
        "qualifying_tiers": [t.value for t in sieve.qualifying_tiers],
        "hard_metrics_passed": sieve.hard_metrics_passed,
        "is_game_changer": sieve.is_game_changer,
        "game_changer_reason": sieve.game_changer_reason,
        "metric_sieve": sieve.model_dump(),
        "makeup_grade": makeup.model_dump() if makeup else None,
        "player_identified": film["player_identified"],
        "identification_note": film["identification_note"],
        "film_analysis": film["analysis"],
        "film_grades": film["film_grades"],
        "film_flags": film["flags"],
    }


async def _persist_evaluation(
    *, athlete: AthleteCreate, athlete_id: Optional[UUID], position: Position,
    sieve: SieveResult, makeup_json: Optional[str], makeup: Optional[GradeDown], film: dict,
) -> tuple[Optional[str], Optional[str]]:
    """Best-effort save; returns (athlete_id, evaluation_id) as strings, or
    (None, None) if the DB was unavailable. Never raises."""
    async with best_effort_session() as session:
        if session is None:
            return None, None
        try:
            if athlete_id is not None:
                athlete_row = await session.get(orm.Athlete, athlete_id)
                if athlete_row is None:
                    logger.warning("truth-report: athlete_id %s not found, creating new athlete instead", athlete_id)
                    athlete_row = orm.Athlete(**athlete.model_dump())
                    session.add(athlete_row)
            else:
                athlete_row = orm.Athlete(**athlete.model_dump())
                session.add(athlete_row)
            await session.flush()  # assigns athlete_row.id

            evaluation_row = orm.Evaluation(
                athlete_id=athlete_row.id,
                position_evaluated=position.value,
                projected_tier=sieve.tier.value,
                metric_sieve_results=sieve.model_dump(mode="json"),
                qualifying_tiers=[t.value for t in sieve.qualifying_tiers],
                is_game_changer=sieve.is_game_changer,
                game_changer_reason=sieve.game_changer_reason,
                makeup_grades=json.loads(makeup_json) if makeup_json else None,
                makeup_grade_down=makeup.model_dump(mode="json") if makeup else None,
                player_identifier=film.get("player_identifier"),
                player_identified=film.get("player_identified"),
                identification_note=film.get("identification_note"),
                film_grades=film["film_grades"],
                film_flags=film["flags"],
                film_analysis=film["analysis"] if isinstance(film["analysis"], dict) else {"raw": film["analysis"]},
                model_used=settings.GEMINI_MODEL,
            )
            session.add(evaluation_row)
            await session.commit()
            return str(athlete_row.id), str(evaluation_row.id)
        except Exception as e:
            logger.warning("truth-report: persistence failed, continuing without it: %s", e)
            await session.rollback()
            return None, None
