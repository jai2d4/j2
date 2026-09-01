"""Shared API-key gate for /api/v1/scout/* and the athlete/evaluation routes.

Local dev stays frictionless: leave API_KEY unset in .env and every request
passes through unchecked. Set API_KEY before exposing the app beyond your
own machine — every protected route then requires a matching X-API-Key
header, and /api/v1/health stays open for uptime monitors either way.
"""
from __future__ import annotations

import secrets

from fastapi import Header, HTTPException

from app.core.config import get_settings


async def require_api_key(x_api_key: str | None = Header(None)) -> None:
    settings = get_settings()
    if settings.API_KEY is None:
        return  # auth disabled — local-dev default
    # compare_digest rather than ==: a plain comparison returns as soon as two
    # bytes differ, and that timing difference is measurable often enough to
    # recover a shared secret one character at a time. The `or ""` keeps a
    # missing header on the same path as a wrong one — compare_digest raises on
    # None, and an early return for the missing case would reintroduce exactly
    # the timing signal being removed.
    if not secrets.compare_digest(x_api_key or "", settings.API_KEY):
        raise HTTPException(status_code=401, detail="Missing or invalid X-API-Key header.")
