"""Request and response shapes. Amounts are always integer cents."""
from __future__ import annotations

from datetime import datetime

from pydantic import BaseModel, Field, field_validator

HANDLE_RE = r"^[a-z0-9_]{3,30}$"


# ---------------------------------------------------------------- auth


class RegisterRequest(BaseModel):
    email: str
    password: str = Field(min_length=8, max_length=128)
    display_name: str = Field(min_length=1, max_length=80)
    avatar_emoji: str = Field(default="🙂", max_length=8)


class LoginRequest(BaseModel):
    email: str
    password: str


class TokenResponse(BaseModel):
    access_token: str
    token_type: str = "bearer"
    user: "UserOut"


class UserOut(BaseModel):
    id: int
    email: str
    display_name: str
    avatar_emoji: str
    is_creator: bool
    wallet_balance_cents: int
    earnings_balance_cents: int
    handle: str | None = None


# ---------------------------------------------------------------- creator


class CreatorCreate(BaseModel):
    handle: str = Field(pattern=HANDLE_RE)
    display_name: str = Field(min_length=1, max_length=80)
    tagline: str = Field(default="", max_length=160)
    bio: str = ""
    category: str = "entertainment"
    accent_color: str = "#7c5cff"
    banner_url: str | None = None
    avatar_url: str | None = None

    @field_validator("handle")
    @classmethod
    def _lower(cls, v: str) -> str:
        return v.lower()


class CreatorUpdate(BaseModel):
    display_name: str | None = None
    tagline: str | None = None
    bio: str | None = None
    category: str | None = None
    accent_color: str | None = None
    banner_url: str | None = None
    avatar_url: str | None = None
    tips_enabled: bool | None = None
    min_tip_cents: int | None = Field(default=None, ge=0)
    dm_price_cents: int | None = Field(default=None, ge=0)


class TierOut(BaseModel):
    id: int
    name: str
    price_cents: int
    description: str
    perks: list[str]
    is_vip: bool
    is_active: bool


class TierCreate(BaseModel):
    name: str = Field(min_length=1, max_length=60)
    price_cents: int = Field(ge=0, le=100_000)
    description: str = ""
    perks: list[str] = Field(default_factory=list)
    is_vip: bool = False


class CreatorOut(BaseModel):
    id: int
    handle: str
    display_name: str
    tagline: str
    bio: str
    category: str
    accent_color: str
    banner_url: str | None
    avatar_url: str | None
    avatar_emoji: str
    is_verified: bool
    tips_enabled: bool
    min_tip_cents: int
    subscriber_count: int
    follower_count: int
    post_count: int
    tiers: list[TierOut]
    # Viewer-relative state; null when nobody is logged in.
    viewer_is_following: bool | None = None
    viewer_subscription: "SubscriptionOut | None" = None
    is_live: bool = False


# ---------------------------------------------------------------- posts


class PostCreate(BaseModel):
    kind: str = "photo"
    title: str = Field(default="", max_length=160)
    body: str = ""
    media_url: str | None = None
    preview_url: str | None = None
    duration_seconds: int | None = None
    visibility: str = "public"
    price_cents: int = Field(default=0, ge=0, le=100_000)
    min_tier_id: int | None = None

    @field_validator("visibility")
    @classmethod
    def _known_visibility(cls, v: str) -> str:
        if v not in {"public", "subscribers", "ppv"}:
            raise ValueError("visibility must be public, subscribers, or ppv")
        return v

    @field_validator("kind")
    @classmethod
    def _known_kind(cls, v: str) -> str:
        if v not in {"photo", "video", "audio", "text"}:
            raise ValueError("kind must be photo, video, audio, or text")
        return v


class PostAuthor(BaseModel):
    handle: str
    display_name: str
    avatar_url: str | None = None
    avatar_emoji: str = "🙂"
    is_verified: bool = False


class PostOut(BaseModel):
    id: int
    author: PostAuthor
    kind: str
    title: str
    visibility: str
    price_cents: int
    created_at: datetime
    like_count: int
    comment_count: int
    unlock_count: int
    duration_seconds: int | None = None

    # Locked posts return the teaser only: preview image, no body, no media_url.
    locked: bool
    lock_reason: str | None = None
    unlock_label: str | None = None
    preview_url: str | None = None
    body: str | None = None
    media_url: str | None = None
    viewer_has_liked: bool = False


