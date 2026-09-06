"""Database models.

Money is always integer cents. Nothing in this schema stores a float amount.
"""
from __future__ import annotations

from datetime import datetime, timezone

from sqlalchemy import (
    Boolean,
    DateTime,
    ForeignKey,
    Index,
    Integer,
    String,
    Text,
    UniqueConstraint,
)
from sqlalchemy.orm import Mapped, mapped_column, relationship

from app.core.db import Base


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


class TimestampMixin:
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class User(TimestampMixin, Base):
    __tablename__ = "users"

    id: Mapped[int] = mapped_column(primary_key=True)
    email: Mapped[str] = mapped_column(String(255), unique=True, index=True)
    password_hash: Mapped[str] = mapped_column(String(255))
    display_name: Mapped[str] = mapped_column(String(80))
    avatar_emoji: Mapped[str] = mapped_column(String(8), default="🙂")
    is_creator: Mapped[bool] = mapped_column(Boolean, default=False)
    # Set by the control plane. A suspended account can sign in but cannot act.
    is_suspended: Mapped[bool] = mapped_column(Boolean, default=False)

    # Fan-side spendable balance and creator-side earned balance are kept apart so a
    # creator's payout can never be silently spent as fan credit.
    wallet_balance_cents: Mapped[int] = mapped_column(Integer, default=0)
    earnings_balance_cents: Mapped[int] = mapped_column(Integer, default=0)

    # Eager by default: `user.creator` is read on nearly every request, and a lazy
    # load here would be sync IO inside async request handling.
    creator: Mapped["CreatorProfile | None"] = relationship(
        back_populates="user", uselist=False, cascade="all, delete-orphan", lazy="selectin"
    )


class CreatorProfile(TimestampMixin, Base):
    """A creator's channel: the storefront, not the account."""

    __tablename__ = "creator_profiles"

    id: Mapped[int] = mapped_column(primary_key=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"), unique=True)
    handle: Mapped[str] = mapped_column(String(40), unique=True, index=True)
    display_name: Mapped[str] = mapped_column(String(80))
    tagline: Mapped[str] = mapped_column(String(160), default="")
    bio: Mapped[str] = mapped_column(Text, default="")
    category: Mapped[str] = mapped_column(String(40), default="entertainment", index=True)
    accent_color: Mapped[str] = mapped_column(String(9), default="#7c5cff")
    banner_url: Mapped[str | None] = mapped_column(String(500), nullable=True)
    avatar_url: Mapped[str | None] = mapped_column(String(500), nullable=True)

    tips_enabled: Mapped[bool] = mapped_column(Boolean, default=True)
    min_tip_cents: Mapped[int] = mapped_column(Integer, default=100)
    dm_price_cents: Mapped[int] = mapped_column(Integer, default=0)

    # Set by a human operator after ID review. Never inferred by the app.
    is_verified: Mapped[bool] = mapped_column(Boolean, default=False)

    user: Mapped[User] = relationship(back_populates="creator")
    tiers: Mapped[list["Tier"]] = relationship(
        back_populates="creator", cascade="all, delete-orphan"
    )


class Tier(TimestampMixin, Base):
    """A membership level. Ordering is by price: a higher-priced tier unlocks lower ones."""

    __tablename__ = "tiers"

    id: Mapped[int] = mapped_column(primary_key=True)
    creator_id: Mapped[int] = mapped_column(ForeignKey("creator_profiles.id"), index=True)
    name: Mapped[str] = mapped_column(String(60))
    price_cents: Mapped[int] = mapped_column(Integer)
    description: Mapped[str] = mapped_column(Text, default="")
    perks: Mapped[str] = mapped_column(Text, default="[]")  # JSON array of strings
    is_vip: Mapped[bool] = mapped_column(Boolean, default=False)
    is_active: Mapped[bool] = mapped_column(Boolean, default=True)

    creator: Mapped[CreatorProfile] = relationship(back_populates="tiers")


class Follow(TimestampMixin, Base):
    """Free follow. Costs nothing and unlocks nothing but the feed."""

    __tablename__ = "follows"
    __table_args__ = (UniqueConstraint("fan_id", "creator_id", name="uq_follow"),)

    id: Mapped[int] = mapped_column(primary_key=True)
    fan_id: Mapped[int] = mapped_column(ForeignKey("users.id"), index=True)
    creator_id: Mapped[int] = mapped_column(ForeignKey("creator_profiles.id"), index=True)


class Subscription(TimestampMixin, Base):
    __tablename__ = "subscriptions"
    __table_args__ = (Index("ix_sub_fan_creator", "fan_id", "creator_id"),)

    id: Mapped[int] = mapped_column(primary_key=True)
    fan_id: Mapped[int] = mapped_column(ForeignKey("users.id"), index=True)
    creator_id: Mapped[int] = mapped_column(ForeignKey("creator_profiles.id"), index=True)
    tier_id: Mapped[int] = mapped_column(ForeignKey("tiers.id"))
    status: Mapped[str] = mapped_column(String(20), default="active")  # active | canceled
    auto_renew: Mapped[bool] = mapped_column(Boolean, default=True)
    started_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    current_period_end: Mapped[datetime] = mapped_column(DateTime(timezone=True))
    canceled_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)


