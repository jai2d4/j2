"""The money screens: wallet, tips, earnings, payouts."""
from __future__ import annotations

from datetime import datetime, timedelta, timezone

from fastapi import APIRouter, HTTPException, Query, status
from sqlalchemy import func, select

from app.core.deps import CreatorDep, SessionDep, SettingsDep, UserDep
from app.models.orm import Follow, LedgerEntry, Subscription, User
from app.models.schemas import (
    EarningsLine,
    EarningsOut,
    LedgerOut,
    PurchaseResult,
    TipRequest,
    TopUpRequest,
    WalletOut,
)
from app.services import catalog, payments
from app.services.access import money

router = APIRouter(prefix="/api/v1", tags=["money"])


@router.get("/me/wallet", response_model=WalletOut)
async def wallet(user: UserDep) -> WalletOut:
    return WalletOut(
        wallet_balance_cents=user.wallet_balance_cents,
        earnings_balance_cents=user.earnings_balance_cents,
    )


@router.post("/me/wallet/topup", response_model=WalletOut)
async def topup(payload: TopUpRequest, user: UserDep, session: SessionDep) -> WalletOut:
    """Add spendable credit.

    With the mock provider this is free money for testing — no card is charged.
    """
    try:
        await payments.top_up(session, user, payload.amount_cents)
    except payments.PaymentError as exc:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, str(exc)) from exc
    await session.commit()
    return WalletOut(
        wallet_balance_cents=user.wallet_balance_cents,
        earnings_balance_cents=user.earnings_balance_cents,
    )


@router.post("/creators/{handle}/tip", response_model=PurchaseResult)
async def tip(
    handle: str, payload: TipRequest, user: UserDep, session: SessionDep, settings: SettingsDep
) -> PurchaseResult:
    creator = await catalog.get_creator_by_handle(session, handle)
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No creator @{handle}")
    if creator.user_id == user.id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "You can't tip yourself")
    if not creator.tips_enabled:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, f"@{handle} has tips turned off")
    if payload.amount_cents < creator.min_tip_cents:
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"@{handle}'s minimum tip is {money(creator.min_tip_cents)}",
        )
    if payload.amount_cents > settings.max_transaction_cents:
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"Single payments are capped at {money(settings.max_transaction_cents)}",
        )

    creator_user = await session.get(User, creator.user_id)
    try:
        await payments.charge(
            session,
            payer=user,
            creator=creator,
            creator_user=creator_user,
            amount_cents=payload.amount_cents,
            kind="tip",
            reference_type="creator",
            reference_id=creator.id,
            note=payload.note[:200],
        )
    except payments.InsufficientFunds as exc:
        raise HTTPException(status.HTTP_402_PAYMENT_REQUIRED, str(exc)) from exc
    await session.commit()
    return PurchaseResult(
        ok=True,
        charged_cents=payload.amount_cents,
        wallet_balance_cents=user.wallet_balance_cents,
        detail=f"Sent {money(payload.amount_cents)} to @{handle}",
    )


@router.get("/me/earnings", response_model=EarningsOut)
async def earnings(
    creator: CreatorDep,
    user: UserDep,
    session: SessionDep,
    settings: SettingsDep,
    days: int = Query(30, ge=1, le=365),
) -> EarningsOut:
    """What came in, what the platform took, and what's yours — over a window."""
    since = datetime.now(timezone.utc) - timedelta(days=days)
    earning_kinds = ("subscription", "ppv", "tip", "ticket", "product")

    rows = await session.execute(
        select(
            LedgerEntry.kind,
            func.count(LedgerEntry.id),
            func.sum(LedgerEntry.gross_cents),
            func.sum(LedgerEntry.fee_cents),
            func.sum(LedgerEntry.net_cents),
        )
        .where(
            LedgerEntry.creator_id == creator.id,
            LedgerEntry.kind.in_(earning_kinds),
            LedgerEntry.created_at >= since,
        )
        .group_by(LedgerEntry.kind)
    )
    by_kind = [
        EarningsLine(
            kind=kind,
            count=int(count or 0),
            gross_cents=int(gross or 0),
            fee_cents=int(fee or 0),
            net_cents=int(net or 0),
        )
        for kind, count, gross, fee, net in rows.all()
    ]
    by_kind.sort(key=lambda line: line.net_cents, reverse=True)

    recent_rows = await session.execute(
        select(LedgerEntry)
        .where(LedgerEntry.creator_id == creator.id, LedgerEntry.kind.in_(earning_kinds))
        .order_by(LedgerEntry.created_at.desc(), LedgerEntry.id.desc())
        .limit(20)
    )
    recent = [
        LedgerOut(
            id=e.id,
            kind=e.kind,
            gross_cents=e.gross_cents,
            fee_cents=e.fee_cents,
            net_cents=e.net_cents,
            note=e.note,
            created_at=e.created_at,
        )
        for e in recent_rows.scalars().all()
    ]

    active_subs = await session.execute(
        select(func.count(Subscription.id)).where(
            Subscription.creator_id == creator.id, Subscription.status == "active"
        )
    )
    followers = await session.execute(
        select(func.count(Follow.id)).where(Follow.creator_id == creator.id)
    )

    return EarningsOut(
        range_days=days,
        gross_cents=sum(line.gross_cents for line in by_kind),
        fee_cents=sum(line.fee_cents for line in by_kind),
        net_cents=sum(line.net_cents for line in by_kind),
        available_cents=user.earnings_balance_cents,
        platform_fee_bps=settings.platform_fee_bps,
        by_kind=by_kind,
        active_subscribers=int(active_subs.scalar_one()),
        followers=int(followers.scalar_one()),
        recent=recent,
    )


@router.post("/me/earnings/payout", response_model=WalletOut)
async def request_payout(
    payload: TopUpRequest, creator: CreatorDep, user: UserDep, session: SessionDep
) -> WalletOut:
    """Records the withdrawal against the balance. No bank transfer is performed."""
    try:
        await payments.payout(session, user, payload.amount_cents)
    except payments.InsufficientFunds as exc:
        raise HTTPException(status.HTTP_402_PAYMENT_REQUIRED, str(exc)) from exc
    except payments.PaymentError as exc:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, str(exc)) from exc
    await session.commit()
    return WalletOut(
        wallet_balance_cents=user.wallet_balance_cents,
        earnings_balance_cents=user.earnings_balance_cents,
    )
