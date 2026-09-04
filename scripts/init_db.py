"""Applies db/init_schema.sql on container startup. Safe to run on every
boot — every CREATE TABLE/INDEX is IF NOT EXISTS and the seed INSERT is
ON CONFLICT DO NOTHING, so a second run against an already-initialized
database is a silent no-op instead of an error.

Used by the Docker CMD (see Dockerfile) so a fresh Render/Railway/Fly
Postgres gets its schema without a manual `psql` step. Never touches the
app's own asyncpg pool — this is a one-shot connection that closes
immediately after running.
"""
import asyncio
import sys
from pathlib import Path

import asyncpg

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from app.core.config import get_settings  # noqa: E402

SCHEMA_PATH = Path(__file__).resolve().parent.parent / "db" / "init_schema.sql"


async def main() -> None:
    settings = get_settings()
    sql = SCHEMA_PATH.read_text()

    conn = await asyncpg.connect(dsn=settings.asyncpg_dsn)
    try:
        # No bind params -> asyncpg uses the simple query protocol, which
        # allows multiple ';'-separated statements in one call.
        await conn.execute(sql)
        print("[init_db] schema applied (or already present) — OK")
    finally:
        await conn.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except Exception as e:
        # Don't block app startup over a DB hiccup — main.py's best-effort
        # persistence layer already tolerates a cold/unreachable database.
        print(f"[init_db] skipped: {type(e).__name__}: {e}", file=sys.stderr)
