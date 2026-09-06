"""Every test runs against a fresh in-memory database and a real ASGI client."""
from __future__ import annotations

import os

os.environ.setdefault("JWT_SECRET", "test-secret-not-for-production-32-bytes-min")
os.environ.setdefault("APP_ENV", "test")
os.environ.setdefault("DATABASE_URL", "sqlite+aiosqlite:///:memory:")
os.environ.setdefault("AXIOME_CONTROL_KEY", "test-control-key")
os.environ.setdefault("AXIOME_BASE_URL", "")

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient
from sqlalchemy.ext.asyncio import async_sessionmaker, create_async_engine
from sqlalchemy.pool import StaticPool

from app.core.db import Base, get_session
from app.core.runtime import runtime
from app.main import app

CONTROL_KEY = "test-control-key"


@pytest_asyncio.fixture
async def session_factory():
    engine = create_async_engine(
        "sqlite+aiosqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)
    yield async_sessionmaker(engine, expire_on_commit=False)
    await engine.dispose()


@pytest_asyncio.fixture
async def client(session_factory):
    async def override():
        async with session_factory() as session:
            yield session

    app.dependency_overrides[get_session] = override
    runtime.maintenance = False
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://test") as ac:
        yield ac
    app.dependency_overrides.clear()
    runtime.maintenance = False


class Actor:
    """A signed-in account plus the header bundle to act as them."""

    def __init__(self, client: AsyncClient, data: dict) -> None:
        self.client = client
        self.token = data["access_token"]
        self.user = data["user"]
        self.id = data["user"]["id"]
        self.headers = {"Authorization": f"Bearer {self.token}"}
        self.handle: str | None = None

    async def topup(self, cents: int) -> None:
        r = await self.client.post(
            "/api/v1/me/wallet/topup", json={"amount_cents": cents}, headers=self.headers
        )
        assert r.status_code == 200, r.text

    async def wallet(self) -> dict:
        r = await self.client.get("/api/v1/me/wallet", headers=self.headers)
        return r.json()


@pytest_asyncio.fixture
async def make_user(client):
    counter = {"n": 0}

    async def _make(name: str = "Fan", emoji: str = "🙂") -> Actor:
        counter["n"] += 1
        r = await client.post(
            "/api/v1/auth/register",
            json={
                "email": f"user{counter['n']}@example.test",
                "password": "correct-horse-battery",
                "display_name": name,
                "avatar_emoji": emoji,
            },
        )
        assert r.status_code == 201, r.text
        return Actor(client, r.json())

    return _make


@pytest_asyncio.fixture
async def make_creator(client, make_user):
    counter = {"n": 0}

    async def _make(name: str = "Creator", price_cents: int = 999) -> tuple[Actor, str, int]:
        """Returns (actor, handle, tier_id) with one active membership tier."""
        counter["n"] += 1
        actor = await make_user(name)
        handle = f"creator{counter['n']}"
        r = await client.post(
            "/api/v1/creators",
            json={"handle": handle, "display_name": name, "tagline": "Test channel"},
            headers=actor.headers,
        )
        assert r.status_code == 201, r.text
        actor.handle = handle

        r = await client.post(
            "/api/v1/creators/me/tiers",
            json={"name": "Insider", "price_cents": price_cents, "perks": ["All posts"]},
            headers=actor.headers,
        )
        assert r.status_code == 201, r.text
        return actor, handle, r.json()["id"]

    return _make
