"""Phase 6 play, observation override, and evidence endpoints."""
from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from backend.api.videos import storage_root, video_store
from backend.football.evidence import EvidenceStore
from backend.football.observations import ObservationOverride
from backend.football.play_segmenter import Play, correct_play


router = APIRouter(prefix="/api", tags=["football"])


class PlayCorrection(BaseModel):
    start_time: float = Field(ge=0)
    snap_time: float = Field(ge=0)
    end_time: float = Field(ge=0)
    excluded: bool = False
    reason: str = Field(min_length=2)
    scout_note: str | None = None


def _json(path: Path, default):
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else default


@router.get("/videos/{video_id}/plays")
async def get_plays(video_id: str):
    if not video_store.get(video_id):
        raise HTTPException(404, "Video not found.")
    path = storage_root / "football" / video_id / "plays.json"
    return {"video_id": video_id, "plays": _json(path, [])}


@router.post("/videos/{video_id}/plays/{play_id}/correct")
async def correct_play_boundary(video_id: str, play_id: str, request: PlayCorrection):
    path = storage_root / "football" / video_id / "plays.json"
    plays = _json(path, [])
    index = next((i for i, value in enumerate(plays) if value["play_id"] == play_id), None)
    if index is None:
        raise HTTPException(404, "Play not found.")
    corrected, audit = correct_play(Play.model_validate(plays[index]), **request.model_dump(exclude={"scout_note"}))
    plays[index] = {**corrected.model_dump(), "scout_note": request.scout_note,
                    "history": plays[index].get("history", []) + [{**audit, "timestamp": datetime.now(timezone.utc).isoformat()}]}
    path.write_text(json.dumps(plays, indent=2), encoding="utf-8")
    return plays[index]


@router.post("/observations/{observation_id}/override")
async def override_observation(observation_id: str, request: ObservationOverride):
    directory = storage_root / "overrides"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{observation_id}.json"
    prior = _json(path, {"observation_id": observation_id, "current_value": "unknown", "history": []})
    audit = {"original_value": prior.get("current_value"), "new_value": request.new_value,
             "timestamp": datetime.now(timezone.utc).isoformat(), "reason": request.reason, "source": "human"}
    prior.update(current_value=request.new_value, source="human", history=prior.get("history", []) + [audit])
    path.write_text(json.dumps(prior, indent=2), encoding="utf-8")
    return prior


@router.get("/players/{player_id}/evidence")
async def player_evidence(player_id: str):
    return {"player_id": player_id, "evidence": EvidenceStore(storage_root / "evidence").for_player(player_id)}

