"""
Module 8 — Web-discovered athlete updates.

Uses Gemini's Google Search grounding tool to look for recent articles,
recruiting-site listings, or updated combine numbers for a saved athlete,
and asks it to surface any new metric numbers it can find in the sources.
Feeds the progression chart alongside evaluation history.
"""
from __future__ import annotations

import json
from datetime import datetime, timezone
from typing import Optional

from google.genai import types


def parse_published_date(raw: Optional[str]) -> Optional[datetime]:
    if not raw:
        return None
    try:
        parsed = datetime.fromisoformat(raw)
    except (ValueError, TypeError):
        return None
    return parsed if parsed.tzinfo else parsed.replace(tzinfo=timezone.utc)


def build_search_prompt(
    first_name: str, last_name: str, position: str, school: Optional[str], grad_year: Optional[int]
) -> str:
    school_bit = f" at {school}" if school else ""
    year_bit = f", class of {grad_year}" if grad_year else ""
    return f"""
    Search the web for recent news, recruiting profiles, or combine results about
    the football prospect {first_name} {last_name}{school_bit}{year_bit}, who plays {position}.

    Only include sources that are clearly about this specific athlete — if you can't
    find anything specific and reliable, say so rather than guessing or including a
    same-named athlete who plays a different sport or position.

    For each relevant source you find, extract:
    - title: the article/page title
    - url: the source URL
    - published_date: ISO date if available, otherwise null
    - summary: one or two sentences on what's new or notable
    - metrics: an object with any of these keys you find an explicit number for —
      height_in, weight_lbs, forty_s, shuttle_s, bench_lbs, squat_lbs, gpa, sat, act
      (omit any key you don't have a real number for)

    Return ONLY a JSON object with exactly this shape:
    {{"found": true or false, "updates": [{{"title": "...", "url": "...", "published_date": "...", "summary": "...", "metrics": {{}}}}]}}
    No prose outside the JSON object.
    """


def search_tool() -> types.Tool:
    return types.Tool(google_search=types.GoogleSearch())


def parse_search_response(text: str) -> dict:
    try:
        data = json.loads(text)
    except (json.JSONDecodeError, TypeError):
        return {"found": False, "updates": []}
    if not isinstance(data, dict):
        return {"found": False, "updates": []}
    updates = data.get("updates", [])
    return {"found": bool(data.get("found")), "updates": updates if isinstance(updates, list) else []}


def find_athlete_updates(
    ai_client,
    model: str,
    *,
    first_name: str,
    last_name: str,
    position: str,
    school: Optional[str],
    grad_year: Optional[int],
) -> dict:
    prompt = build_search_prompt(first_name, last_name, position, school, grad_year)
    response = ai_client.models.generate_content(
        model=model,
        contents=[prompt],
        config=types.GenerateContentConfig(tools=[search_tool()]),
    )
    return parse_search_response(response.text)
