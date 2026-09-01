"""Regression tests for eight defects found by auditing the running app.

Each test here failed before its fix. They are grouped by the defect rather
than by endpoint, because what is being protected is the property, not the
route — the same pagination mistake existed in two routers and would happily
come back in a third.

Two of the eight are not meaningfully testable and say so where they sit:
the constant-time key comparison (a timing property, not a behavioural one)
and the non-blocking Gemini calls (asserted structurally rather than by
racing the event loop, which would be flaky in CI).
"""
import asyncio
import io
import os

import pytest


# ------------------------------------------------------------------ 1. bounds

@pytest.mark.parametrize("path", ["/api/v1/athletes", "/api/v1/evaluations"])
@pytest.mark.parametrize("qs", ["?limit=-1", "?offset=-1", "?limit=-5&offset=-5"])
def test_negative_pagination_is_rejected_not_a_server_error(client, path, qs):
    """Postgres refuses a negative LIMIT/OFFSET, so an unbounded parameter
    reached the driver and came back as an unhandled DBAPIError — a 500
    carrying an asyncpg exception. It must be a 422 naming the parameter."""
    r = client.get(path + qs)
    assert r.status_code == 422, f"{path}{qs} returned {r.status_code}"


@pytest.mark.parametrize("path", ["/api/v1/athletes", "/api/v1/evaluations"])
def test_oversized_limit_is_rejected_rather_than_silently_clamped(client, path):
    """The old code did min(limit, 200), which quietly returned something other
    than what was asked for. Saying no is more honest than pretending."""
    assert client.get(path + "?limit=100000").status_code == 422


@pytest.mark.parametrize("path", ["/api/v1/athletes", "/api/v1/evaluations"])
def test_ordinary_pagination_still_works(client, path):
    """The bound must not have cost the normal case. limit=0 stays legal: it is
    valid SQL, returns an empty page, and some clients send it."""
    for qs in ["", "?limit=10", "?limit=0", "?limit=200", "?offset=5"]:
        assert client.get(path + qs).status_code == 200, qs


# ---------------------------------------------------------------- 2. uploads

def test_oversized_upload_is_refused_without_being_read_whole(client, mock_gemini, monkeypatch):
    """The size check used to run after `await file.read()` with no argument,
    which materialised the entire body first: the 413 was correct and the
    memory was already spent. The fix streams in bounded chunks, so what this
    asserts is that every read is bounded."""
    from app.core.config import get_settings
    monkeypatch.setattr(get_settings(), "MAX_UPLOAD_MB", 1)

    from starlette.datastructures import UploadFile as StarletteUploadFile
    original = StarletteUploadFile.read
    sizes = []

    async def recording_read(self, size=-1):
        sizes.append(size)
        return await original(self, size)

    monkeypatch.setattr(StarletteUploadFile, "read", recording_read)

    payload = b"\0" * (4 * 1024 * 1024)  # 4 MB against a 1 MB limit
    r = client.post(
        "/api/v1/scout/analyze-film",
        files={"file": ("big.mp4", io.BytesIO(payload), "video/mp4")},
        data={"position": "DB"},
    )
    assert r.status_code == 413

    assert sizes, "the handler never read the upload"
    assert all(s > 0 for s in sizes), (
        f"an unbounded read slipped through: {sizes} — read(-1) returns the whole body"
    )


def test_a_rejected_upload_leaves_no_file_behind(client, mock_gemini, monkeypatch):
    from app.core.config import get_settings
    settings = get_settings()
    monkeypatch.setattr(settings, "MAX_UPLOAD_MB", 1)
    before = set(os.listdir(settings.UPLOAD_TMP_DIR))

    client.post(
        "/api/v1/scout/analyze-film",
        files={"file": ("big.mp4", io.BytesIO(b"\0" * (3 * 1024 * 1024)), "video/mp4")},
        data={"position": "DB"},
    )
    assert set(os.listdir(settings.UPLOAD_TMP_DIR)) == before


