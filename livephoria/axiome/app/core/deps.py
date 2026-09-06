"""Who is calling: an operator at the dashboard, or an app reporting in."""
from __future__ import annotations

import hmac
from typing import Annotated

from fastapi import Depends, Header, HTTPException, status
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.config import Settings, get_settings
from app.core.db import get_session
from app.core.security import key_matches
from app.models.orm import App

SessionDep = Annotated[AsyncSession, Depends(get_session)]
SettingsDep = Annotated[Settings, Depends(get_settings)]


async def require_admin(
    settings: SettingsDep,
    x_admin_key: Annotated[str | None, Header()] = None,
) -> None:
    expected = settings.resolved_admin_key()
    if not x_admin_key or not hmac.compare_digest(x_admin_key, expected):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Bad admin key")


AdminAuth = Depends(require_admin)


def bearer(authorization: str | None) -> str | None:
    if not authorization:
        return None
    scheme, _, token = authorization.partition(" ")
    return token if scheme.lower() == "bearer" and token else None


async def app_from_key(
    session: AsyncSession, slug: str, authorization: str | None
) -> App | None:
    """Match a reporting app to its issued key. Wrong key means no match, not a hint."""
    token = bearer(authorization)
    if not token:
        return None
    result = await session.execute(select(App).where(App.slug == slug.lower()))
    app = result.scalar_one_or_none()
    if app is None or not key_matches(token, app.app_key_hash):
        return None
    return app
