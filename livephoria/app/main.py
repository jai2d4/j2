"""Livephoria — a creator-owned fan experience and monetization platform.

The API lives under /api/v1. The single-page fan/creator app is served at /.
"""
from __future__ import annotations

import asyncio
import contextlib
import logging
from collections.abc import AsyncIterator
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from app.core.config import get_settings
from app.core.db import SessionLocal, create_all
from app.core.runtime import VERSION, runtime
from app.routers import (
    auth,
    control,
    creators,
    discover,
    live,
    messages,
    posts,
    safety,
    shop,
    subscriptions,
    wallet,
)
from app.services import axiome

log = logging.getLogger("livephoria")
WEB_DIR = Path(__file__).parent / "web"

CAPABILITIES = [
    "subscriptions",
    "pay-per-view",
    "tips",
    "live-events",
    "live-tickets",
    "merch",
    "digital-downloads",
    "direct-messages",
    "creator-earnings",
    "moderation-queue",
    "appeals",
    "age-assurance",
    "kyc",
    "payout-settlement",
]

# Reachable while the app is in maintenance, so the controller can turn it back on.
ALWAYS_OPEN_PREFIXES = ("/api/v1/control", "/api/v1/health")


async def _heartbeat_loop(app: FastAPI) -> None:
    settings = get_settings()
    client = axiome.client()
    while True:
        await asyncio.sleep(settings.axiome_heartbeat_seconds)
        try:
            async with SessionLocal() as session:
                metrics = await control.collect_metrics(session)
        except Exception as exc:  # a broken query must not kill the loop
            log.warning("heartbeat metrics failed: %s", exc)
            metrics = {"error": type(exc).__name__}
        await client.heartbeat(metrics)


@contextlib.asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    settings = get_settings()
    await create_all()

    client = axiome.configure(
        axiome.AxiomeConfig(
            base_url=settings.axiome_base_url,
            app_key=settings.axiome_app_key,
            public_url=settings.axiome_public_url,
            heartbeat_seconds=settings.axiome_heartbeat_seconds,
        )
    )
    task: asyncio.Task | None = None
    if client.config.enabled:
        await client.register(VERSION, CAPABILITIES)
        task = asyncio.create_task(_heartbeat_loop(app))
    else:
        log.info("Axiome control plane disabled (AXIOME_BASE_URL unset) — running standalone")

    yield

    if task is not None:
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task


app = FastAPI(
    title="Livephoria",
    version=VERSION,
    description="Where fans get closer. Creator subscriptions, exclusive content, LIVE, and commerce.",
    lifespan=lifespan,
)


@app.middleware("http")
async def maintenance_gate(request: Request, call_next):
    if runtime.maintenance and not request.url.path.startswith(ALWAYS_OPEN_PREFIXES):
        return JSONResponse(
            status_code=503,
            content={"detail": runtime.maintenance_message},
            headers={"Retry-After": "120"},
        )
    return await call_next(request)


@app.get("/api/v1/health", tags=["ops"])
async def health() -> dict:
    """Open, unauthenticated, and cheap — for uptime checks and container probes."""
    return {
        "status": "maintenance" if runtime.maintenance else "ok",
        "app": "livephoria",
        "version": VERSION,
        "uptime_seconds": runtime.uptime_seconds,
    }


for module in (
    auth,
    creators,
    posts,
    subscriptions,
    wallet,
    live,
    shop,
    messages,
    discover,
    safety,
    control,
):
    app.include_router(module.router)


if WEB_DIR.is_dir():
    app.mount("/static", StaticFiles(directory=WEB_DIR), name="static")

    @app.get("/", include_in_schema=False)
    async def index() -> FileResponse:
        return FileResponse(WEB_DIR / "index.html")
