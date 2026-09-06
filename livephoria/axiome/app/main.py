"""Axiome — the control plane for your apps.

Two halves:
  * /api/apps/*  — apps report in (register, heartbeat, events)
  * /api/admin/* — you add apps, watch them, and drive them

The dashboard is served at /.
"""
from __future__ import annotations

import asyncio
import contextlib
import logging
from collections.abc import AsyncIterator
from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from sqlalchemy import select

from app.core.config import get_settings
from app.core.db import SessionLocal, create_all
from app.models.orm import App
from app.routers import admin, ingest
from app.services import registry

log = logging.getLogger("axiome")
WEB_DIR = Path(__file__).parent / "web"
VERSION = "0.1.0"


async def _poll_loop() -> None:
    """Pull status from every app that has a base URL.

    Polling is what makes an app show up without changing the app at all: push is
    an optimisation, not a requirement.
    """
    settings = get_settings()
    while True:
        await asyncio.sleep(settings.poll_seconds)
        try:
            async with SessionLocal() as session:
                result = await session.execute(select(App).where(App.enabled.is_(True)))
                for app in result.scalars().all():
                    if app.base_url:
                        await registry.refresh(session, app)
                await session.commit()
        except Exception as exc:  # one bad round must not end the loop
            log.warning("poll round failed: %s", exc)


@contextlib.asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    settings = get_settings()
    await create_all()

    if not settings.admin_key and not settings.is_production:
        # Printed, not silently blank: the dashboard needs it to open.
        print("\n" + "=" * 62)
        print("  Axiome admin key (this run only — set ADMIN_KEY to keep one):")
        print(f"  {settings.resolved_admin_key()}")
        print("=" * 62 + "\n", flush=True)

    task = asyncio.create_task(_poll_loop()) if settings.poll_seconds > 0 else None
    yield
    if task is not None:
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task


app = FastAPI(
    title="Axiome",
    version=VERSION,
    description="Control plane: one place to add, watch, and drive your apps.",
    lifespan=lifespan,
)

app.include_router(ingest.router)
app.include_router(admin.router)


@app.get("/api/health", tags=["ops"])
async def health() -> dict:
    return {"status": "ok", "app": "axiome", "version": VERSION}


if WEB_DIR.is_dir():
    app.mount("/static", StaticFiles(directory=WEB_DIR), name="static")

    @app.get("/", include_in_schema=False)
    async def index() -> FileResponse:
        return FileResponse(WEB_DIR / "index.html")
