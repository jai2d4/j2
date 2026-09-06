"""Direct messages between a fan and a creator."""
from __future__ import annotations

from datetime import datetime, timezone

from fastapi import APIRouter, HTTPException, status
from sqlalchemy import func, or_, select

from app.core.deps import SessionDep, UserDep
from app.models.orm import CreatorProfile, Message, Thread, User
from app.models.schemas import MessageCreate, MessageOut, ThreadOut

router = APIRouter(prefix="/api/v1", tags=["messages"])


def _msg_out(msg: Message, viewer_id: int) -> MessageOut:
    return MessageOut(
        id=msg.id,
        thread_id=msg.thread_id,
        sender_id=msg.sender_id,
        from_me=msg.sender_id == viewer_id,
        body=msg.body,
        created_at=msg.created_at,
    )


async def _thread_for(session: SessionDep, creator: CreatorProfile, fan_id: int) -> Thread:
    result = await session.execute(
        select(Thread).where(Thread.creator_id == creator.id, Thread.fan_id == fan_id)
    )
    thread = result.scalar_one_or_none()
    if thread is None:
        thread = Thread(creator_id=creator.id, fan_id=fan_id)
        session.add(thread)
        await session.flush()
    return thread


@router.get("/inbox", response_model=list[ThreadOut])
async def inbox(user: UserDep, session: SessionDep) -> list[ThreadOut]:
    """Both sides of the app read the same inbox: your threads as a fan and as a creator."""
    mine = Thread.fan_id == user.id
    if user.creator is not None:
        mine = or_(mine, Thread.creator_id == user.creator.id)

    result = await session.execute(
        select(Thread).where(mine).order_by(Thread.last_message_at.desc())
    )
    threads = list(result.scalars().all())

    out: list[ThreadOut] = []
    for thread in threads:
        creator = await session.get(CreatorProfile, thread.creator_id)
        viewer_is_creator = user.creator is not None and thread.creator_id == user.creator.id
        if viewer_is_creator:
            other = await session.get(User, thread.fan_id)
            title, emoji, avatar = other.display_name, other.avatar_emoji, None
        else:
            owner = await session.get(User, creator.user_id)
            title, emoji, avatar = creator.display_name, owner.avatar_emoji, creator.avatar_url

        last = await session.execute(
            select(Message)
            .where(Message.thread_id == thread.id)
            .order_by(Message.created_at.desc())
            .limit(1)
        )
        last_msg = last.scalar_one_or_none()
        unread = await session.execute(
            select(func.count(Message.id)).where(
                Message.thread_id == thread.id,
                Message.sender_id != user.id,
                Message.read_at.is_(None),
            )
        )
        out.append(
            ThreadOut(
                id=thread.id,
                creator_handle=creator.handle,
                display_name=title,
                avatar_emoji=emoji,
                avatar_url=avatar,
                last_message=last_msg.body if last_msg else None,
                last_message_at=thread.last_message_at,
                unread_count=int(unread.scalar_one()),
            )
        )
    return out


@router.get("/threads/{thread_id}", response_model=list[MessageOut])
async def read_thread(thread_id: int, user: UserDep, session: SessionDep) -> list[MessageOut]:
    thread = await session.get(Thread, thread_id)
    if thread is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such conversation")
    is_participant = thread.fan_id == user.id or (
        user.creator is not None and thread.creator_id == user.creator.id
    )
    if not is_participant:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "Not your conversation")

    result = await session.execute(
        select(Message).where(Message.thread_id == thread.id).order_by(Message.created_at)
    )
    messages = list(result.scalars().all())
    now = datetime.now(timezone.utc)
    for msg in messages:
        if msg.sender_id != user.id and msg.read_at is None:
            msg.read_at = now
    await session.commit()
    return [_msg_out(m, user.id) for m in messages]


@router.post("/creators/{handle}/messages", response_model=MessageOut, status_code=201)
async def send_to_creator(
    handle: str, payload: MessageCreate, user: UserDep, session: SessionDep
) -> MessageOut:
    result = await session.execute(
        select(CreatorProfile).where(CreatorProfile.handle == handle.lower())
    )
    creator = result.scalar_one_or_none()
    if creator is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, f"No creator @{handle}")

    # A creator replying in their own thread sends as the creator side.
    if user.creator is not None and user.creator.id == creator.id:
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST, "Reply from your inbox, not your own channel"
        )

    thread = await _thread_for(session, creator, user.id)
    message = Message(thread_id=thread.id, sender_id=user.id, body=payload.body)
    thread.last_message_at = datetime.now(timezone.utc)
    session.add(message)
    await session.commit()
    await session.refresh(message)
    return _msg_out(message, user.id)


@router.post("/threads/{thread_id}/messages", response_model=MessageOut, status_code=201)
async def reply(
    thread_id: int, payload: MessageCreate, user: UserDep, session: SessionDep
) -> MessageOut:
    thread = await session.get(Thread, thread_id)
    if thread is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "No such conversation")
    is_participant = thread.fan_id == user.id or (
        user.creator is not None and thread.creator_id == user.creator.id
    )
    if not is_participant:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "Not your conversation")

    message = Message(thread_id=thread.id, sender_id=user.id, body=payload.body)
    thread.last_message_at = datetime.now(timezone.utc)
    session.add(message)
    await session.commit()
    await session.refresh(message)
    return _msg_out(message, user.id)
