"""Pydantic models — request/response validation for the TRU Scouting Engine."""
from __future__ import annotations

from datetime import datetime
from enum import Enum
from typing import Optional
from uuid import UUID

from pydantic import BaseModel, Field


class Position(str, Enum):
    QB = "QB"
    RB = "RB"
    WR = "WR"
    DB = "DB"
    LB = "LB"
    DE = "DE"
    DL = "DL"
    OL = "OL"
    TE = "TE"


class Tier(str, Enum):
    D1_FBS = "D1_FBS"
    D1_FCS = "D1_FCS"
    D1_FCS_D2 = "D1_FCS_D2"
    D2_D3_NAIA_JUCO = "D2_D3_NAIA_JUCO"
    UNRANKED = "UNRANKED"


class Rank(str, Enum):
    """Qualitative trait/makeup grade, P4-standard — best to worst."""
    GAME_CHANGER = "GAME_CHANGER"
    ALL_CONF = "ALL_CONF"
    WIN_PLUS = "WIN_PLUS"
    WIN = "WIN"
    WIN_MINUS = "WIN_MINUS"
    NGE = "NGE"


# Best-to-worst order, used for averaging and for the grade-down shift.
RANK_ORDER: list[Rank] = [
    Rank.GAME_CHANGER, Rank.ALL_CONF, Rank.WIN_PLUS,
    Rank.WIN, Rank.WIN_MINUS, Rank.NGE,
]


class FilmStatus(str, Enum):
    PENDING = "pending"
    PROCESSING = "processing"
    ANALYZED = "analyzed"
    FAILED = "failed"


class AthleteCreate(BaseModel):
    first_name: str = Field(..., max_length=64)
    last_name: str = Field(..., max_length=64)
    grad_year: Optional[int] = None
    school: Optional[str] = None
    state: Optional[str] = Field(None, max_length=2)
    position: Position
    height_in: Optional[float] = Field(None, gt=50, lt=90)
    weight_lbs: Optional[int] = Field(None, gt=100, lt=450)
    forty_s: Optional[float] = Field(None, gt=3.8, lt=7.0, description="Laser-timed 40-yd dash")
    shuttle_s: Optional[float] = Field(None, gt=3.0, lt=6.0, description="Laser-timed pro-agility")
    bench_lbs: Optional[int] = None
    squat_lbs: Optional[int] = None
    gpa: Optional[float] = Field(None, ge=0, le=5.0)
    sat: Optional[int] = Field(None, ge=400, le=1600)
    act: Optional[int] = Field(None, ge=1, le=36)


class Athlete(AthleteCreate):
    id: UUID
    created_at: datetime
    updated_at: datetime

    class Config:
        from_attributes = True


class MetricCheck(BaseModel):
    """One row of the metric sieve — a single threshold comparison."""
    metric: str
    athlete_value: Optional[float] = None
    threshold: str
    passed: Optional[bool] = None  # None = data missing, not failed


class SieveResult(BaseModel):
    position: Position
    tier: Tier
    checks: list[MetricCheck]
    hard_metrics_passed: bool
    qualifying_tiers: list[Tier] = []
    is_game_changer: bool = False
    game_changer_reason: Optional[str] = None


class MakeupGrades(BaseModel):
    """Profile & Makeup rubric — graded against the P4 standard."""
    size: Optional[Rank] = None
    athletic_ability: Optional[Rank] = None
    play_history: Optional[Rank] = None
    play_style: Optional[Rank] = None
    character: Optional[Rank] = None


class GradeDown(BaseModel):
    """The overall Makeup grade, shifted down per classification level."""
    overall: Rank
    p4: Rank
    group_of_5: Rank
    fcs: Rank
    d2_d3_naia_juco: Rank


class EvaluationOut(BaseModel):
    id: UUID
    athlete_id: UUID
    film_id: Optional[UUID] = None
    position_evaluated: Position
    projected_tier: Tier
    physical_projection: Optional[dict] = None
    explosive_traits: Optional[dict] = None
    mechanics_grade: Optional[float] = Field(None, ge=0, le=10)
    situational_attributes: Optional[dict] = None
    metric_sieve_results: Optional[dict] = None
    is_game_changer: bool = False
    model_used: str = "gemini-3.5-flash"
    created_at: datetime
