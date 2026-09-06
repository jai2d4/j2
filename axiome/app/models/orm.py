"""The registry: what apps exist, how to reach them, and what they last said."""
from __future__ import annotations

from datetime import datetime, timezone

from sqlalchemy import Boolean, DateTime, Float, ForeignKey, Integer, String, Text
from sqlalchemy.orm import Mapped, mapped_column

from app.core.db import Base


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


class App(Base):
    """One application under Axiome's control."""

    __tablename__ = "apps"

    id: Mapped[int] = mapped_column(primary_key=True)
    slug: Mapped[str] = mapped_column(String(60), unique=True, index=True)
    name: Mapped[str] = mapped_column(String(120))
    kind: Mapped[str] = mapped_column(String(60), default="app")
    version: Mapped[str] = mapped_column(String(40), default="")

    # Where Axiome reaches the app, and the path its control surface lives under.
    base_url: Mapped[str] = mapped_column(String(400), default="")
    control_path: Mapped[str] = mapped_column(String(200), default="/api/v1/control")
    public_url: Mapped[str] = mapped_column(String(400), default="")

    # Credential Axiome presents to the app (X-Axiome-Key). Stored as given,
    # because Axiome has to replay it. In production this belongs in a secret
    # manager or an encrypted column, not in plain SQL.
    control_key: Mapped[str] = mapped_column(String(200), default="")

    # Credential the app presents to Axiome. Only the hash is kept — the plain
    # key is shown once, when it is issued.
    app_key_hash: Mapped[str] = mapped_column(String(64), default="", index=True)
    app_key_hint: Mapped[str] = mapped_column(String(12), default="")

    capabilities: Mapped[str] = mapped_column(Text, default="[]")  # JSON array

    # ok | degraded | unreachable | unknown
    status: Mapped[str] = mapped_column(String(20), default="unknown", index=True)
    status_detail: Mapped[str] = mapped_column(String(300), default="")
    maintenance: Mapped[bool] = mapped_column(Boolean, default=False)
    last_metrics: Mapped[str] = mapped_column(Text, default="{}")  # JSON object
    last_seen_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True), nullable=True)
    last_latency_ms: Mapped[float | None] = mapped_column(Float, nullable=True)

    enabled: Mapped[bool] = mapped_column(Boolean, default=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)

    @property
    def control_url(self) -> str:
        return self.base_url.rstrip("/") + "/" + self.control_path.strip("/")


class Event(Base):
    """Something an app told Axiome about, or something Axiome did to an app."""

    __tablename__ = "events"

    id: Mapped[int] = mapped_column(primary_key=True)
    app_id: Mapped[int | None] = mapped_column(ForeignKey("apps.id"), nullable=True, index=True)
    app_slug: Mapped[str] = mapped_column(String(60), index=True, default="")
    event: Mapped[str] = mapped_column(String(80), index=True)
    data: Mapped[str] = mapped_column(Text, default="{}")
    # inbound (the app told us) | outbound (we did it)
    direction: Mapped[str] = mapped_column(String(10), default="inbound")
    at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, index=True)


class Sample(Base):
    """A point of heartbeat history, so a dashboard can show a trend, not just a number."""

    __tablename__ = "samples"

    id: Mapped[int] = mapped_column(primary_key=True)
    app_id: Mapped[int] = mapped_column(ForeignKey("apps.id"), index=True)
    at: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, index=True)
    status: Mapped[str] = mapped_column(String(20), default="ok")
    latency_ms: Mapped[float | None] = mapped_column(Float, nullable=True)
    metrics: Mapped[str] = mapped_column(Text, default="{}")
    source: Mapped[str] = mapped_column(String(10), default="push")  # push | poll