class CommentCreate(BaseModel):
    body: str = Field(min_length=1, max_length=1000)


class CommentOut(BaseModel):
    id: int
    user_display_name: str
    user_avatar_emoji: str
    body: str
    created_at: datetime


# ---------------------------------------------------------------- money


class SubscribeRequest(BaseModel):
    tier_id: int


class SubscriptionOut(BaseModel):
    id: int
    creator_handle: str
    creator_display_name: str
    tier_id: int
    tier_name: str
    price_cents: int
    status: str
    auto_renew: bool
    started_at: datetime
    current_period_end: datetime


class TipRequest(BaseModel):
    amount_cents: int = Field(ge=1)
    note: str = Field(default="", max_length=200)


class TopUpRequest(BaseModel):
    amount_cents: int = Field(ge=100, le=100_000)


class WalletOut(BaseModel):
    wallet_balance_cents: int
    earnings_balance_cents: int


class PurchaseResult(BaseModel):
    ok: bool
    charged_cents: int
    wallet_balance_cents: int
    detail: str


# ---------------------------------------------------------------- live


class LiveCreate(BaseModel):
    title: str = Field(min_length=1, max_length=160)
    description: str = ""
    cover_url: str | None = None
    access: str = "free"
    ticket_price_cents: int = Field(default=0, ge=0, le=100_000)
    min_tier_id: int | None = None
    scheduled_for: datetime | None = None

    @field_validator("access")
    @classmethod
    def _known_access(cls, v: str) -> str:
        if v not in {"free", "subscribers", "ticket"}:
            raise ValueError("access must be free, subscribers, or ticket")
        return v


class LiveOut(BaseModel):
    id: int
    creator: PostAuthor
    title: str
    description: str
    cover_url: str | None
    access: str
    ticket_price_cents: int
    status: str
    scheduled_for: datetime | None
    started_at: datetime | None
    viewer_count: int
    can_watch: bool
    lock_reason: str | None = None
    playback_url: str | None = None


# ---------------------------------------------------------------- shop


class ProductCreate(BaseModel):
    kind: str = "merch"
    name: str = Field(min_length=1, max_length=120)
    description: str = ""
    image_url: str | None = None
    price_cents: int = Field(ge=0, le=1_000_000)
    inventory: int | None = Field(default=None, ge=0)
    digital_asset_url: str | None = None

    @field_validator("kind")
    @classmethod
    def _known_kind(cls, v: str) -> str:
        if v not in {"merch", "digital"}:
            raise ValueError("kind must be merch or digital")
        return v


class ProductOut(BaseModel):
    id: int
    creator_handle: str
    kind: str
    name: str
    description: str
    image_url: str | None
    price_cents: int
    inventory: int | None
    is_active: bool
    sold_out: bool


class BuyRequest(BaseModel):
    quantity: int = Field(default=1, ge=1, le=20)
    shipping_address: str | None = None


class OrderOut(BaseModel):
    id: int
    product_name: str
    creator_handle: str
    kind: str
    quantity: int
    amount_cents: int
    status: str
    created_at: datetime
    download_url: str | None = None


# ---------------------------------------------------------------- messages


class MessageCreate(BaseModel):
    body: str = Field(min_length=1, max_length=4000)


class MessageOut(BaseModel):
    id: int
    thread_id: int
    sender_id: int
    from_me: bool
    body: str
    created_at: datetime


class ThreadOut(BaseModel):
    id: int
    creator_handle: str
    display_name: str
    avatar_emoji: str
    avatar_url: str | None = None
    last_message: str | None
    last_message_at: datetime
    unread_count: int


# ---------------------------------------------------------------- earnings


class EarningsLine(BaseModel):
    kind: str
    count: int
    gross_cents: int
    fee_cents: int
    net_cents: int


class EarningsOut(BaseModel):
    currency: str = "USD"
    range_days: int
    gross_cents: int
    fee_cents: int
    net_cents: int
    available_cents: int
    platform_fee_bps: int
    by_kind: list[EarningsLine]
    active_subscribers: int
    followers: int
    recent: list["LedgerOut"]


class LedgerOut(BaseModel):
    id: int
    kind: str
    gross_cents: int
    fee_cents: int
    net_cents: int
    note: str
    created_at: datetime


CreatorOut.model_rebuild()
TokenResponse.model_rebuild()
EarningsOut.model_rebuild()
