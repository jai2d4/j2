"""Sign-up, sign-in, and the current account."""
from __future__ import annotations

from fastapi import APIRouter, HTTPException, status
from sqlalchemy import select

from app.core.deps import SessionDep, UserDep
from app.core.security import create_access_token, hash_password, verify_password
from app.models.orm import User
from app.models.schemas import LoginRequest, RegisterRequest, TokenResponse, UserOut

router = APIRouter(prefix="/api/v1/auth", tags=["auth"])


def user_out(user: User) -> UserOut:
    return UserOut(
        id=user.id,
        email=user.email,
        display_name=user.display_name,
        avatar_emoji=user.avatar_emoji,
        is_creator=user.is_creator,
        wallet_balance_cents=user.wallet_balance_cents,
        earnings_balance_cents=user.earnings_balance_cents,
        handle=user.creator.handle if user.creator else None,
        age_check_status=user.age_check_status,
    )


@router.post("/register", response_model=TokenResponse, status_code=status.HTTP_201_CREATED)
async def register(payload: RegisterRequest, session: SessionDep) -> TokenResponse:
    email = payload.email.strip().lower()
    if "@" not in email or "." not in email.split("@")[-1]:
        raise HTTPException(status.HTTP_422_UNPROCESSABLE_ENTITY, "That email address looks wrong")

    existing = await session.execute(select(User).where(User.email == email))
    if existing.scalar_one_or_none():
        raise HTTPException(status.HTTP_409_CONFLICT, "That email is already registered")

    user = User(
        email=email,
        password_hash=hash_password(payload.password),
        display_name=payload.display_name.strip(),
        avatar_emoji=payload.avatar_emoji or "🙂",
    )
    session.add(user)
    await session.commit()
    await session.refresh(user, ["creator"])
    return TokenResponse(access_token=create_access_token(user.id), user=user_out(user))


@router.post("/login", response_model=TokenResponse)
async def login(payload: LoginRequest, session: SessionDep) -> TokenResponse:
    result = await session.execute(select(User).where(User.email == payload.email.strip().lower()))
    user = result.scalar_one_or_none()
    # Same message either way, so the endpoint does not confirm which emails exist.
    if user is None or not verify_password(payload.password, user.password_hash):
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "Email or password is incorrect")
    await session.refresh(user, ["creator"])
    return TokenResponse(access_token=create_access_token(user.id), user=user_out(user))


@router.get("/me", response_model=UserOut)
async def me(user: UserDep) -> UserOut:
    return user_out(user)
