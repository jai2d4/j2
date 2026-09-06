"""Load and validate human-editable position rules."""
from __future__ import annotations

import json
from pathlib import Path

from pydantic import BaseModel, Field


RULES_DIR = Path(__file__).with_name("rules")
POSITION_ALIASES = {"DB": "CB", "DL": "DT", "OL": "IOL", "TE": "Y", "S": "SAFETY"}


class TraitRule(BaseModel):
    weight: float = Field(gt=0)
    enabled: bool = True


class PositionRules(BaseModel):
    position: str
    version: str
    traits: dict[str, TraitRule]


def normalize_position(position: str) -> str:
    value = position.strip().upper()
    return POSITION_ALIASES.get(value, value)


def load_position_rules(position: str) -> PositionRules:
    normalized = normalize_position(position)
    path = RULES_DIR / f"{normalized.lower()}.json"
    if not path.is_file():
        raise ValueError(f"Unsupported TruGrade position: {position}")
    rules = PositionRules.model_validate(json.loads(path.read_text(encoding="utf-8")))
    if rules.position != normalized:
        raise ValueError(f"Rule file position mismatch: expected {normalized}, got {rules.position}")
    return rules

