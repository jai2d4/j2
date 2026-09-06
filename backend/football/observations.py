"""Strict evidence-aware football observation contracts."""
from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, Field

from backend.grading.models import Evidence


class ObservationDatum(BaseModel):
    value: Any | Literal["unknown"]
    confidence: float = Field(ge=0, le=1)
    evidence_timestamp: float | None = Field(default=None, ge=0)
    reason: str


class FootballObservation(BaseModel):
    observation_id: str
    play_id: str
    player_id: str
    position: str
    pre_snap: dict[str, ObservationDatum] = Field(default_factory=dict)
    movement: dict[str, ObservationDatum] = Field(default_factory=dict)
    result: dict[str, ObservationDatum] = Field(default_factory=dict)
    evidence: list[Evidence] = Field(default_factory=list)
    excluded: bool = False


class ObservationOverride(BaseModel):
    new_value: Any
    reason: str = Field(min_length=2)
    source: Literal["human"] = "human"

