"""Each test gets a fresh in-memory registry and a real ASGI client."""
from __future__ import annotations

import os

os.environ.setdefault("ADMIN_KEY", "test-admin-key")
os.environ.setdefault("APP_ENV", "test")
os.environ.setdefault("DATABASE_URL", "sqlite+aiosqlite:///:memory:")
os.environ.setdefault("POLL_SECONDS", "0")
os.environ.setdefault("OPEN_REGISTRATION", "false")

import pytest_asyncio
from httpx import ASGITransport, AsyncClient
from sqlalchemy.ext.asyncio import async_sessionmaker, create_async_engine
from sqlalchemy.pool import StaticPool

from app.core.db import Base, get_session
from app.main import app

ADMIN = {"X-Admin-Key": "test-admin-key"}


@pytest_asyncio.fixture
async def client():
    engine = create_async_engine(
        "sqlite+aiosqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
    factory = async_sessionmaker(engine, expire_on_commit=False)

    async def override():
        async with factory() as session:
            yield session

    app.dependency_overrides[get_session] = override
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://axiome") as ac:
        yield ac
    app.dependency_overrides.clear()
    await engine.dispose()


@pytest_asyncio.fixture
async def connected(client):
    """An app added by hand. The probe fails (nothing is listening) — on purpose:
    that is the normal case for an app that is not up yet, and it must still save."""

    async def _add(name: str = "Livephoria", base_url: str = "http://127.0.0.1:9") -> dict:
        r = await client.post(
            "/api/admin/apps",
            json={"name": name, "base_url": base_url, "control_key": "ck"},
            headers=ADMIN,
        )
        assert r.status_code == 201, r.text
        return r.json()

    return _add
