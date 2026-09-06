"""The half an operator uses: add apps, watch them, drive them.

Every route needs the admin key. The dashboard sends it as X-Admin-Key.
"""
from __future__ import annotations

import json

from fastapi import APIRouter, HTTPException, Query, status
from sqlalchemy import delete, func, select

from app.core.deps import AdminAuth, SessionDep, SettingsDep
from app.core.security import hash_key, hint, new_app_key
from app.models.orm import App, Event, Sample
from app.models.schemas import (
    ActionRequest,
    AppCreate,
    AppOut,
    AppUpdate,
    ConnectResult,
    EventOut,
    MaintenanceRequest,
    ProbeRequest,
    SampleOut,
)
from app.services import client, registry

router = APIRouter(prefix="/api/admin", tags=["admin"], dependencies=[AdminAuth])


async def _app_or_404(session: SessionDep, slug: str) -> App:
    app = await registry.get_by_slug(session, slug)
    if app is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No app '{slug}'")
    return app


@router.get("/overview")
async def overview(session: SessionDep, settings: SettingsDep) -> dict:
    total = int((await session.execute(select(func.count(App.id)))).scalar_one())
    by_status = {
        row[0]: int(row[1])
        for row in (
            await session.execute(select(App.status, func.count(App.id)).group_by(App.status))
        ).all()
    }
    events = int((await session.execute(select(func.count(Event.id)))).scalar_one())
    return {
        "apps": total,
        "by_status": by_status,
        "events": events,
        "poll_seconds": settings.poll_seconds,
        "open_registration": settings.open_registration,
    }


@router.get("/apps", response_model=list[AppOut])
async def list_apps(session: SessionDep) -> list[AppOut]:
    result = await session.execute(select(App).order_by(App.name))
    return [registry.app_out(a) for a in result.scalars().all()]


@router.post("/probe")
async def probe(payload: ProbeRequest) -> dict:
    """Dry run before saving, so 'Add app' can tell you what's wrong first."""
    return await registry.probe(
        payload.base_url.rstrip("/"), payload.control_path, payload.control_key
    )


@router.post("/apps", response_model=ConnectResult, status_code=status.HTTP_201_CREATED)
async def connect_app(payload: AppCreate, session: SessionDep) -> ConnectResult:
    """Add an app.

    Axiome probes it first and stores whatever it learns, then issues an app key
    for the app to push with. A probe that fails is not fatal — the app is saved
    so it can come up later — but the result says exactly what happened.
    """
    slug = payload.slug or registry.slugify(payload.name)
    if await registry.get_by_slug(session, slug) is not None:
        raise HTTPException(status.HTTP_409_CONFLICT, f"'{slug}' is already connected")

    found = await registry.probe(payload.base_url, payload.control_path, payload.control_key)
    reported = found.get("status") or {}

    token = new_app_key()
    app = App(
        slug=slug,
        name=payload.name,
        base_url=payload.base_url,
        control_path=payload.control_path,
        control_key=payload.control_key,
        app_key_hash=hash_key(token),
        app_key_hint=hint(token),
        kind=reported.get("app", "app") if reported else "app",
        version=reported.get("version", "") if reported else "",
        maintenance=bool(reported.get("maintenance")) if reported else False,
    )
    metrics = found.get("metrics") if isinstance(found.get("metrics"), dict) else {}

    if not found["reachable"]:
        app.status, app.status_detail = "unreachable", found.get("error") or "No response"
        detail = f"Saved, but nothing answered at {payload.base_url}."
    elif found.get("unauthorized"):
        app.status, app.status_detail = "degraded", "Control key rejected"
        detail = "Reachable, but the control key was rejected. Check AXIOME_CONTROL_KEY on the app."
    elif not found["authenticated"]:
        app.status, app.status_detail = "degraded", found.get("error") or "No control surface"
        detail = "Reachable, but its control surface did not answer."
    else:
        app.status, app.status_detail = "ok", ""
        registry.mark_seen(app, status="ok", metrics=metrics, latency_ms=found.get("latency_ms"))
        detail = "Connected."

    session.add(app)
    await session.flush()
    await registry.record_event(
        session, app=app, slug=app.slug, event="app.connected",
        data={"base_url": app.base_url, "status": app.status}, direction="outbound",
    )
    await session.commit()
    await session.refresh(app)

    return ConnectResult(
        ok=app.status == "ok",
        reachable=bool(found["reachable"]),
        authenticated=bool(found["authenticated"]),
        detail=detail,
        app=registry.app_out(app),
        app_key=token,
        probe=found,
    )


@router.get("/apps/{slug}", response_model=AppOut)
async def get_app(slug: str, session: SessionDep) -> AppOut:
    return registry.app_out(await _app_or_404(session, slug))


@router.patch("/apps/{slug}", response_model=AppOut)
async def update_app(slug: str, payload: AppUpdate, session: SessionDep) -> AppOut:
    app = await _app_or_404(session, slug)
    for field, value in payload.model_dump(exclude_unset=True).items():
        if value is not None:
            setattr(app, field, value.rstrip("/") if field == "base_url" else value)
    await session.commit()
    return registry.app_out(app)


