"""Shared read helpers: the queries every router needs and the shapes they return."""
from __future__ import annotations

import json
from datetime import datetime, timezone

from sqlalchemy import func, select
from sqlalchemy.ext.asyncio import AsyncSession

from app.models.orm import (
    CreatorProfile,
    Follow,
    LiveEvent,
    Post,
    PostLike,
    PostUnlock,
    Subscription,
    Tier,
    User,
)
from app.models.schemas import PostAuthor, PostOut, TierOut
from app.services.access import Access, decide_post_access


async def get_creator_by_handle(session: AsyncSession, handle: str) -> CreatorProfile | None:
    result = await session.execute(
        select(CreatorProfile).where(CreatorProfile.handle == handle.lower())
    )
    return result.scalar_one_or_none()


async def active_subscription(
    session: AsyncSession, fan_id: int, creator_id: int
) -> Subscription | None:
    """Active means not canceled and still inside the paid period."""
    result = await session.execute(
        select(Subscription)
        .where(
            Subscription.fan_id == fan_id,
            Subscription.creator_id == creator_id,
            Subscription.status == "active",
        )
        .order_by(Subscription.current_period_end.desc())
    )
    sub = result.scalars().first()
    if sub is None:
        return None
    period_end = sub.current_period_end
    if period_end.tzinfo is None:  # SQLite hands back naive datetimes
        period_end = period_end.replace(tzinfo=timezone.utc)
    return sub if period_end > datetime.now(timezone.utc) else None


async def viewer_tier_price_cents(
    session: AsyncSession, viewer: User | None, creator_id: int
) -> int | None:
    """What the viewer currently pays this creator per month, or None if nothing."""
    if viewer is None:
        return None
    sub = await active_subscription(session, viewer.id, creator_id)
    if sub is None:
        return None
    tier = await session.get(Tier, sub.tier_id)
    return tier.price_cents if tier else 0


async def unlocked_post_ids(session: AsyncSession, viewer: User | None) -> set[int]:
    if viewer is None:
        return set()
    result = await session.execute(select(PostUnlock.post_id).where(PostUnlock.user_id == viewer.id))
    return set(result.scalars().all())


async def liked_post_ids(session: AsyncSession, viewer: User | None) -> set[int]:
    if viewer is None:
        return set()
    result = await session.execute(select(PostLike.post_id).where(PostLike.user_id == viewer.id))
    return set(result.scalars().all())


async def tier_price_map(session: AsyncSession) -> dict[int, int]:
    result = await session.execute(select(Tier.id, Tier.price_cents))
    return {tid: price for tid, price in result.all()}


async def subscriber_count(session: AsyncSession, creator_id: int) -> int:
    result = await session.execute(
        select(func.count(Subscription.id)).where(
            Subscription.creator_id == creator_id, Subscription.status == "active"
        )
    )
    return int(result.scalar_one())


async def follower_count(session: AsyncSession, creator_id: int) -> int:
    result = await session.execute(
        select(func.count(Follow.id)).where(Follow.creator_id == creator_id)
    )
    return int(result.scalar_one())


async def post_count(session: AsyncSession, creator_id: int) -> int:
    result = await session.execute(select(func.count(Post.id)).where(Post.creator_id == creator_id))
    return int(result.scalar_one())


async def is_live_now(session: AsyncSession, creator_id: int) -> bool:
    result = await session.execute(
        select(func.count(LiveEvent.id)).where(
            LiveEvent.creator_id == creator_id, LiveEvent.status == "live"
        )
    )
    return int(result.scalar_one()) > 0


def tier_out(tier: Tier) -> TierOut:
    try:
        perks = json.loads(tier.perks) if tier.perks else []
    except json.JSONDecodeError:
        perks = []
    return TierOut(
        id=tier.id,
        name=tier.name,
        price_cents=tier.price_cents,
        description=tier.description,
        perks=[str(p) for p in perks],
        is_vip=tier.is_vip,
        is_active=tier.is_active,
    )


def author_of(creator: CreatorProfile, user: User | None = None) -> PostAuthor:
    return PostAuthor(
        handle=creator.handle,
        display_name=creator.display_name,
        avatar_url=creator.avatar_url,
        avatar_emoji=user.avatar_emoji if user else "🙂",
        is_verified=creator.is_verified,
    )


def serialize_post(
    post: Post,
    author: PostAuthor,
    access: Access,
    *,
    viewer_has_liked: bool = False,
) -> PostOut:
    """A locked post returns its teaser only — never the body, media, or full text."""
    return PostOut(
        id=post.id,
        author=author,
        kind=post.kind,
        title=post.title,
        visibility=post.visibility,
        price_cents=post.price_cents,
        created_at=post.created_at,
        like_count=post.like_count,
        comment_count=post.comment_count,
        unlock_count=post.unlock_count,
        duration_seconds=post.duration_seconds,
        locked=not access.granted,
        lock_reason=None if access.granted else access.reason,
        unlock_label=access.unlock_label,
        preview_url=post.preview_url,
        body=post.body if access.granted else None,
        media_url=post.media_url if access.granted else None,
        viewer_has_liked=viewer_has_liked,
    )


async def serialize_posts(
    session: AsyncSession,
    posts: list[Post],
    creators: dict[int, CreatorProfile],
    creator_users: dict[int, User],
    viewer: User | None,
) -> list[PostOut]:
    """Batch version: one query each for tiers, unlocks, and likes, not one per post."""
    unlocked = await unlocked_post_ids(session, viewer)
    liked = await liked_post_ids(session, viewer)
    prices = await tier_price_map(session)

    viewer_tiers: dict[int, int | None] = {}
    for creator_id in {p.creator_id for p in posts}:
        viewer_tiers[creator_id] = await viewer_tier_price_cents(session, viewer, creator_id)

    out: list[PostOut] = []
    for post in posts:
        creator = creators[post.creator_id]
        access = decide_post_access(
            is_owner=viewer is not None and viewer.id == creator.user_id,
            visibility=post.visibility,
            price_cents=post.price_cents,
            min_tier_price_cents=prices.get(post.min_tier_id) if post.min_tier_id else None,
            viewer_tier_price_cents=viewer_tiers.get(post.creator_id),
            has_unlock=post.id in unlocked,
        )
        author = author_of(creator, creator_users.get(creator.user_id))
        out.append(serialize_post(post, author, access, viewer_has_liked=post.id in liked))
    return out


async def load_creator_context(
    session: AsyncSession, creator_ids: set[int]
) -> tuple[dict[int, CreatorProfile], dict[int, User]]:
    if not creator_ids:
        return {}, {}
    result = await session.execute(
        select(CreatorProfile).where(CreatorProfile.id.in_(creator_ids))
    )
    creators = {c.id: c for c in result.scalars().all()}
    user_result = await session.execute(
        select(User).where(User.id.in_({c.user_id for c in creators.values()}))
    )
    users = {u.id: u for u in user_result.scalars().all()}
    return creators, users
