"""Registry operations shared by the ingest and admin halves."""
from __future__ import annotations

import json
import re
from datetime import datetime, timezone

from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import get_settings
from app.models.orm import App, Event, Sample
from app.models.schemas import AppOut
from app.services import client

# Keep a bounded history per app; this is a dashboard, not a metrics store.
MAX_SAMPLES = 200


def slugify(name: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    return (slug or "app")[:60]


def loads(raw: str, fallback):
    try:
        return json.loads(raw) if raw else fallback
    except json.JSONDecodeError:
        return fallback


def app_out(app: App) -> AppOut:
    seen = app.last_seen_at
    seconds = None
    if seen is not None:
        if seen.tzinfo is None:
            seen = seen.replace(tzinfo=timezone.utc)
        seconds = int((datetime.now(timezone.utc) - seen).total_seconds())
    return AppOut(
        id=app.id,
        slug=app.slug,
        name=app.name,
        kind=app.kind,
        version=app.version,
        base_url=app.base_url,
        public_url=app.public_url,
        control_path=app.control_path,
        capabilities=loads(app.capabilities, []),
        status=app.status,
        status_detail=app.status_detail,
        maintenance=app.maintenance,
        enabled=app.enabled,
        metrics=loads(app.last_metrics, {}),
        last_seen_at=app.last_seen_at,
        last_latency_ms=app.last_latency_ms,
        seconds_since_seen=seconds,
        has_control_key=bool(app.control_key),
        app_key_hint=app.app_key_hint,
        created_at=app.created_at,
    )


async def get_by_slug(session: AsyncSession, slug: str) -> App | None:
    result = await session.execute(select(App).where(App.slug == slug.lower()))
    return result.scalar_one_or_none()


async def record_event(
    session: AsyncSession,
    *,
    app: App | None,
    slug: str,
    event: str,
    data: dict,
    direction: str = "inbound",
    at: datetime | None = None,
) -> Event:
    row = Event(
        app_id=app.id if app else None,
        app_slug=slug,
        event=event,
        data=json.dumps(data)[:20_000],
        direction=direction,
        at=at or datetime.now(timezone.utc),
    )
    session.add(row)
    return row


async def record_sample(
    session: AsyncSession,
    app: App,
    *,
    status: str,
    metrics: dict,
    latency_ms: float | None,
    source: str,
) -> None:
    session.add(
        Sample(
            app_id=app.id,
            status=status,
            metrics=json.dumps(metrics)[:20_000],
            latency_ms=latency_ms,
            source=source,
        )
    )
    # Trim rather than grow forever.
    old = await session.execute(
        select(Sample.id)
        .where(Sample.app_id == app.id)
        .order_by(Sample.at.desc())
        .offset(MAX_SAMPLES)
    )
    stale = list(old.scalars().all())
    if stale:
        for sample_id in stale:
            row = await session.get(Sample, sample_id)
            if row is not None:
                await session.delete(row)


def mark_seen(app: App, *, status: str, metrics: dict, latency_ms: float | None = None) -> None:
    app.status = status
    app.last_metrics = json.dumps(metrics)[:20_000]
    app.last_seen_at = datetime.now(timezone.utc)
    app.last_latency_ms = latency_ms


async def probe(base_url: str, control_path: str, control_key: str) -> dict:
    """Ask an app who it is. Used by 'connect' and by the poller.

    Distinguishes three failures on purpose: nothing answered, something answered
    but rejected the key, and answered fine.
    """
    settings = get_settings()
    timeout = settings.poll_timeout_seconds

    health = await client.call(base_url, "/api/v1/health", timeout=timeout)
    status = await client.call(
        base_url, control_path.rstrip("/") + "/status", key=control_key, timeout=timeout
    )
    metrics = await client.call(
        base_url, control_path.rstrip("/") + "/metrics", key=control_key, timeout=timeout
    )

    return {
        "reachable": health.reachable or status.reachable,
        "authenticated": status.ok,
        "health": health.data if health.ok else None,
        "status": status.data if status.ok else None,
        "metrics": metrics.data if metrics.ok else {},
        "latency_ms": status.latency_ms or health.latency_ms,
        "error": status.error or health.error,
        "unauthorized": status.unauthorized,
    }


async def refresh(session: AsyncSession, app: App) -> dict:
    """Pull current state from one app and write it down."""
    if not app.base_url:
        app.status = "unknown"
        app.status_detail = "No base URL — this app only pushes"
        return {}

    result = await probe(app.base_url, app.control_path, app.control_key)

    if not result["reachable"]:
        app.status = "unreachable"
        app.status_detail = result["error"] or "No response"
    elif result["unauthorized"]:
        app.status = "degraded"
        app.status_detail = "Control key rejected"
    elif not result["authenticated"]:
        app.status = "degraded"
        app.status_detail = result["error"] or "Control surface did not answer"
    else:
        reported = (result["status"] or {}).get("maintenance")
        app.maintenance = bool(reported)
        app.status = "degraded" if reported else "ok"
        app.status_detail = "In maintenance" if reported else ""
        app.version = (result["status"] or {}).get("version", app.version)

    metrics = result["metrics"] if isinstance(result["metrics"], dict) else {}
    mark_seen(
        app,
        status=app.status,
        metrics=metrics or loads(app.last_metrics, {}),
        latency_ms=result["latency_ms"],
    )
    await record_sample(
        session, app, status=app.status, metrics=metrics, latency_ms=result["latency_ms"], source="poll"
    )
    return result
