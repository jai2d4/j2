"""
Module 5 — async PostgreSQL wiring.

Kept deliberately optional at the connection level: metric-sieve, makeup-grade,
and analyze-film all still work with a cold or unreachable DB (the in-memory
matrices remain the system of record for the sieve, per the original design).
Only the athlete/evaluation persistence endpoints — and truth-report's
best-effort save of its own result — actually need the database up.
"""
from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from typing import AsyncIterator

from fastapi import HTTPException
from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine

from app.core.config import get_settings

logger = logging.getLogger("tru.db")

settings = get_settings()
_engine = create_async_engine(settings.database_url, pool_pre_ping=True, pool_size=5, max_overflow=5)
_SessionLocal = async_sessionmaker(_engine, expire_on_commit=False, class_=AsyncSession)


async def get_db() -> AsyncIterator[AsyncSession]:
    """FastAPI dependency for routes that require the database.
    Raises 503 rather than a raw connection error if PostgreSQL is unreachable."""
    try:
        async with _SessionLocal() as session:
            yield session
    except ConnectionError as e:
        raise HTTPException(status_code=503, detail=f"Database unavailable: {e}")


@asynccontextmanager
async def best_effort_session() -> AsyncIterator[AsyncSession | None]:
    """For call sites that should persist opportunistically (e.g. truth-report)
    without failing the whole request if the DB happens to be down.

    Only *opening* the session is best-effort. The caller's own block is not:
    wrapping the yield in `try/except Exception` meant an error raised inside
    the `async with` body was thrown back in at the yield, caught here, logged
    as "DB unavailable" — which it was not — and then followed by a second
    yield, which an async generator cannot do. The caller saw
    `RuntimeError: generator didn't stop after athrow()` instead of its own
    exception, with a misleading warning in the log pointing at the database.

    So the session is opened first, and the yield sits outside the guard. A
    caller's bug propagates as itself; only a genuine connection failure
    produces the `None` this exists for.
    """
    session: AsyncSession | None = None
    try:
        session = _SessionLocal()
        # Force the connection now. Without this the session is lazy and a dead
        # database would not surface until the caller's first statement, which
        # is past the point where this function can still hand back None.
        await session.connection()
    except Exception as e:  # noqa: BLE001 — any failure to *reach* the DB is non-fatal here
        logger.warning("Skipping best-effort persistence — DB unavailable: %s", e)
        if session is not None:
            await session.close()
        yield None
        return

    try:
        yield session
    finally:
        await session.close()
