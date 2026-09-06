"""Phase 5 track identity and field-calibration endpoints."""
from __future__ import annotations

import json
from pathlib import Path

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from backend.api.videos import storage_root, video_store
from backend.vision.field_calibration import FieldCalibration, automatic_calibration_unavailable
from backend.vision.jersey_identifier import TrackIdentity, confirm_identity


router = APIRouter(prefix="/api/videos", tags=["players"])


class AssignmentRequest(BaseModel):
    player_id: str | None = None
    jersey_number: str = Field(pattern=r"^[0-9]{1,2}$")
    team: str | None = None
    position: str | None = None
    reason: str = Field(min_length=2)


def _require_video(video_id: str) -> dict:
    video = video_store.get(video_id)
    if not video:
        raise HTTPException(404, "Video not found.")
    return video


def _track_path(video_id: str) -> Path:
    return storage_root / "vision" / video_id / "tracks.json"


@router.get("/{video_id}/tracks")
async def get_tracks(video_id: str):
    _require_video(video_id)
    path = _track_path(video_id)
    tracks = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else []
    assignment_path = storage_root / "assignments" / f"{video_id}.json"
    assignments = json.loads(assignment_path.read_text(encoding="utf-8")) if assignment_path.is_file() else {}
    return {"video_id": video_id, "tracks": tracks, "assignments": assignments}


@router.post("/{video_id}/tracks/{track_id}/assign")
async def assign_track(video_id: str, track_id: int, request: AssignmentRequest):
    _require_video(video_id)
    tracks_path = _track_path(video_id)
    tracks = json.loads(tracks_path.read_text(encoding="utf-8")) if tracks_path.is_file() else []
    if not any(item.get("track_id") == track_id for item in tracks):
        raise HTTPException(404, "Track not found.")
    directory = storage_root / "assignments"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{video_id}.json"
    data = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    existing = data.get(str(track_id), {})
    original = TrackIdentity(track_id=track_id, player_id=existing.get("player_id"),
                             jersey_number=existing.get("jersey_number"), team=existing.get("team"),
                             position=existing.get("position"), candidates=existing.get("candidates", []),
                             confidence=existing.get("confidence", 0), confirmed=existing.get("confirmed", False))
    identity, audit = confirm_identity(original, **request.model_dump())
    history = existing.get("history", []) + [audit]
    data[str(track_id)] = {**identity.model_dump(), "history": history}
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    return data[str(track_id)]


@router.get("/{video_id}/calibration")
async def get_calibration(video_id: str):
    _require_video(video_id)
    path = storage_root / "calibrations" / f"{video_id}.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else automatic_calibration_unavailable()


@router.post("/{video_id}/calibration")
async def save_calibration(video_id: str, calibration: FieldCalibration):
    _require_video(video_id)
    directory = storage_root / "calibrations"
    directory.mkdir(parents=True, exist_ok=True)
    payload = calibration.model_dump()
    (directory / f"{video_id}.json").write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return payload

