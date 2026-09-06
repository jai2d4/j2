"""Control plane — the surface Axiome drives this app through.

Every route requires the `X-Axiome-Key` shared secret. If AXIOME_CONTROL_KEY is
unset the routes refuse all callers, so an unconfigured deployment is closed,
not open.
"""
from __future__ import annotations

from typing import Annotated

from datetime import datetime, timezone

from fastapi import APIRouter, Depends, Header, HTTPException, Query, status
from pydantic import BaseModel, Field
from sqlalchemy import case, func, select

from app.core.config import Settings, get_settings
from app.core.deps import SessionDep, SettingsDep
from app.core.runtime import VERSION, runtime
from app.models.orm import (
    Appeal,
    CreatorProfile,
    LedgerEntry,
    LiveEvent,
    ModerationAction,
    Payout,
    PayoutAccount,
    Post,
    Report,
    Subscription,
    User,
)
from app.models.schemas import (
    AgeDecision,
    AppealDecision,
    AppealOut,
    KycDecision,
    ModerationDecision,
    PayoutOut,
    PayoutSettlement,
    ReportOut,
)
from app.routers.safety import report_out
from app.services import axiome, moderation, payments

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
        # A controller should be able to see a safety backlog building without
        # being asked, so these ride along on every heartbeat.
        "open_reports": await count(Report, Report.status == "open"),
        "urgent_reports": await count(
            Report, Report.status == "open", Report.priority == "urgent"
        ),
        "open_appeals": await count(Appeal, Appeal.status == "open"),
        "removed_posts": await count(Post, Post.status == "removed"),
        "suspended_users": await count(User, User.is_suspended.is_(True)),
        "kyc_pending": await count(PayoutAccount, PayoutAccount.status == "pending"),
        "payouts_pending": await count(Payout, Payout.status == "pending"),
        "payouts_pending_cents": int(
            (
                await session.execute(
                    select(func.coalesce(func.sum(Payout.amount_cents), 0)).where(
                        Payout.status == "pending"
                    )
                )
            ).scalar_one()
        ),
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


# ---------------------------------------------------------------- moderation queue


@router.get("/moderation/reports", response_model=list[ReportOut], dependencies=[ControlAuth])
async def moderation_queue(
    session: SessionDep,
    status_filter: str = Query("open", pattern="^(open|actioned|dismissed|all)$", alias="status"),
    limit: int = Query(50, ge=1, le=200),
) -> list[ReportOut]:
    """The review queue. Urgent reasons sort first, then oldest — nothing rots at the bottom."""
    query = select(Report)
    if status_filter != "all":
        query = query.where(Report.status == status_filter)
    result = await session.execute(
        query.order_by(
            case((Report.priority == "urgent", 0), else_=1), Report.created_at
        ).limit(limit)
    )
    reports = list(result.scalars().all())
    reporters = {}
    ids = {r.reporter_id for r in reports if r.reporter_id}
    if ids:
        rows = await session.execute(select(User).where(User.id.in_(ids)))
        reporters = {u.id: u for u in rows.scalars().all()}
    return [report_out(r, reporters.get(r.reporter_id)) for r in reports]


@router.post("/moderation/reports/{report_id}", dependencies=[ControlAuth])
async def decide_report(
    report_id: int, payload: ModerationDecision, session: SessionDep
) -> dict:
    """Act on a report. The action is recorded, and the subject can appeal it."""
    report = await session.get(Report, report_id)
    if report is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such report")
    if report.status != "open":
        raise HTTPException(status.HTTP_409_CONFLICT, "That report is already resolved")
    if payload.action not in moderation.ACTIONS:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "Unknown action")

    action = await moderation.apply_action(
        session,
        action=payload.action,
        target_type=report.target_type,
        target_id=report.target_id,
        note=payload.note,
        report=report,
    )
    await session.commit()
    return {
        "report_id": report.id,
        "report_status": report.status,
        "action_id": action.id,
        "action": action.action,
        "subject_user_id": action.subject_user_id,
        "appealable": action.appealable,
    }


@router.post("/moderation/actions", dependencies=[ControlAuth])
async def act_directly(payload: dict, session: SessionDep) -> dict:
    """Take an action with no report behind it — proactive review, or a legal order."""
    for field in ("action", "target_type", "target_id"):
        if field not in payload:
            raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, f"{field} is required")
    if payload["action"] not in moderation.ACTIONS:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "Unknown action")
    if payload["target_type"] not in moderation.TARGET_TYPES:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "Unknown target type")

    action = await moderation.apply_action(
        session,
        action=payload["action"],
        target_type=payload["target_type"],
        target_id=int(payload["target_id"]),
        reason=payload.get("reason", ""),
        note=payload.get("note", ""),
    )
    await session.commit()
    return {"action_id": action.id, "action": action.action, "subject_user_id": action.subject_user_id}


@router.get("/moderation/appeals", response_model=list[AppealOut], dependencies=[ControlAuth])
async def appeal_queue(
    session: SessionDep,
    status_filter: str = Query("open", pattern="^(open|upheld|rejected|all)$", alias="status"),
) -> list[AppealOut]:
    query = select(Appeal, ModerationAction, User).join(
        ModerationAction, ModerationAction.id == Appeal.action_id
    ).join(User, User.id == Appeal.user_id)
    if status_filter != "all":
        query = query.where(Appeal.status == status_filter)
    result = await session.execute(query.order_by(Appeal.created_at))
    return [
        AppealOut(
            id=ap.id,
            action_id=ac.id,
            action=ac.action,
            target_type=ac.target_type,
            target_id=ac.target_id,
            body=ap.body,
            status=ap.status,
            created_at=ap.created_at,
            decided_at=ap.decided_at,
            decision_note=ap.decision_note,
            user_display_name=u.display_name,
        )
        for ap, ac, u in result.all()
    ]