def test_upload_does_not_write_under_the_clients_filename(client, mock_gemini, monkeypatch):
    """Two concurrent uploads called film.mp4 previously shared one path: they
    overwrote each other, and the first to finish deleted the file the second
    was still sending. The stored name must be ours, and the extension kept."""
    from app.core.config import get_settings
    settings = get_settings()
    seen = []

    real_open = open

    def watching_open(path, *a, **kw):
        if str(path).startswith(settings.UPLOAD_TMP_DIR):
            seen.append(os.path.basename(str(path)))
        return real_open(path, *a, **kw)

    monkeypatch.setattr("builtins.open", watching_open)
    client.post(
        "/api/v1/scout/analyze-film",
        files={"file": ("film.mp4", io.BytesIO(b"\0" * 2048), "video/mp4")},
        data={"position": "DB"},
    )

    assert seen, "nothing was written to the upload directory"
    assert "film.mp4" not in seen, f"the client's filename was used verbatim: {seen}"
    assert all(n.endswith(".mp4") for n in seen), f"the extension was lost: {seen}"


def test_two_uploads_of_the_same_name_get_different_paths(client, mock_gemini, monkeypatch):
    from app.core.config import get_settings
    settings = get_settings()
    seen = []
    real_open = open

    def watching_open(path, *a, **kw):
        if str(path).startswith(settings.UPLOAD_TMP_DIR):
            seen.append(str(path))
        return real_open(path, *a, **kw)

    monkeypatch.setattr("builtins.open", watching_open)
    for _ in range(2):
        client.post(
            "/api/v1/scout/analyze-film",
            files={"file": ("film.mp4", io.BytesIO(b"\0" * 1024), "video/mp4")},
            data={"position": "DB"},
        )
    assert len(seen) == 2 and seen[0] != seen[1], f"both uploads used one path: {seen}"


# ------------------------------------------------------- 3. best-effort session

def test_best_effort_session_propagates_the_callers_own_exception(monkeypatch):
    """The broad `except Exception` used to sit outside the yield, so an error
    raised in the caller's block was thrown back in, caught, logged as "DB
    unavailable" — which it was not — and followed by a second yield. The
    caller saw RuntimeError: generator didn't stop after athrow().

    Deliberately against a stub rather than the real engine: this is a test
    about control flow, and reaching the shared SQLAlchemy pool from a
    throwaway asyncio.run loop binds a pooled asyncpg connection to a loop
    that then closes, breaking every later test that uses the engine. The
    conftest db_available fixture avoids the same trap for the same reason."""
    import app.core.db as db

    class LiveSession:
        async def connection(self):
            return None

        async def close(self):
            pass

    monkeypatch.setattr(db, "_SessionLocal", lambda: LiveSession())

    async def run():
        async with db.best_effort_session() as session:
            assert session is not None, "a reachable database should yield a session"
            raise ValueError("a bug in the caller's own code")

    with pytest.raises(ValueError, match="a bug in the caller's own code"):
        asyncio.run(run())


def test_best_effort_session_yields_none_when_the_database_is_unreachable(monkeypatch):
    """The behaviour it exists for has to survive the fix."""
    import app.core.db as db

    class DeadSession:
        async def connection(self):
            raise ConnectionRefusedError("nothing listening")

        async def close(self):
            pass

    monkeypatch.setattr(db, "_SessionLocal", lambda: DeadSession())

    async def run():
        async with db.best_effort_session() as session:
            return session

    assert asyncio.run(run()) is None


# ----------------------------------------------------------------- 4. auth

