"""Module 5 — evaluation (Truth Report) history."""
from __future__ import annotations

from uuid import UUID

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy import select
from sqlalchemy.ext.asyncio import AsyncSession

from app.core.auth import require_api_key
from app.core.db import get_db
from app.models import orm
from app.models.schemas import EvaluationOut

router = APIRouter(
    prefix="/api/v1/evaluations", tags=["evaluations"], dependencies=[Depends(require_api_key)],
)


@router.get("", response_model=list[EvaluationOut])
async def list_evaluations(
    athlete_id: UUID | None = None,
    limit: int = 50,
    offset: int = 0,
    db: AsyncSession = Depends(get_db),
):
    stmt = select(orm.Evaluation).order_by(orm.Evaluation.created_at.desc()).limit(min(limit, 200)).offset(offset)
    if athlete_id:
        stmt = stmt.where(orm.Evaluation.athlete_id == athlete_id)
    result = await db.execute(stmt)
    return result.scalars().all()


@router.get("/{evaluation_id}", response_model=EvaluationOut)
async def get_evaluation(evaluation_id: UUID, db: AsyncSession = Depends(get_db)):
    row = await db.get(orm.Evaluation, evaluation_id)
    if row is None:
        raise HTTPException(status_code=404, detail="Evaluation not found.")
    return row