class Post(TimestampMixin, Base):
    __tablename__ = "posts"

    id: Mapped[int] = mapped_column(primary_key=True)
    creator_id: Mapped[int] = mapped_column(ForeignKey("creator_profiles.id"), index=True)
    kind: Mapped[str] = mapped_column(String(20), default="photo")  # photo|video|audio|text
    title: Mapped[str] = mapped_column(String(160), default="")
    body: Mapped[str] = mapped_column(Text, default="")
    media_url: Mapped[str | None] = mapped_column(String(500), nullable=True)
    preview_url: Mapped[str | None] = mapped_column(String(500), nullable=True)
    duration_seconds: Mapped[int | None] = mapped_column(Integer, nullable=True)

    # public: anyone. subscribers: active sub (at min_tier_id or above). ppv: one-off purchase.
    visibility: Mapped[str] = mapped_column(String(20), default="public", index=True)
    price_cents: Mapped[int] = mapped_column(Integer, default=0)
    min_tier_id: Mapped[int | None] = mapped_column(ForeignKey("tiers.id"), nullable=True)

    like_count: Mapped[int] = mapped_column(Integer, default=0)
    comment_count: Mapped[int] = mapped_column(Integer, default=0)
    unlock_count: Mapped[int] = mapped_column(Integer, default=0)


