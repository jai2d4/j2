"""
Module 7 — How to Improve.

Turns the metric sieve's failed checks and the film analysis's NGE-flagged
factors into a concrete "what to work on" plan: drills, and target numbers
per NCAA classification for the athlete's position. Gemini generates the
targets from its own knowledge (not pinned to metric_sieve.py's matrix),
so treat them as coaching guidance rather than the sieve's hard thresholds.
"""
from __future__ import annotations

import json

from google.genai import types

from app.models.schemas import Position, SieveResult


def build_improvement_prompt(
    position: Position, sieve: SieveResult, film_grades: dict, flags: list
) -> str:
    failed = [c for c in sieve.checks if c.passed is False]
    failed_bit = (
        "\n".join(f"    - {c.metric}: {c.athlete_value} (needs {c.threshold})" for c in failed)
        or "    - none — all provided hard metrics already clear the sieve"
    )
    nge_factors = [f for f, rank in film_grades.items() if rank == "NGE"]
    nge_bit = "\n".join(f"    - {f}" for f in nge_factors) or "    - none flagged on this film"
    flags_bit = "\n".join(f"    - {f}" for f in flags) or "    - none"

    return f"""
    You are a college football strength/skills coach advising a {position.value} prospect
    on exactly what to work on next.

    Failed metric-sieve checks (hard combine numbers that missed the athlete's current
    projected tier's threshold):
{failed_bit}

    Film factors graded NGE (a real, observable deficiency):
{nge_bit}

    Other film flags for context:
{flags_bit}

    Based on this, write a concrete improvement plan:
    1. weak_areas: array of short strings naming the specific things to fix, prioritized.
    2. drills: array of objects {{"focus": <weak area>, "drill": <specific named drill/exercise
       with sets/reps or duration where sensible>, "why": <one sentence on what it fixes>}} —
       3 to 6 drills, concrete and actionable, not generic ("work out more").
    3. division_targets: an object with keys "D1_FBS", "Group_of_5_or_FCS", "D2_D3_NAIA_JUCO",
       each a short string describing the realistic combine/film bar to clear that level for a
       {position.value}, grounded in typical published recruiting thresholds for this position.
    4. summary: 2-3 sentences, direct and specific, telling the athlete what to prioritize first
       and why it moves the needle most.

    Return ONLY a JSON object with exactly these four keys. No prose outside the JSON object.
    """


def parse_improvement_response(text: str) -> dict:
    try:
        data = json.loads(text)
    except (json.JSONDecodeError, TypeError):
        return {"raw": text}
    return data if isinstance(data, dict) else {"raw": text}


def generate_improvement_plan(
    ai_client, model: str, position: Position, sieve: SieveResult, film_grades: dict, flags: list
) -> dict:
    prompt = build_improvement_prompt(position, sieve, film_grades, flags)
    response = ai_client.models.generate_content(
        model=model,
        contents=[prompt],
        config=types.GenerateContentConfig(response_mime_type="application/json"),
    )
    return parse_improvement_response(response.text)
