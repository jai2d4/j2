"""Durable-on-disk Phase 3 background analysis jobs."""
from __future__ import annotations

import asyncio
import json
import os
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4

from fastapi import APIRouter, BackgroundTasks, HTTPException

from backend.api.videos import storage_root, video_store
from backend.video.frame_extractor import FrameExtractor
from backend.vision.pipeline import analyze_frames
from backend.football.play_segmenter import PlaySegmenter


router = APIRouter(prefix="/api/analysis", tags=["analysis"])
jobs_dir = storage_root / "jobs"
jobs_dir.mkdir(parents=True, exist_ok=True)


def _write_job(job: dict) -> None:
    (jobs_dir / f"{job['job_id']}.json").write_text(json.dumps(job, indent=2), encoding="utf-8")


def _read_job(job_id: str) -> dict | None:
    path = jobs_dir / f"{job_id}.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else None


async def _extract_job(job_id: str, video: dict) -> None:
    job = _read_job(job_id)
    if not job:
        return
    try:
        job.update(status="extracting_frames", progress=1, message="Extracting timestamped tracking frames.")
        _write_job(job)
        extractor = FrameExtractor(storage_root / "frames", float(os.getenv("ANALYSIS_FPS", "10")))

        def update(value: int) -> None:
            job["progress"] = value
            _write_job(job)

        frames = await asyncio.to_thread(extractor.extract, video["video_id"], Path(video["file_path"]), update)
        job.update(status="detecting", progress=0, frame_count=len(frames), message="Detecting players and football objects.")
        _write_job(job)

        def vision_progress(value: int) -> None:
            job["progress"] = value
            _write_job(job)

        detections, tracks = await asyncio.to_thread(
            analyze_frames, video["video_id"], frames, storage_root / "vision", vision_progress
        )
        job.update(status="tracking", progress=99, message="Finalizing persistent player tracks.")
        _write_job(job)
        job.update(status="segmenting_plays", progress=99, message="Estimating play boundaries.")
        _write_job(job)
        plays = PlaySegmenter().segment(video["video_id"], tracks)
        football_dir = storage_root / "football" / video["video_id"]
        football_dir.mkdir(parents=True, exist_ok=True)
        (football_dir / "plays.json").write_text(
            json.dumps([play.model_dump() for play in plays], indent=2), encoding="utf-8"
        )
        job.update(status="completed", progress=100, detection_frames=len(detections),
                   track_count=len(tracks), message="Detection and tracking completed.")
    except Exception as exc:
        job.update(status="failed", message="Frame extraction failed.", error=str(exc))
    job["updated_at"] = datetime.now(timezone.utc).isoformat()
    _write_job(job)


@router.post("/start/{video_id}", status_code=202)
async def start_analysis(video_id: str, background_tasks: BackgroundTasks):
    video = video_store.get(video_id)
    if not video:
        raise HTTPException(404, "Video not found.")
    job_id = str(uuid4())
    now = datetime.now(timezone.utc).isoformat()
    job = {"job_id": job_id, "video_id": video_id, "status": "uploaded", "progress": 0,
           "message": "Analysis queued.", "created_at": now, "updated_at": now}
    _write_job(job)
    background_tasks.add_task(_extract_job, job_id, video)
    return job


@router.get("/status/{job_id}")
async def analysis_status(job_id: str):
    job = _read_job(job_id)
    if not job:
        raise HTTPException(404, "Analysis job not found.")
    return job
