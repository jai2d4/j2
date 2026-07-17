"""
Module 8 batch job — runs the web-search-for-updates check across every
saved athlete. Meant to run on a schedule (see README), separate from the
API server, so it works whether or not uvicorn happens to be running.

Usage:
    python -m scripts.search_athlete_updates
"""
from __future__ import annotations

import asyncio

from google import genai

from app.core.config import get_settings
from app.core.db import create_pool
from app.services import repository
from app.services.web_search import find_athlete_updates, parse_published_date


async def run() -> None:
    settings = get_settings()
    ai_client = genai.Client(api_key=settings.GEMINI_API_KEY)
    pool = await create_pool()
    try:
        athletes = await repository.list_athletes(pool, limit=1000)
        print(f"Checking {len(athletes)} athlete(s) for updates...")

        total_found = 0
        for athlete in athletes:
            name = f"{athlete['first_name']} {athlete['last_name']}"
            try:
                result = find_athlete_updates(
                    ai_client, settings.GEMINI_MODEL,
                    first_name=athlete["first_name"], last_name=athlete["last_name"],
                    position=athlete["position"], school=athlete["school"],
                    grad_year=athlete["grad_year"],
                )
            except Exception as e:
                print(f"  {name}: search failed — {e}")
                continue

            saved = 0
            for update in result["updates"]:
                if not isinstance(update, dict):
                    continue
                await repository.create_web_update(
                    pool,
                    athlete_id=athlete["id"],
                    source_title=update.get("title"),
                    source_url=update.get("url"),
                    published_at=parse_published_date(update.get("published_date")),
                    summary=update.get("summary") or "No summary provided.",
                    discovered_metrics=update.get("metrics") if isinstance(update.get("metrics"), dict) else None,
                )
                saved += 1
            total_found += saved
            print(f"  {name}: {saved} update(s) saved")

        print(f"Done - {total_found} update(s) saved across {len(athletes)} athlete(s).")
    finally:
        await pool.close()


if __name__ == "__main__":
    asyncio.run(run())