class PostUnlock(TimestampMixin, Base):
    """One fan's paid access to one pay-per-view post. Permanent once bought."""

    __tablename__ = "post_unlocks"
    __table_args__ = (UniqueConstraint("post_id", "user_id", name="uq_unlock"),)

    id: Mapped[int] = mapped_column(primary_key=True)
    post_id: Mapped[int] = mapped_column(ForeignKey("posts.id"), index=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"), index=True)
    amount_cents: Mapped[int] = mapped_column(Integer, default=0)


class PostLike(TimestampMixin, Base):
    __tablename__ = "post_likes"
    __table_args__ = (UniqueConstraint("post_id", "user_id", name="uq_like"),)

    id: Mapped[int] = mapped_column(primary_key=True)
    post_id: Mapped[int] = mapped_column(ForeignKey("posts.id"), index=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"), index=True)


class Comment(TimestampMixin, Base):
    __tablename__ = "comments"

    id: Mapped[int] = mapped_column(primary_key=True)
    post_id: Mapped[int] = mapped_column(ForeignKey("posts.id"), index=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"))
    body: Mapped[str] = mapped_column(Text)


class LiveEvent(TimestampMixin, Base):
    __tablename__ = "live_events"

    id: Mapped[int] = mapped_column(primary_key=True)
    creator_id: Mapped[int] = mapped_column(ForeignKey("creator_profiles.id"), index=True)
    title: Mapped[str] = mapped_column(String(160))
    description: Mapped[str] = mapped_column(Text, default="")
    cover_url: Mapped[str | None] = mapped_column(String(500), nullable=True)

    # free | subscribers | ticket
    access: Mapped[str] = mapped_column(String(20), default="free")
    ticket_price_cents: Mapped[int] = mapped_column(Integer, default=0)
    min_tier_id: Mapped[int | None] = mapped_column(ForeignKey("tiers.id"), nullable=True)

    status: Mapped[str] = mapped_column(String(20), default="scheduled", index=True)
    scheduled_for: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    started_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    ended_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)

    # Filled in by whatever streaming provider is wired up. Empty until then.
    playback_url: Mapped[str | None] = mapped_column(String(500), nullable=True)
    viewer_count: Mapped[int] = mapped_column(Integer, default=0)


class LiveTicket(TimestampMixin, Base):
    __tablename__ = "live_tickets"
    __table_args__ = (UniqueConstraint("event_id", "user_id", name="uq_ticket"),)

    id: Mapped[int] = mapped_column(primary_key=True)
    event_id: Mapped[int] = mapped_column(ForeignKey("live_events.id"), index=True)
    user_id: Mapped[int] = mapped_column(ForeignKey("users.id"), index=True)
    amount_cents: Mapped[int] = mapped_column(Integer, default=0)


class Product(TimestampMixin, Base):
    """Merch (shippable) or a digital download."""

    __tablename__ = "products"

    id: Mapped[int] = mapped_column(primary_key=True)
    creator_id: Mapped[int] = mapped_column(ForeignKey("creator_profiles.id"), index=True)
    kind: Mapped[str] = mapped_column(String(20), default="merch")  # merch | digital
    name: Mapped[str] = mapped_column(String(120))
    description: Mapped[str] = mapped_column(Text, default="")
    image_url: Mapped[str | None] = mapped_column(String(500), nullable=True)
    price_cents: Mapped[int] = mapped_column(Integer)
    # None means unlimited (always the case for digital goods).
    inventory: Mapped[int | None] = mapped_column(Integer, nullable=True)
    digital_asset_url: Mapped[str | None] = mapped_column(String(500), nullable=True)
    is_active: Mapped[bool] = mapped_column(Boolean, default=True)


class Order(TimestampMixin, Base):
    __tablename__ = "orders"

    id: Mapped[int] = mapped_column(primary_key=True)
    product_id: Mapped[int] = mapped_column(ForeignKey("products.id"), index=True)
    buyer_id: Mapped[int] = mapped_column(ForeignKey("users.id"), index=True)
    creator_id: Mapped[int] = mapped_column(ForeignKey("creator_profiles.id"), index=True)
    quantity: Mapped[int] = mapped_column(Integer, default=1)
    amount_cents: Mapped[int] = mapped_column(Integer)
    status: Mapped[str] = mapped_column(String(20), default="paid")
    shipping_address: Mapped[str | None] = mapped_column(Text, nullable=True)
    download_token: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)


class Thread(TimestampMixin, Base):
    """One fan <-> one creator conversation."""

    __tablename__ = "threads"
    __table_args__ = (UniqueConstraint("creator_id", "fan_id", name="uq_thread"),)

    id: Mapped[int] = mapped_column(primary_key=True)
    creator_id: Mapped[int] = mapped_column(ForeignKey("creator_profiles.id"), index=True)
    fan_id: Mapped[int] = mapped_column(ForeignKey("users.id"), index=True)
    last_message_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)


class Message(TimestampMixin, Base):
    __tablename__ = "messages"

    id: Mapped[int] = mapped_column(primary_key=True)
    thread_id: Mapped[int] = mapped_column(ForeignKey("threads.id"), index=True)
    sender_id: Mapped[int] = mapped_column(ForeignKey("users.id"))
    body: Mapped[str] = mapped_column(Text)
    read_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)


class LedgerEntry(TimestampMixin, Base):
    """Every movement of money. Append-only: corrections are new rows, not edits.

    gross = what the payer paid. fee = platform's cut. net = creator's take.
    Top-ups and payouts have no creator and no fee.
    """

    __tablename__ = "ledger_entries"

    id: Mapped[int] = mapped_column(primary_key=True)
    kind: Mapped[str] = mapped_column(String(20), index=True)
    payer_id: Mapped[int | None] = mapped_column(ForeignKey("users.id"), nullable=True, index=True)
    creator_id: Mapped[int | None] = mapped_column(
        ForeignKey("creator_profiles.id"), nullable=True, index=True
    )
    reference_type: Mapped[str | None] = mapped_column(String(30), nullable=True)
    reference_id: Mapped[int | None] = mapped_column(Integer, nullable=True)
    gross_cents: Mapped[int] = mapped_column(Integer)
    fee_cents: Mapped[int] = mapped_column(Integer, default=0)
    net_cents: Mapped[int] = mapped_column(Integer, default=0)
    note: Mapped[str] = mapped_column(String(200), default="")
