"""Discover: search, categories, and who is worth a look right now."""
from __future__ import annotations

from datetime import datetime, timedelta, timezone

from fastapi import APIRouter, Query
from sqlalchemy import func, or_, select

from app.core.deps import OptionalUserDep, SessionDep
from app.models.orm import CreatorProfile, LedgerEntry, LiveEvent, Post, Subscription, User
from app.models.schemas import CreatorOut
from app.routers.creators import build_creator_out

router = APIRouter(prefix="/api/v1/discover", tags=["discover"])


@router.get("", response_model=list[CreatorOut])
async def discover(
    session: SessionDep,
    viewer: OptionalUserDep,
    q: str | None = Query(None, max_length=80),
    category: str | None = Query(None, max_length=40),
    sort: str = Query("trending", pattern="^(trending|new|subscribers)$"),
    limit: int = Query(20, ge=1, le=50),
) -> list[CreatorOut]:
    """`trending` ranks by money and new subscribers over the last week.

    It is a straightforward ranking over this app's own data, not a personalized model.
    """
    query = select(CreatorProfile)
    if q:
        needle = f"%{q.lower()}%"
        query = query.where(
            or_(
                func.lower(CreatorProfile.handle).like(needle),
                func.lower(CreatorProfile.display_name).like(needle),
                func.lower(CreatorProfile.tagline).like(needle),
            )
        )
    if category:
        query = query.where(CreatorProfile.category == category.lower())

    result = await session.execute(query.limit(200))
    creators = list(result.scalars().all())

    if sort == "new":
        creators.sort(key=lambda c: c.created_at, reverse=True)
    else:
        since = datetime.now(timezone.utc) - timedelta(days=7)
        revenue = dict(
            (
                await session.execute(
                    select(LedgerEntry.creator_id, func.sum(LedgerEntry.gross_cents))
                    .where(LedgerEntry.created_at >= since, LedgerEntry.creator_id.isnot(None))
                    .group_by(LedgerEntry.creator_id)
                )
            ).all()
        )
        subs = dict(
            (
                await session.execute(
                    select(Subscription.creator_id, func.count(Subscription.id))
                    .where(Subscription.status == "active")
                    .group_by(Subscription.creator_id)
                )
            ).all()
        )
        if sort == "subscribers":
            creators.sort(key=lambda c: subs.get(c.id, 0), reverse=True)
        else:
            # A subscriber is worth more than a one-off dollar, so weight it.
            creators.sort(
                key=lambda c: (revenue.get(c.id) or 0) + subs.get(c.id, 0) * 500, reverse=True
            )

    return [await build_creator_out(session, c, viewer) for c in creators[:limit]]


@router.get("/categories")
async def categories(session: SessionDep) -> list[dict]:
    result = await session.execute(
        select(CreatorProfile.category, func.count(CreatorProfile.id))
        .group_by(CreatorProfile.category)
        .order_by(func.count(CreatorProfile.id).desc())
    )
    return [{"category": cat, "creators": int(count)} for cat, count in result.all()]


@router.get("/stats")
async def stats(session: SessionDep) -> dict:
    """Counts straight out of the database — nothing estimated."""

    async def count(model) -> int:
        return int((await session.execute(select(func.count(model.id)))).scalar_one())

    live = await session.execute(
        select(func.count(LiveEvent.id)).where(LiveEvent.status == "live")
    )
    return {
        "creators": await count(CreatorProfile),
        "fans": await count(User),
        "posts": await count(Post),
        "live_now": int(live.scalar_one()),
    }
