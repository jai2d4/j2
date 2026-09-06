"""Async engine and session factory."""
from __future__ import annotations

from collections.abc import AsyncIterator

from sqlalchemy.ext.asyncio import AsyncSession, async_sessionmaker, create_async_engine
from sqlalchemy.orm import DeclarativeBase

from app.core.config import get_settings


class Base(DeclarativeBase):
    pass


_url = get_settings().resolved_database_url()
_kwargs: dict = {"echo": False, "future": True}
if _url.startswith("sqlite"):
    _kwargs["connect_args"] = {"check_same_thread": False}
else:
    _kwargs["pool_pre_ping"] = True

engine = create_async_engine(_url, **_kwargs)
SessionLocal = async_sessionmaker(engine, expire_on_commit=False, class_=AsyncSession)


async def create_all() -> None:
    from app.models import orm  # noqa: F401

    async with engine.begin() as conn:
        await conn.run_sync(Base.metadata.create_all)


async def get_session() -> AsyncIterator[AsyncSession]:
    async with SessionLocal() as session:
        yield session
