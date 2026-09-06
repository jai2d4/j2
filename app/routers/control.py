"""Control plane — the surface Axiome drives this app through.

Every route requires the `X-Axiome-Key` shared secret. If AXIOME_CONTROL_KEY is
unset the routes refuse all callers, so an unconfigured deployment is closed,
not open.
"""
from __future__ import annotations

from typing import Annotated

from fastapi import APIRouter, Depends, Header, HTTPException, status
from pydantic import BaseModel, Field
from sqlalchemy import func, select

from app.core.config import Settings, get_settings
from app.core.deps import SessionDep, SettingsDep
from app.core.runtime import VERSION, runtime
from app.models.orm import CreatorProfile, LedgerEntry, LiveEvent, Post, Subscription, User
from app.services import axiome

router = APIRouter(prefix="/api/v1/control", tags=["control"])


async def require_control_key(
    settings: Annotated[Settings, Depends(get_settings)],
    x_axiome_key: Annotated[str | None, Header()] = None,
) -> None:
    if not settings.axiome_control_key:
        raise HTTPException(
            status.HTTP_503_SERVICE_UNAVAILABLE,
            "Control plane is not configured (set AXIOME_CONTROL_KEY)",
        )
    if x_axiome_key != settings.axiome_control_key:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Bad control key")


ControlAuth = Depends(require_control_key)


class MaintenanceRequest(BaseModel):
    enabled: bool
    message: str = Field(default="", max_length=300)


async def collect_metrics(session: SessionDep) -> dict:
    """The numbers Axiome gets on every heartbeat. All read straight from the database."""

    async def count(model, *where) -> int:
        stmt = select(func.count(model.id))
        if where:
            stmt = stmt.where(*where)
        return int((await session.execute(stmt)).scalar_one())

    gross = await session.execute(
        select(func.coalesce(func.sum(LedgerEntry.gross_cents), 0)).where(
            LedgerEntry.kind.notin_(["topup", "payout"])
        )
    )
    fees = await session.execute(
        select(func.coalesce(func.sum(LedgerEntry.fee_cents), 0)).where(
            LedgerEntry.kind.notin_(["topup", "payout"])
        )
    )
    return {
        "users": await count(User),
        "creators": await count(CreatorProfile),
        "posts": await count(Post),
        "active_subscriptions": await count(Subscription, Subscription.status == "active"),
        "live_now": await count(LiveEvent, LiveEvent.status == "live"),
        "gross_volume_cents": int(gross.scalar_one()),
        "platform_fees_cents": int(fees.scalar_one()),
    }


@router.get("/status", dependencies=[ControlAuth])
async def status_report(session: SessionDep, settings: SettingsDep) -> dict:
    try:
        await session.execute(select(1))
        database = "ok"
    except Exception as exc:  # surfaced to the controller rather than raised
        database = f"unreachable: {type(exc).__name__}"

    return {
        "app": "livephoria",
        "version": VERSION,
        "env": settings.app_env,
        "uptime_seconds": runtime.uptime_seconds,
        "maintenance": runtime.maintenance,
        "database": database,
        "axiome": {
            "configured": bool(settings.axiome_base_url),
            "base_url": settings.axiome_base_url,
            "last_results": axiome.client().last_result,
        },
    }


@router.get("/metrics", dependencies=[ControlAuth])
async def metrics(session: SessionDep) -> dict:
    return await collect_metrics(session)


@router.get("/config", dependencies=[ControlAuth])
async def config(settings: SettingsDep) -> dict:
    """Effective settings, secrets excluded."""
    return {
        "app_env": settings.app_env,
        "platform_fee_bps": settings.platform_fee_bps,
        "payments_provider": settings.payments_provider,
        "max_transaction_cents": settings.max_transaction_cents,
        "database": settings.resolved_database_url().split("@")[-1],
        "axiome_base_url": settings.axiome_base_url,
        "axiome_heartbeat_seconds": settings.axiome_heartbeat_seconds,
    }


@router.post("/maintenance", dependencies=[ControlAuth])
async def set_maintenance(payload: MaintenanceRequest) -> dict:
    """Kill switch. While on, every route except control and health returns 503."""
    runtime.maintenance = payload.enabled
    if payload.message:
        runtime.maintenance_message = payload.message
    return {"maintenance": runtime.maintenance, "message": runtime.maintenance_message}


@router.post("/users/{user_id}/suspend", dependencies=[ControlAuth])
async def suspend_user(user_id: int, session: SessionDep) -> dict:
    user = await session.get(User, user_id)
    if user is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such user")
    user.is_suspended = True
    await session.commit()
    return {"user_id": user.id, "suspended": True}


@router.post("/users/{user_id}/restore", dependencies=[ControlAuth])
async def restore_user(user_id: int, session: SessionDep) -> dict:
    user = await session.get(User, user_id)
    if user is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such user")
    user.is_suspended = False
    await session.commit()
    return {"user_id": user.id, "suspended": False}
