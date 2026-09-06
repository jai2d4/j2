"""Request dependencies: who is calling, and are they allowed to."""
from __future__ import annotations

from typing import Annotated

from fastapi import Depends, Header, HTTPException, status
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession
from sqlalchemy.orm import selectinload

from app.core.config import Settings, get_settings
from app.core.db import get_session
from app.core.security import decode_access_token
from app.models.orm import CreatorProfile, User

SessionDep = Annotated[AsyncSession, Depends(get_session)]
SettingsDep = Annotated[Settings, Depends(get_settings)]


def _bearer(authorization: str | None) -> str | None:
    if not authorization:
        return None
    scheme, _, token = authorization.partition(" ")
    if scheme.lower() != "bearer" or not token:
        return None
    return token


async def _load_user(session: AsyncSession, user_id: int) -> User | None:
    result = await session.execute(
        select(User).options(selectinload(User.creator)).where(User.id == user_id)
    )
    return result.scalar_one_or_none()


async def optional_user(
    session: SessionDep,
    authorization: Annotated[str | None, Header()] = None,
) -> User | None:
    """Logged-out browsing is a first-class case: public feeds work with no token."""
    token = _bearer(authorization)
    if not token:
        return None
    user_id = decode_access_token(token)
    if user_id is None:
        return None
    return await _load_user(session, user_id)


async def current_user(
    session: SessionDep,
    authorization: Annotated[str | None, Header()] = None,
) -> User:
    token = _bearer(authorization)
    if not token:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Sign in to continue")
    user_id = decode_access_token(token)
    if user_id is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Session expired — sign in again")
    user = await _load_user(session, user_id)
    if user is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Account no longer exists")
    if user.is_suspended:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "This account is suspended")
    return user


async def current_creator(
    user: Annotated[User, Depends(current_user)],
) -> CreatorProfile:
    if not user.is_creator or user.creator is None:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "Set up your creator channel first")
    return user.creator


UserDep = Annotated[User, Depends(current_user)]
OptionalUserDep = Annotated[User | None, Depends(optional_user)]
CreatorDep = Annotated[CreatorProfile, Depends(current_creator)]
