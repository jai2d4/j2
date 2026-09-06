"""Validated data contracts for deterministic film grading."""
from __future__ import annotations

from enum import Enum
from typing import Literal

from pydantic import BaseModel, Field


class ObservationValue(str, Enum):
    CORRECT_ASSIGNMENT = "correct_assignment"
    WRONG_ASSIGNMENT = "wrong_assignment"
    ELITE_EXECUTION = "elite_execution"
    POSITIVE_EXECUTION = "positive_execution"
    NEUTRAL_EXECUTION = "neutral_execution"
    MINOR_ERROR = "minor_error"
    MAJOR_ERROR = "major_error"
    GAME_CHANGING_POSITIVE = "game_changing_positive"
    GAME_CHANGING_NEGATIVE = "game_changing_negative"
    UNKNOWN = "unknown"


class Evidence(BaseModel):
    video_id: str
    play_id: str
    timestamp_start: float = Field(ge=0)
    timestamp_end: float = Field(ge=0)
    frame_ids: list[int] = Field(default_factory=list)
    track_id: int | None = None
    player_id: str | None = None
    description: str


class GradingEvent(BaseModel):
    rule_id: str
    trait: str
    value: float
    confidence: float = Field(ge=0, le=1)
    timestamp: float = Field(ge=0)
    play_id: str
    reason: str
    observation: ObservationValue
    evidence: Evidence | None = None


class TraitScore(BaseModel):
    trait: str
    score: float = Field(ge=0, le=100)
    confidence: float = Field(ge=0, le=1)
    sample_count: int = Field(ge=0)
    evidence: list[Evidence] = Field(default_factory=list)


class PositionGrade(BaseModel):
    position: str
    grade: float | None = Field(default=None, ge=0, le=100)
    confidence: float = Field(ge=0, le=1)
    traits: dict[str, TraitScore | None]
    unknown_traits: list[str] = Field(default_factory=list)


class ProspectProfile(str, Enum):
    ELITE = "ELITE"
    ALL_CONF = "ALL-CONF"
    WIN_PLUS = "WIN+"
    WIN = "WIN"
    WIN_MINUS = "WIN-"


class ProfileCategory(BaseModel):
    category: Literal["SIZE", "ATHLETIC ABILITY", "PLAY HISTORY", "PLAY STYLE", "CHARACTER"]
    profile: ProspectProfile | None = None
    available: bool = False
    source: str | None = None

