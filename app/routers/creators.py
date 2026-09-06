"""Creator channels: the profile, its membership tiers, and following."""
from __future__ import annotations

import json

from fastapi import APIRouter, HTTPException, status
from sqlalchemy import delete, select

from app.core.deps import CreatorDep, OptionalUserDep, SessionDep, UserDep
from app.models.orm import CreatorProfile, Follow, Tier, User
from app.models.schemas import (
    CreatorCreate,
    CreatorOut,
    CreatorUpdate,
    SubscriptionOut,
    TierCreate,
    TierOut,
)
from app.services import catalog

router = APIRouter(prefix="/api/v1/creators", tags=["creators"])

RESERVED_HANDLES = {
    "admin", "api", "live", "discover", "inbox", "home", "settings", "support",
    "about", "terms", "privacy", "login", "signup", "wallet", "shop", "help",
}


async def build_creator_out(
    session: SessionDep, creator: CreatorProfile, viewer: User | None
) -> CreatorOut:
    owner = await session.get(User, creator.user_id)
    tier_result = await session.execute(
        select(Tier)
        .where(Tier.creator_id == creator.id, Tier.is_active.is_(True))
        .order_by(Tier.price_cents)
    )
    tiers = [catalog.tier_out(t) for t in tier_result.scalars().all()]

    viewer_sub: SubscriptionOut | None = None
    viewer_following: bool | None = None
    if viewer is not None:
        sub = await catalog.active_subscription(session, viewer.id, creator.id)
        if sub is not None:
            tier = await session.get(Tier, sub.tier_id)
            viewer_sub = SubscriptionOut(
                id=sub.id,
                creator_handle=creator.handle,
                creator_display_name=creator.display_name,
                tier_id=sub.tier_id,
                tier_name=tier.name if tier else "",
                price_cents=tier.price_cents if tier else 0,
                status=sub.status,
                auto_renew=sub.auto_renew,
                started_at=sub.started_at,
                current_period_end=sub.current_period_end,
            )
        follow = await session.execute(
            select(Follow).where(Follow.fan_id == viewer.id, Follow.creator_id == creator.id)
        )
        viewer_following = follow.scalar_one_or_none() is not None

    return CreatorOut(
        id=creator.id,
        handle=creator.handle,
        display_name=creator.display_name,
        tagline=creator.tagline,
        bio=creator.bio,
        category=creator.category,
        accent_color=creator.accent_color,
        banner_url=creator.banner_url,
        avatar_url=creator.avatar_url,
        avatar_emoji=owner.avatar_emoji if owner else "🙂",
        is_verified=creator.is_verified,
        tips_enabled=creator.tips_enabled,
        min_tip_cents=creator.min_tip_cents,
        subscriber_count=await catalog.subscriber_count(session, creator.id),
        follower_count=await catalog.follower_count(session, creator.id),
        post_count=await catalog.post_count(session, creator.id),
        tiers=tiers,
        viewer_is_following=viewer_following,
        viewer_subscription=viewer_sub,
        is_live=await catalog.is_live_now(session, creator.id),
    )


@router.post("", response_model=CreatorOut, status_code=status.HTTP_201_CREATED)
async def become_creator(
    payload: CreatorCreate, user: UserDep, session: SessionDep
) -> CreatorOut:
    """Turn an account into a channel. One channel per account."""
    if user.is_creator:
        raise HTTPException(status.HTTP_409_CONFLICT, "This account already has a channel")
    if payload.handle in RESERVED_HANDLES:
        raise HTTPException(status.HTTP_409_CONFLICT, f"@{payload.handle} is reserved")
    taken = await session.execute(
        select(CreatorProfile).where(CreatorProfile.handle == payload.handle)
    )
    if taken.scalar_one_or_none():
        raise HTTPException(status.HTTP_409_CONFLICT, f"@{payload.handle} is taken")

    creator = CreatorProfile(user_id=user.id, **payload.model_dump())
    user.is_creator = True
    session.add(creator)
    await session.commit()
    await session.refresh(creator)
    return await build_creator_out(session, creator, user)


@router.get("/{handle}", response_model=CreatorOut)
async def get_creator(handle: str, session: SessionDep, viewer: OptionalUserDep) -> CreatorOut:
    creator = await catalog.get_creator_by_handle(session, handle)
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No creator @{handle}")
    return await build_creator_out(session, creator, viewer)


@router.patch("/me", response_model=CreatorOut)
async def update_my_channel(
    payload: CreatorUpdate, creator: CreatorDep, user: UserDep, session: SessionDep
) -> CreatorOut:
    for field, value in payload.model_dump(exclude_unset=True).items():
        if value is not None:
            setattr(creator, field, value)
    await session.commit()
    await session.refresh(creator)
    return await build_creator_out(session, creator, user)


@router.post("/me/tiers", response_model=TierOut, status_code=status.HTTP_201_CREATED)
async def create_tier(payload: TierCreate, creator: CreatorDep, session: SessionDep) -> TierOut:
    tier = Tier(
        creator_id=creator.id,
        name=payload.name,
        price_cents=payload.price_cents,
        description=payload.description,
        perks=json.dumps(payload.perks),
        is_vip=payload.is_vip,
    )
    session.add(tier)
    await session.commit()
    await session.refresh(tier)
    return catalog.tier_out(tier)


@router.delete("/me/tiers/{tier_id}", status_code=status.HTTP_204_NO_CONTENT)
async def retire_tier(tier_id: int, creator: CreatorDep, session: SessionDep) -> None:
    """Retiring hides a tier from new subscribers; existing subscriptions keep running."""
    tier = await session.get(Tier, tier_id)
    if tier is None or tier.creator_id != creator.id:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such tier")
    tier.is_active = False
    await session.commit()


@router.post("/{handle}/follow", status_code=status.HTTP_204_NO_CONTENT)
async def follow(handle: str, user: UserDep, session: SessionDep) -> None:
    creator = await catalog.get_creator_by_handle(session, handle)
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No creator @{handle}")
    existing = await session.execute(
        select(Follow).where(Follow.fan_id == user.id, Follow.creator_id == creator.id)
    )
    if existing.scalar_one_or_none() is None:
        session.add(Follow(fan_id=user.id, creator_id=creator.id))
        await session.commit()


@router.delete("/{handle}/follow", status_code=status.HTTP_204_NO_CONTENT)
async def unfollow(handle: str, user: UserDep, session: SessionDep) -> None:
    creator = await catalog.get_creator_by_handle(session, handle)
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No creator @{handle}")
    await session.execute(
        delete(Follow).where(Follow.fan_id == user.id, Follow.creator_id == creator.id)
    )
    await session.commit()
