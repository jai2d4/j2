"""Async Postgres connection pool — asyncpg, lifespan-managed.

db/init_schema.sql is the single source of truth for table shape; this
module only opens/closes the pool and teaches asyncpg to (de)serialize
JSONB columns as plain Python dicts/lists instead of raw strings.
"""
from __future__ import annotations

import json
from contextlib import asynccontextmanager
from typing import AsyncIterator

import asyncpg
from fastapi import FastAPI, Request

from app.core.config import get_settings


async def _init_connection(conn: asyncpg.Connection) -> None:
    await conn.set_type_codec(
        "jsonb", encoder=json.dumps, decoder=json.loads, schema="pg_catalog", format="text",
    )


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    settings = get_settings()
    app.state.db_pool = await asyncpg.create_pool(
        dsn=settings.database_url, min_size=1, max_size=10, init=_init_connection,
    )
    try:
        yield
    finally:
        await app.state.db_pool.close()


def get_pool(request: Request) -> asyncpg.Pool:
    return request.app.state.db_pool