@router.post("/moderation/appeals/{appeal_id}", dependencies=[ControlAuth])
async def decide_appeal(appeal_id: int, payload: AppealDecision, session: SessionDep) -> dict:
    """Upholding an appeal reverses the original action; rejecting it leaves it standing."""
    if payload.decision not in {"upheld", "rejected"}:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "decision must be upheld or rejected")
    appeal = await session.get(Appeal, appeal_id)
    if appeal is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such appeal")
    if appeal.status != "open":
        raise HTTPException(status.HTTP_409_CONFLICT, "That appeal is already decided")

    original = await session.get(ModerationAction, appeal.action_id)
    reversed_action = None
    if payload.decision == "upheld":
        reversal = moderation.reversal_of(original.action)
        if reversal:
            record = await moderation.apply_action(
                session,
                action=reversal,
                target_type=original.target_type,
                target_id=original.target_id,
                note=f"appeal {appeal.id} upheld",
            )
            reversed_action = record.action

    appeal.status = payload.decision
    appeal.decided_at = datetime.now(timezone.utc)
    appeal.decision_note = payload.note
    await session.commit()
    return {"appeal_id": appeal.id, "status": appeal.status, "reversed_with": reversed_action}


# ---------------------------------------------------------------- age assurance


@router.get("/age-verifications", dependencies=[ControlAuth])
async def pending_age_checks(session: SessionDep) -> list[dict]:
    result = await session.execute(
        select(User).where(User.age_check_status == "pending").order_by(User.id)
    )
    return [
        {
            "user_id": u.id,
            "display_name": u.display_name,
            "reference": u.age_check_reference,
            "submitted": u.created_at.isoformat(),
        }
        for u in result.scalars().all()
    ]


@router.post("/age-verifications/{user_id}", dependencies=[ControlAuth])
async def decide_age_check(user_id: int, payload: AgeDecision, session: SessionDep) -> dict:
    user = await session.get(User, user_id)
    if user is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such user")
    user.age_check_status = "verified" if payload.approved else "failed"
    user.age_verified_at = datetime.now(timezone.utc) if payload.approved else None
    await session.commit()
    return {"user_id": user.id, "age_check_status": user.age_check_status}


# ---------------------------------------------------------------- payouts and KYC


@router.get("/kyc", dependencies=[ControlAuth])
async def kyc_queue(
    session: SessionDep,
    status_filter: str = Query("pending", pattern="^(pending|approved|rejected|all)$", alias="status"),
) -> list[dict]:
    query = select(PayoutAccount, User).join(User, User.id == PayoutAccount.user_id)
    if status_filter != "all":
        query = query.where(PayoutAccount.status == status_filter)
    result = await session.execute(query.order_by(PayoutAccount.created_at))
    return [
        {
            "id": a.id,
            "user_id": a.user_id,
            "display_name": u.display_name,
            "legal_name": a.legal_name,
            "country": a.country,
            "status": a.status,
            "submitted": a.created_at.isoformat(),
        }
        for a, u in result.all()
    ]


@router.post("/kyc/{account_id}", dependencies=[ControlAuth])
async def decide_kyc(account_id: int, payload: KycDecision, session: SessionDep) -> dict:
    account = await session.get(PayoutAccount, account_id)
    if account is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such payout account")
    account.status = "approved" if payload.approved else "rejected"
    account.note = payload.note
    account.provider_reference = payload.provider_reference or account.provider_reference
    account.reviewed_at = datetime.now(timezone.utc)
    await session.commit()
    return {"account_id": account.id, "status": account.status}


@router.get("/payouts", response_model=list[PayoutOut], dependencies=[ControlAuth])
async def payout_queue(
    session: SessionDep,
    status_filter: str = Query("pending", pattern="^(pending|paid|failed|all)$", alias="status"),
) -> list[PayoutOut]:
    query = select(Payout, User).join(User, User.id == Payout.user_id)
    if status_filter != "all":
        query = query.where(Payout.status == status_filter)
    result = await session.execute(query.order_by(Payout.created_at))
    return [
        PayoutOut(
            id=p.id,
            amount_cents=p.amount_cents,
            status=p.status,
            created_at=p.created_at,
            settled_at=p.settled_at,
            note=p.note,
            provider_reference=p.provider_reference,
            user_display_name=u.display_name,
        )
        for p, u in result.all()
    ]


@router.post("/payouts/{payout_id}", dependencies=[ControlAuth])
async def settle_payout(payout_id: int, payload: PayoutSettlement, session: SessionDep) -> dict:
    """Record what the money actually did.

    `failed` returns the amount to the creator's balance and leaves the failed
    row in place, so the attempt stays on the record.
    """
    if payload.status not in {"paid", "failed"}:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "status must be paid or failed")
    record = await session.get(Payout, payout_id)
    if record is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such payout")
    if record.status != "pending":
        raise HTTPException(status.HTTP_409_CONFLICT, "That payout is already settled")

    user = await session.get(User, record.user_id)
    if payload.status == "failed":
        await payments.reverse_payout(session, user, record)

    record.status = payload.status
    record.provider_reference = payload.provider_reference or None
    record.note = payload.note
    record.settled_at = datetime.now(timezone.utc)
    await session.commit()
    return {
        "payout_id": record.id,
        "status": record.status,
        "earnings_balance_cents": user.earnings_balance_cents,
    }
