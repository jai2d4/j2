"""Data-access layer — raw SQL against the schema in db/init_schema.sql.

Plain asyncpg queries, no ORM, so the SQL file stays the single source of
truth for table shape. Every query here mirrors a table defined there.
"""
from __future__ import annotations

from typing import Optional
from uuid import UUID

import asyncpg

from app.models.schemas import AthleteCreate, Position


async def create_athlete(pool: asyncpg.Pool, athlete: AthleteCreate) -> asyncpg.Record:
    return await pool.fetchrow(
        """
        INSERT INTO athletes
            (first_name, last_name, grad_year, school, state, position,
             height_in, weight_lbs, forty_s, shuttle_s, bench_lbs, squat_lbs,
             gpa, sat, act)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15)
        RETURNING *
        """,
        athlete.first_name, athlete.last_name, athlete.grad_year, athlete.school,
        athlete.state, athlete.position.value, athlete.height_in, athlete.weight_lbs,
        athlete.forty_s, athlete.shuttle_s, athlete.bench_lbs, athlete.squat_lbs,
        athlete.gpa, athlete.sat, athlete.act,
    )


async def get_athlete(pool: asyncpg.Pool, athlete_id: UUID) -> Optional[asyncpg.Record]:
    return await pool.fetchrow("SELECT * FROM athletes WHERE id = $1", athlete_id)


async def list_athletes(pool: asyncpg.Pool, limit: int = 100) -> list[asyncpg.Record]:
    return await pool.fetch(
        "SELECT * FROM athletes ORDER BY created_at DESC LIMIT $1", limit
    )


async def create_film_upload(
    pool: asyncpg.Pool,
    *,
    athlete_id: UUID,
    filename: str,
    gemini_file_id: Optional[str],
    mime_type: Optional[str],
    status: str,
) -> asyncpg.Record:
    return await pool.fetchrow(
        """
        INSERT INTO film_uploads (athlete_id, filename, gemini_file_id, mime_type, status)
        VALUES ($1, $2, $3, $4, $5)
        RETURNING *
        """,
        athlete_id, filename, gemini_file_id, mime_type, status,
    )


async def update_film_status(pool: asyncpg.Pool, film_id: UUID, status: str) -> None:
    await pool.execute("UPDATE film_uploads SET status = $1 WHERE id = $2", status, film_id)


async def create_evaluation(
    pool: asyncpg.Pool,
    *,
    athlete_id: UUID,
    film_id: Optional[UUID],
    position_evaluated: Position,
    projected_tier: str,
    physical_projection: Optional[dict],
    metric_sieve_results: dict,
    qualifying_tiers: list[str],
    is_game_changer: bool,
    game_changer_reason: Optional[str],
    makeup_grades: Optional[dict],
    makeup_grade_down: Optional[dict],
    model_used: str,
) -> asyncpg.Record:
    return await pool.fetchrow(
        """
        INSERT INTO evaluations
            (athlete_id, film_id, position_evaluated, projected_tier,
             physical_projection, metric_sieve_results, qualifying_tiers,
             is_game_changer, game_changer_reason, makeup_grades,
             makeup_grade_down, model_used)
        VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)
        RETURNING *
        """,
        athlete_id, film_id, position_evaluated.value, projected_tier,
        physical_projection, metric_sieve_results, qualifying_tiers,
        is_game_changer, game_changer_reason, makeup_grades,
        makeup_grade_down, model_used,
    )


async def get_evaluation(pool: asyncpg.Pool, evaluation_id: UUID) -> Optional[asyncpg.Record]:
    return await pool.fetchrow("SELECT * FROM evaluations WHERE id = $1", evaluation_id)


async def list_evaluations_for_athlete(
    pool: asyncpg.Pool, athlete_id: UUID
) -> list[asyncpg.Record]:
    return await pool.fetch(
        "SELECT * FROM evaluations WHERE athlete_id = $1 ORDER BY created_at DESC",
        athlete_id,
    )
