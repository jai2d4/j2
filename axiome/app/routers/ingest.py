"""The half apps call: register, heartbeat, events.

These paths are the contract documented in AXIOME.md, and the payloads match what
Livephoria's client already sends.
"""
from __future__ import annotations

import json
from typing import Annotated

from fastapi import APIRouter, Header, HTTPException, status

from app.core.deps import SessionDep, SettingsDep, app_from_key, bearer
from app.core.security import hash_key, hint, new_app_key
from app.models.orm import App
from app.models.schemas import Ack, EventRequest, HeartbeatRequest, RegisterRequest
from app.services import registry

router = APIRouter(prefix="/api/apps", tags=["ingest"])


@router.post("/register", response_model=Ack)
async def register(
    payload: RegisterRequest,
    session: SessionDep,
    settings: SettingsDep,
    authorization: Annotated[str | None, Header()] = None,
) -> Ack:
    """An app announcing itself.

    A known app must present the key Axiome issued it. An unknown app is refused
    unless OPEN_REGISTRATION is on — otherwise anything that can reach this port
    could plant itself in the registry.
    """
    existing = await registry.get_by_slug(session, payload.slug)

    if existing is not None:
        if await app_from_key(session, payload.slug, authorization) is None:
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Bad or missing app key")
        app = existing
    else:
        if not settings.open_registration:
            raise HTTPException(
                status.HTTP_403_FORBIDDEN,
                "Unknown app. Add it in Axiome first to get an app key, "
                "or set OPEN_REGISTRATION=true for local development.",
            )
        app = App(slug=payload.slug)
        # In open registration the app brings its own key and we adopt it.
        token = bearer(authorization) or new_app_key()
        app.app_key_hash = hash_key(token)
        app.app_key_hint = hint(token)
        session.add(app)

    app.name = payload.name or app.name or payload.slug
    app.kind = payload.kind or app.kind
    app.version = payload.version or app.version
    app.public_url = payload.public_url or app.public_url
    app.control_path = payload.control_url or app.control_path
    app.capabilities = json.dumps(payload.capabilities)
    app.status = "ok"
    app.status_detail = ""
    registry.mark_seen(app, status="ok", metrics=registry.loads(app.last_metrics, {}))

    await registry.record_event(
        session,
        app=app,
        slug=app.slug,
        event="app.registered",
        data={"version": app.version, "capabilities": payload.capabilities},
    )
    await session.commit()
    return Ack(app=app.slug, detail="Registered")


@router.post("/heartbeat", response_model=Ack)
async def heartbeat(
    payload: HeartbeatRequest,
    session: SessionDep,
    authorization: Annotated[str | None, Header()] = None,
) -> Ack:
    app = await app_from_key(session, payload.slug, authorization)
    if app is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Bad or missing app key")

    reported = payload.status if payload.status in {"ok", "degraded"} else "ok"
    registry.mark_seen(app, status=reported, metrics=payload.metrics)
    app.status_detail = "" if reported == "ok" else "Reported by the app"
    await registry.record_sample(
        session, app, status=reported, metrics=payload.metrics, latency_ms=None, source="push"
    )
    await session.commit()
    return Ack(app=app.slug, detail="Heartbeat recorded")


@router.post("/events", response_model=Ack)
async def event(
    payload: EventRequest,
    session: SessionDep,
    authorization: Annotated[str | None, Header()] = None,
) -> Ack:
    app = await app_from_key(session, payload.slug, authorization)
    if app is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Bad or missing app key")
    await registry.record_event(
        session, app=app, slug=app.slug, event=payload.event, data=payload.data, at=payload.at
    )
    await session.commit()
    return Ack(app=app.slug, detail="Event recorded")
