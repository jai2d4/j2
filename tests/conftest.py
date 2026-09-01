"""Shared pytest fixtures. Sets required env vars before app.main is ever
imported — Settings() reads the environment at import time."""
import os

os.environ.setdefault("GEMINI_API_KEY", "test-key-not-real")
os.environ.setdefault("POSTGRES_PASSWORD", "test")
os.environ.setdefault("POSTGRES_DB", os.environ.get("POSTGRES_DB", "tru_scouting_test"))

import json  # noqa: E402
from unittest.mock import MagicMock  # noqa: E402

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402


@pytest.fixture
def client():
    """Each test gets its own TestClient, which runs its own event loop —
    but the SQLAlchemy async engine's asyncpg pool is a module-level
    singleton whose connections are bound to whichever loop opened them.
    Dispose the pool after every test so the next one starts clean on its
    own loop instead of reusing (and crashing on) a stale connection."""
    import app.main as m
    from app.core.db import _engine
    with TestClient(m.app) as c:
        yield c
    import asyncio
    loop = asyncio.new_event_loop()
    try:
        loop.run_until_complete(_engine.dispose())
    finally:
        loop.close()


@pytest.fixture
def mock_gemini():
    """Patches the Gemini client to return a canned, valid film-grading response."""
    import app.main as m

    def _set(film_grades=None, flags=None, player_identified=True, identification_note="ok"):
        payload = {
            "player_identified": player_identified,
            "identification_note": identification_note,
            "film_grades": {"Toughness": "WIN"} if film_grades is None else film_grades,
            "flags": [] if flags is None else flags,
        }
        fake_response = MagicMock()
        fake_response.text = json.dumps(payload)
        m.ai_client.models.generate_content = MagicMock(return_value=fake_response)
        m.ai_client.files.upload = MagicMock(return_value=MagicMock(name="files/fake"))
        return payload

    _set()  # sensible default so tests that don't customize still get a valid response
    return _set


@pytest.fixture
def api_key_required(monkeypatch):
    """Turns on auth for the duration of a test and returns the key to send."""
    from app.core.config import get_settings
    settings = get_settings()
    monkeypatch.setattr(settings, "API_KEY", "test-shared-key")
    return "test-shared-key"


@pytest.fixture(scope="session")
def db_available():
    """Skips DB-dependent tests gracefully when no PostgreSQL is reachable —
    CI provides one via a service container; local runs without one still pass.
    Returns (True, None) or (False, "<real error>") so skips show *why*.

    Deliberately pings with a standalone asyncpg connection, NOT the shared
    SQLAlchemy engine from app.core.db — asyncpg connections are bound to the
    event loop that created them, and running this check against the shared
    engine's pool on a throwaway loop poisons it for every later test that
    uses the engine on TestClient's own loop."""
    import asyncio
    import asyncpg
    from app.core.config import get_settings

    settings = get_settings()

    async def _ping():
        conn = await asyncpg.connect(
            host=settings.POSTGRES_HOST, port=settings.POSTGRES_PORT,
            user=settings.POSTGRES_USER, password=settings.POSTGRES_PASSWORD,
            database=settings.POSTGRES_DB,
        )
        await conn.close()

    try:
        loop = asyncio.new_event_loop()
        try:
            loop.run_until_complete(_ping())
        finally:
            loop.close()
        return True, None
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"
