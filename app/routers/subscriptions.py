"""Memberships: subscribe, cancel, and see what you pay for."""
from __future__ import annotations

from datetime import datetime, timedelta, timezone

from fastapi import APIRouter, HTTPException, status
from sqlalchemy import select

from app.core.deps import SessionDep, UserDep
from app.models.orm import CreatorProfile, Subscription, Tier, User
from app.models.schemas import SubscribeRequest, SubscriptionOut
from app.services import catalog, payments

router = APIRouter(prefix="/api/v1", tags=["subscriptions"])

# One "month" of access. Real billing would use a calendar-aware scheduler.
PERIOD = timedelta(days=30)


def _sub_out(sub: Subscription, creator: CreatorProfile, tier: Tier) -> SubscriptionOut:
    return SubscriptionOut(
        id=sub.id,
        creator_handle=creator.handle,
        creator_display_name=creator.display_name,
        tier_id=tier.id,
        tier_name=tier.name,
        price_cents=tier.price_cents,
        status=sub.status,
        auto_renew=sub.auto_renew,
        started_at=sub.started_at,
        current_period_end=sub.current_period_end,
    )


@router.post("/creators/{handle}/subscribe", response_model=SubscriptionOut, status_code=201)
async def subscribe(
    handle: str, payload: SubscribeRequest, user: UserDep, session: SessionDep
) -> SubscriptionOut:
    creator = await catalog.get_creator_by_handle(session, handle)
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No creator @{handle}")
    if creator.user_id == user.id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "You can't subscribe to yourself")

    tier = await session.get(Tier, payload.tier_id)
    if tier is None or tier.creator_id != creator.id or not tier.is_active:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "That tier isn't available")

    existing = await catalog.active_subscription(session, user.id, creator.id)
    if existing is not None:
        if existing.tier_id == tier.id:
            raise HTTPException(
                status.HTTP_409_CONFLICT, f"You're already on {tier.name} for @{handle}"
            )
        # Switching tiers: charge the new tier and restart the period. No proration.
        existing.status = "canceled"
        existing.canceled_at = datetime.now(timezone.utc)

    creator_user = await session.get(User, creator.user_id)
    try:
        await payments.charge(
            session,
            payer=user,
            creator=creator,
            creator_user=creator_user,
            amount_cents=tier.price_cents,
            kind="subscription",
            reference_type="tier",
            reference_id=tier.id,
            note=f"{tier.name} — @{creator.handle}",
        )
    except payments.InsufficientFunds as exc:
        raise HTTPException(status.HTTP_402_PAYMENT_REQUIRED, str(exc)) from exc

    sub = Subscription(
        fan_id=user.id,
        creator_id=creator.id,
        tier_id=tier.id,
        current_period_end=datetime.now(timezone.utc) + PERIOD,
    )
    session.add(sub)
    await session.commit()
    await session.refresh(sub)
    return _sub_out(sub, creator, tier)


@router.get("/me/subscriptions", response_model=list[SubscriptionOut])
async def my_subscriptions(user: UserDep, session: SessionDep) -> list[SubscriptionOut]:
    result = await session.execute(
        select(Subscription, CreatorProfile, Tier)
        .join(CreatorProfile, CreatorProfile.id == Subscription.creator_id)
        .join(Tier, Tier.id == Subscription.tier_id)
        .where(Subscription.fan_id == user.id)
        .order_by(Subscription.created_at.desc())
    )
    return [_sub_out(s, c, t) for s, c, t in result.all()]


@router.delete("/me/subscriptions/{subscription_id}", response_model=SubscriptionOut)
async def cancel_subscription(
    subscription_id: int, user: UserDep, session: SessionDep
) -> SubscriptionOut:
    """Cancels renewal. Access runs to the end of the period already paid for."""
    sub = await session.get(Subscription, subscription_id)
    if sub is None or sub.fan_id != user.id:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such subscription")
    sub.auto_renew = False
    sub.canceled_at = datetime.now(timezone.utc)
    await session.commit()

    creator = await session.get(CreatorProfile, sub.creator_id)
    tier = await session.get(Tier, sub.tier_id)
    return _sub_out(sub, creator, tier)
