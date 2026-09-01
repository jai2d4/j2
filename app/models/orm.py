"""SQLAlchemy ORM models — mirror db/init_schema.sql exactly (Module 5)."""
from __future__ import annotations

import uuid
from datetime import datetime

from sqlalchemy import (
    ARRAY, Boolean, CheckConstraint, DateTime, ForeignKey, Numeric, String, Text, func,
)
from sqlalchemy.dialects.postgresql import JSONB, UUID
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column, relationship


class Base(DeclarativeBase):
    pass


class Athlete(Base):
    __tablename__ = "athletes"

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, server_default=func.uuid_generate_v4())
    first_name: Mapped[str] = mapped_column(String(64), nullable=False)
    last_name: Mapped[str] = mapped_column(String(64), nullable=False)
    grad_year: Mapped[int | None]
    school: Mapped[str | None] = mapped_column(String(128))
    state: Mapped[str | None] = mapped_column(String(2))
    position: Mapped[str] = mapped_column(String(4), nullable=False)
    height_in: Mapped[float | None] = mapped_column(Numeric(4, 1))
    weight_lbs: Mapped[int | None]
    forty_s: Mapped[float | None] = mapped_column(Numeric(3, 2))
    shuttle_s: Mapped[float | None] = mapped_column(Numeric(3, 2))
    bench_lbs: Mapped[int | None]
    squat_lbs: Mapped[int | None]
    gpa: Mapped[float | None] = mapped_column(Numeric(3, 2))
    sat: Mapped[int | None]
    act: Mapped[int | None]
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())
    updated_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), server_default=func.now(), onupdate=func.now(),
    )

    evaluations: Mapped[list["Evaluation"]] = relationship(back_populates="athlete", cascade="all, delete-orphan")


class FilmUpload(Base):
    __tablename__ = "film_uploads"

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, server_default=func.uuid_generate_v4())
    athlete_id: Mapped[uuid.UUID | None] = mapped_column(ForeignKey("athletes.id", ondelete="CASCADE"))
    filename: Mapped[str] = mapped_column(String(255), nullable=False)
    gemini_file_id: Mapped[str | None] = mapped_column(String(255))
    mime_type: Mapped[str | None] = mapped_column(String(32))
    duration_s: Mapped[float | None] = mapped_column(Numeric(8, 2))
    status: Mapped[str] = mapped_column(
        String(16), nullable=False, default="pending",
        server_default="pending",
    )
    uploaded_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())

    __table_args__ = (
        CheckConstraint("status IN ('pending','processing','analyzed','failed')", name="film_uploads_status_check"),
    )


class Evaluation(Base):
    __tablename__ = "evaluations"

    id: Mapped[uuid.UUID] = mapped_column(UUID(as_uuid=True), primary_key=True, server_default=func.uuid_generate_v4())
    athlete_id: Mapped[uuid.UUID] = mapped_column(ForeignKey("athletes.id", ondelete="CASCADE"), nullable=False)
    film_id: Mapped[uuid.UUID | None] = mapped_column(ForeignKey("film_uploads.id", ondelete="SET NULL"))
    position_evaluated: Mapped[str] = mapped_column(String(4), nullable=False)
    projected_tier: Mapped[str | None] = mapped_column(String(16))
    metric_sieve_results: Mapped[dict | None] = mapped_column(JSONB)
    qualifying_tiers: Mapped[list[str] | None] = mapped_column(ARRAY(String(16)))
    is_game_changer: Mapped[bool] = mapped_column(Boolean, nullable=False, default=False)
    game_changer_reason: Mapped[str | None] = mapped_column(Text)
    makeup_grades: Mapped[dict | None] = mapped_column(JSONB)
    makeup_grade_down: Mapped[dict | None] = mapped_column(JSONB)
    player_identifier: Mapped[str | None] = mapped_column(Text)
    player_identified: Mapped[bool | None] = mapped_column(Boolean)
    identification_note: Mapped[str | None] = mapped_column(Text)
    film_grades: Mapped[dict | None] = mapped_column(JSONB)
    film_flags: Mapped[list | None] = mapped_column(JSONB)
    film_analysis: Mapped[dict | None] = mapped_column(JSONB)
    model_used: Mapped[str | None] = mapped_column(String(64), default="gemini-3.5-flash")
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now())

    athlete: Mapped["Athlete"] = relationship(back_populates="evaluations")
