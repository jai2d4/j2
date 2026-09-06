"""Strict football reasoning that cannot produce an official TruGrade score."""
from __future__ import annotations

import json

from pydantic import BaseModel, ConfigDict, Field

from backend.football.observations import FootballObservation
from .provider import AIProvider


SYSTEM_RULE = """You are an observation and football reasoning layer.
You do not determine the player's official TruGrade score.
Only identify evidence-supported football traits and outcomes.
Return unknown when evidence is insufficient. Never guess.
Every observation must cite a timestamp and explain its evidence."""


class ReasonedTrait(BaseModel):
    model_config = ConfigDict(extra="forbid")

    trait: str
    value: str
    confidence: float = Field(ge=0, le=1)
    evidence_timestamp: float | None = Field(default=None, ge=0)
    reason: str


class FootballReasoningResult(BaseModel):
    model_config = ConfigDict(extra="forbid")

    observations: list[ReasonedTrait]
    provider_note: str | None = None


class FootballReasoner:
    def __init__(self, provider: AIProvider):
        self.provider = provider

    async def reason(self, observation: FootballObservation) -> FootballReasoningResult:
        prompt = SYSTEM_RULE + "\nStructured source observation:\n" + json.dumps(observation.model_dump(mode="json"))
        raw = await self.provider.generate_json(prompt, FootballReasoningResult.model_json_schema())
        return FootballReasoningResult.model_validate(raw)

