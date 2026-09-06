"""LIVE: scheduling, going live, ticketing, and who may watch."""
from __future__ import annotations

from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, Query, status
from sqlalchemy import select

from app.core.deps import CreatorDep, OptionalUserDep, SessionDep, UserDep
from app.models.orm import CreatorProfile, LiveEvent, LiveTicket, Tier, User
from app.models.schemas import LiveCreate, LiveOut, PurchaseResult
from app.services import catalog, payments
from app.services.access import decide_live_access, money

router = APIRouter(prefix="/api/v1/live", tags=["live"])


async def _serialize(
    session: SessionDep, event: LiveEvent, viewer: User | None
) -> LiveOut:
    creator = await session.get(CreatorProfile, event.creator_id)
    owner = await session.get(User, creator.user_id)

    has_ticket = False
    if viewer is not None:
        ticket = await session.execute(
            select(LiveTicket).where(
                LiveTicket.event_id == event.id, LiveTicket.user_id == viewer.id
            )
        )
        has_ticket = ticket.scalar_one_or_none() is not None

    min_tier_price = None
    if event.min_tier_id:
        tier = await session.get(Tier, event.min_tier_id)
        min_tier_price = tier.price_cents if tier else None

    access = decide_live_access(
        is_owner=viewer is not None and viewer.id == creator.user_id,
        access=event.access,
        ticket_price_cents=event.ticket_price_cents,
        min_tier_price_cents=min_tier_price,
        viewer_tier_price_cents=await catalog.viewer_tier_price_cents(session, viewer, creator.id),
        has_ticket=has_ticket,
        is_adult=creator.is_adult_channel,
        viewer_is_verified_adult=viewer is not None and viewer.age_check_status == "verified",
    )
    return LiveOut(
        id=event.id,
        creator=catalog.author_of(creator, owner),
        title=event.title,
        description=event.description,
        cover_url=event.cover_url,
        access=event.access,
        ticket_price_cents=event.ticket_price_cents,
        status=event.status,
        scheduled_for=event.scheduled_for,
        started_at=event.started_at,
        viewer_count=event.viewer_count,
        can_watch=access.granted,
        lock_reason=None if access.granted else access.unlock_label or access.reason,
        # The stream URL is only ever handed to someone who may watch.
        playback_url=event.playback_url if access.granted else None,
    )


@router.post("", response_model=LiveOut, status_code=status.HTTP_201_CREATED)
async def schedule_live(
    payload: LiveCreate, creator: CreatorDep, user: UserDep, session: SessionDep
) -> LiveOut:
    if payload.access == "ticket" and payload.ticket_price_cents <= 0:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "A ticketed live needs a price")

    event = LiveEvent(creator_id=creator.id, **payload.model_dump())
    session.add(event)
    await session.commit()
    await session.refresh(event)
    return await _serialize(session, event, user)


@router.get("", response_model=list[LiveOut])
async def list_live(
    session: SessionDep,
    viewer: OptionalUserDep,
    status_filter: str = Query("live", pattern="^(live|scheduled|ended|all)$", alias="status"),
    limit: int = Query(20, ge=1, le=50),
) -> list[LiveOut]:
    """Default view is the LIVE NOW rail: what's streaming this second."""
    query = select(LiveEvent).order_by(LiveEvent.started_at.desc().nulls_last(), LiveEvent.id.desc())
    if status_filter != "all":
        query = query.where(LiveEvent.status == status_filter)
    if status_filter == "scheduled":
        query = query.order_by(None).order_by(LiveEvent.scheduled_for.asc().nulls_last())

    result = await session.execute(query.limit(limit))
    return [await _serialize(session, e, viewer) for e in result.scalars().all()]


@router.get("/{event_id}", response_model=LiveOut)
async def get_live(event_id: int, session: SessionDep, viewer: OptionalUserDep) -> LiveOut:
    event = await session.get(LiveEvent, event_id)
    if event is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such live event")
    return await _serialize(session, event, viewer)


async def _own_event(session: SessionDep, event_id: int, creator: CreatorProfile) -> LiveEvent:
    event = await session.get(LiveEvent, event_id)
    if event is None or event.creator_id != creator.id:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such live event")
    return event


@router.post("/{event_id}/start", response_model=LiveOut)
async def start_live(
    event_id: int, creator: CreatorDep, user: UserDep, session: SessionDep
) -> LiveOut:
    """Flips the event live.

    `playback_url` stays empty until a streaming provider is wired up — this app
    does not ingest or serve video itself.
    """
    event = await _own_event(session, event_id, creator)
    if event.status == "ended":
        raise HTTPException(status.HTTP_409_CONFLICT, "That event has already ended")
    event.status = "live"
    event.started_at = event.started_at or datetime.now(timezone.utc)
    await session.commit()
    return await _serialize(session, event, user)


@router.post("/{event_id}/end", response_model=LiveOut)
async def end_live(
    event_id: int, creator: CreatorDep, user: UserDep, session: SessionDep
) -> LiveOut:
    event = await _own_event(session, event_id, creator)
    event.status = "ended"
    event.ended_at = datetime.now(timezone.utc)
    event.viewer_count = 0
    await session.commit()
    return await _serialize(session, event, user)


@router.post("/{event_id}/ticket", response_model=PurchaseResult)
async def buy_ticket(event_id: int, user: UserDep, session: SessionDep) -> PurchaseResult:
    event = await session.get(LiveEvent, event_id)
    if event is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such live event")
    if event.access != "ticket":
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "This event does not sell tickets")
    if event.status == "ended":
        raise HTTPException(status.HTTP_409_CONFLICT, "That event has already ended")

    creator = await session.get(CreatorProfile, event.creator_id)
    if creator.user_id == user.id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "It's your event")

    existing = await session.execute(
        select(LiveTicket).where(LiveTicket.event_id == event.id, LiveTicket.user_id == user.id)
    )
    if existing.scalar_one_or_none():
        return PurchaseResult(
            ok=True,
            charged_cents=0,
            wallet_balance_cents=user.wallet_balance_cents,
            detail="You already have a ticket",
        )

    creator_user = await session.get(User, creator.user_id)
    try:
        await payments.charge(
            session,
            payer=user,
            creator=creator,
            creator_user=creator_user,
            amount_cents=event.ticket_price_cents,
            kind="ticket",
            reference_type="live_event",
            reference_id=event.id,
            note=event.title[:200],
        )
    except payments.InsufficientFunds as exc:
        raise HTTPException(status.HTTP_402_PAYMENT_REQUIRED, str(exc)) from exc

    session.add(
        LiveTicket(event_id=event.id, user_id=user.id, amount_cents=event.ticket_price_cents)
    )
    await session.commit()
    return PurchaseResult(
        ok=True,
        charged_cents=event.ticket_price_cents,
        wallet_balance_cents=user.wallet_balance_cents,
        detail=f"Ticket bought — {money(event.ticket_price_cents)}",
    )
