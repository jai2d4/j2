"""Module 5 — athlete roster CRUD."""
from __future__ import annotations

from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.auth import require_api_key
from app.core.db import get_db
from app.models import orm
from app.models.schemas import Athlete, AthleteCreate

router = APIRouter(
    prefix="/api/v1/athletes", tags=["athletes"], dependencies=[Depends(require_api_key)],
)


@router.post("", response_model=Athlete, status_code=201)
async def create_athlete(athlete: AthleteCreate, db: AsyncSession = Depends(get_db)):
    row = orm.Athlete(**athlete.model_dump())
    db.add(row)
    await db.commit()
    await db.refresh(row)
    return row


@router.get("", response_model=list[Athlete])
async def list_athletes(
    position: str | None = None,
    limit: int = 50,
    offset: int = 0,
    db: AsyncSession = Depends(get_db),
):
    stmt = select(orm.Athlete).order_by(orm.Athlete.created_at.desc()).limit(min(limit, 200)).offset(offset)
    if position:
        stmt = stmt.where(orm.Athlete.position == position)
    result = await db.execute(stmt)
    return result.scalars().all()


@router.get("/{athlete_id}", response_model=Athlete)
async def get_athlete(athlete_id: UUID, db: AsyncSession = Depends(get_db)):
    row = await db.get(orm.Athlete, athlete_id)
    if row is None:
        raise HTTPException(status_code=404, detail="Athlete not found.")
    return row