@router.delete("/apps/{slug}", status_code=status.HTTP_204_NO_CONTENT)
async def remove_app(slug: str, session: SessionDep) -> None:
    """Forget an app. Its history goes with it; the app itself is untouched."""
    app = await _app_or_404(session, slug)
    await session.execute(delete(Sample).where(Sample.app_id == app.id))
    await session.execute(delete(Event).where(Event.app_id == app.id))
    await session.delete(app)
    await session.commit()


@router.post("/apps/{slug}/refresh", response_model=AppOut)
async def refresh_app(slug: str, session: SessionDep) -> AppOut:
    app = await _app_or_404(session, slug)
    await registry.refresh(session, app)
    await session.commit()
    return registry.app_out(app)


@router.post("/apps/{slug}/rotate-key", response_model=ConnectResult)
async def rotate_key(slug: str, session: SessionDep) -> ConnectResult:
    """Issue a fresh app key. The old one stops working immediately."""
    app = await _app_or_404(session, slug)
    token = new_app_key()
    app.app_key_hash = hash_key(token)
    app.app_key_hint = hint(token)
    await registry.record_event(
        session, app=app, slug=app.slug, event="app.key_rotated", data={}, direction="outbound"
    )
    await session.commit()
    return ConnectResult(
        ok=True, reachable=True, authenticated=True,
        detail="New key issued. Paste it into the app's AXIOME_APP_KEY and restart it.",
        app=registry.app_out(app), app_key=token,
    )


@router.post("/apps/{slug}/maintenance", response_model=AppOut)
async def set_maintenance(
    slug: str, payload: MaintenanceRequest, session: SessionDep
) -> AppOut:
    """Flip an app's kill switch through its own control surface."""
    app = await _app_or_404(session, slug)
    result = await client.call(
        app.base_url,
        app.control_path.rstrip("/") + "/maintenance",
        method="POST",
        key=app.control_key,
        body={"enabled": payload.enabled, "message": payload.message},
    )
    if not result.ok:
        raise HTTPException(
            status.HTTP_502_BAD_GATEWAY,
            f"{app.name} refused: {result.error or result.status_code}",
        )
    app.maintenance = payload.enabled
    app.status = "degraded" if payload.enabled else "ok"
    app.status_detail = "In maintenance" if payload.enabled else ""
    await registry.record_event(
        session, app=app, slug=app.slug, event="app.maintenance",
        data={"enabled": payload.enabled}, direction="outbound",
    )
    await session.commit()
    return registry.app_out(app)


@router.post("/apps/{slug}/action")
async def proxy_action(slug: str, payload: ActionRequest, session: SessionDep) -> dict:
    """Call any route on an app's control surface, with Axiome's stored key.

    This is what lets one dashboard drive apps that expose different things: the
    capability list says what an app has, and this passes the call through.
    """
    app = await _app_or_404(session, slug)
    result = await client.call(
        app.base_url,
        app.control_path.rstrip("/") + "/" + payload.path.lstrip("/"),
        method=payload.method,
        key=app.control_key,
        body=payload.body if payload.method == "POST" else None,
    )
    if payload.method == "POST":
        await registry.record_event(
            session, app=app, slug=app.slug, event=f"action.{payload.path.strip('/')}",
            data={"ok": result.ok}, direction="outbound",
        )
        await session.commit()
    if not result.ok:
        raise HTTPException(
            status.HTTP_502_BAD_GATEWAY, f"{app.name}: {result.error or result.status_code}"
        )
    return {"data": result.data, "latency_ms": result.latency_ms}


@router.get("/apps/{slug}/samples", response_model=list[SampleOut])
async def samples(
    slug: str, session: SessionDep, limit: int = Query(60, ge=1, le=200)
) -> list[SampleOut]:
    app = await _app_or_404(session, slug)
    result = await session.execute(
        select(Sample).where(Sample.app_id == app.id).order_by(Sample.at.desc()).limit(limit)
    )
    rows = list(result.scalars().all())[::-1]
    return [
        SampleOut(
            at=s.at, status=s.status, latency_ms=s.latency_ms, source=s.source,
            metrics=registry.loads(s.metrics, {}),
        )
        for s in rows
    ]


@router.get("/events", response_model=list[EventOut])
async def events(
    session: SessionDep,
    slug: str | None = None,
    limit: int = Query(50, ge=1, le=200),
) -> list[EventOut]:
    query = select(Event).order_by(Event.at.desc(), Event.id.desc())
    if slug:
        query = query.where(Event.app_slug == slug.lower())
    result = await session.execute(query.limit(limit))
    return [
        EventOut(
            id=e.id, app_slug=e.app_slug, event=e.event,
            data=registry.loads(e.data, {}), direction=e.direction, at=e.at,
        )
        for e in result.scalars().all()
    ]
