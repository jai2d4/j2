"""Jersey identity candidates and human-confirmed mappings."""
from __future__ import annotations

from datetime import datetime, timezone

from pydantic import BaseModel, Field


class JerseyCandidate(BaseModel):
    jersey_number: str = Field(pattern=r"^[0-9]{1,2}$")
    confidence: float = Field(ge=0, le=1)
    source: str = "ocr"


class TrackIdentity(BaseModel):
    track_id: int = Field(ge=0)
    player_id: str | None = None
    jersey_number: str | None = Field(default=None, pattern=r"^[0-9]{1,2}$")
    team: str | None = None
    position: str | None = None
    candidates: list[JerseyCandidate] = Field(default_factory=list)
    confidence: float = Field(ge=0, le=1)
    confirmed: bool = False


def confirm_identity(original: TrackIdentity, *, player_id: str | None, jersey_number: str,
                     team: str | None, position: str | None, reason: str) -> tuple[TrackIdentity, dict]:
    confirmed = original.model_copy(update={"player_id": player_id, "jersey_number": jersey_number,
                                             "team": team, "position": position,
                                             "confidence": 1.0, "confirmed": True})
    audit = {"original_value": original.model_dump(), "new_value": confirmed.model_dump(),
             "timestamp": datetime.now(timezone.utc).isoformat(), "reason": reason, "source": "human"}
    return confirmed, audit