def test_a_missing_key_and_a_wrong_key_are_both_refused(client, api_key_required):
    """compare_digest raises on None, so the missing-header case needed handling
    without an early return — an early return is exactly the timing signal the
    change removes. The timing property itself is not asserted here; that would
    need a statistical harness this suite does not have."""
    assert client.get("/api/v1/athletes").status_code == 401
    assert client.get("/api/v1/athletes", headers={"X-API-Key": "wrong"}).status_code == 401
    assert client.get("/api/v1/athletes", headers={"X-API-Key": ""}).status_code == 401
    assert client.get("/api/v1/athletes", headers={"X-API-Key": api_key_required}).status_code == 200


def test_auth_uses_a_constant_time_comparison():
    """Structural, because the property is a timing one. If someone replaces
    compare_digest with == this fails and says why."""
    import inspect

    from app.core import auth

    source = inspect.getsource(auth.require_api_key)
    assert "compare_digest" in source, "the API key is compared with a plain =="


# ------------------------------------------------------ 5. blocking SDK calls

def test_gemini_calls_are_dispatched_off_the_event_loop():
    """ai_client.files.upload and models.generate_content are synchronous SDK
    calls. Awaited directly they block the loop for the length of an upload
    plus an inference, stalling every other request. Structural rather than a
    race: timing a concurrent request would be flaky in CI."""
    import inspect

    import app.main as m

    upload = inspect.getsource(m.analyze_player_film)
    assert "asyncio.to_thread(ai_client.files.upload" in upload, (
        "the Gemini file upload runs on the event loop"
    )
    analysis = inspect.getsource(m._run_film_analysis)
    assert "asyncio.to_thread(" in analysis and "generate_content" in analysis, (
        "generate_content runs on the event loop"
    )


# ------------------------------------------------------------ 6. error detail

def test_a_failing_analysis_does_not_return_internal_exception_text(client, monkeypatch):
    """str(e) on an SDK exception can carry request URLs, model identifiers and
    fragments of the payload. It is logged, not returned."""
    import app.main as m
    from unittest.mock import MagicMock

    secret = "postgres://admin:hunter2@10.0.0.5/internal"
    m.ai_client.models.generate_content = MagicMock(side_effect=RuntimeError(secret))
    m.ai_client.files.upload = MagicMock(return_value=MagicMock())

    r = client.post(
        "/api/v1/scout/analyze-film",
        files={"file": ("f.mp4", io.BytesIO(b"\0" * 1024), "video/mp4")},
        data={"position": "DB"},
    )
    assert r.status_code == 500
    assert secret not in r.text, "internal exception text reached the client"


# --------------------------------------------------------------- 7. updated_at

def test_updated_at_moves_on_update(db_available):
    """The column had a DEFAULT, which fires on INSERT and never again, so it
    stayed equal to created_at for the life of the row. There is no update
    endpoint yet — this guards the column against the day there is one."""
    ok, why = db_available
    if not ok:
        pytest.skip(f"No PostgreSQL reachable — {why}")

    import asyncpg

    from app.core.config import get_settings

    settings = get_settings()

    async def run():
        conn = await asyncpg.connect(
            host=settings.POSTGRES_HOST, port=settings.POSTGRES_PORT,
            user=settings.POSTGRES_USER, password=settings.POSTGRES_PASSWORD,
            database=settings.POSTGRES_DB,
        )
        try:
            row = await conn.fetchrow(
                "INSERT INTO athletes (first_name,last_name,position) "
                "VALUES ('Regression','Updated','WR') RETURNING id, created_at, updated_at"
            )
            await conn.execute(
                "UPDATE athletes SET weight_lbs = 205 WHERE id = $1", row["id"]
            )
            after = await conn.fetchrow(
                "SELECT created_at, updated_at FROM athletes WHERE id = $1", row["id"]
            )
            await conn.execute("DELETE FROM athletes WHERE id = $1", row["id"])
            return row, after
        finally:
            await conn.close()

    before, after = asyncio.run(run())
    assert after["updated_at"] > before["updated_at"], "updated_at did not move on UPDATE"
    assert after["created_at"] == before["created_at"], "created_at was rewritten"
