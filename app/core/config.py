"""Environment configuration — loads .env into typed settings."""
from functools import lru_cache
from typing import Optional

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    # --- Gemini / google-genai ---
    # The native genai.Client() auto-reads GEMINI_API_KEY; we surface it here
    # so misconfiguration fails loudly at startup instead of at first request.
    GEMINI_API_KEY: str
    GEMINI_MODEL: str = "gemini-3.5-flash"

    # --- Auth ---
    # Shared API key required via the X-API-Key header on every /api/v1/scout/*
    # and /api/v1/athletes|evaluations route. Unset (the local-dev default)
    # disables the check entirely — set this before exposing the app publicly.
    API_KEY: Optional[str] = None

    # --- PostgreSQL ---
    # Two ways to configure this: a single DATABASE_URL (what Replit, Neon,
    # Railway, and most one-click Postgres add-ons hand you), or the
    # individual POSTGRES_* fields (what Render's Blueprint wires up). Either
    # is fine — database_url below picks whichever is set.
    DATABASE_URL: Optional[str] = None
    POSTGRES_HOST: str = "localhost"
    POSTGRES_PORT: int = 5432
    POSTGRES_DB: str = "tru_scouting"
    POSTGRES_USER: str = "tru_admin"
    POSTGRES_PASSWORD: Optional[str] = None

    # --- App ---
    APP_ENV: str = "development"
    UPLOAD_TMP_DIR: str = "/tmp/tru_uploads"
    MAX_UPLOAD_MB: int = 500
    GEMINI_FILE_PROCESSING_TIMEOUT_S: int = 21600
    GEMINI_FILE_POLL_INTERVAL_S: int = 5

    def _normalized_dsn(self) -> str:
        """A plain postgresql:// DSN (no driver suffix) from whichever config
        style is in use. Raises a clear error if neither is set, instead of a
        confusing asyncpg/SQLAlchemy failure downstream."""
        if self.DATABASE_URL:
            url = self.DATABASE_URL
            if url.startswith("postgres://"):
                url = "postgresql://" + url[len("postgres://"):]
            return url
        if not self.POSTGRES_PASSWORD:
            raise ValueError(
                "Set either DATABASE_URL, or POSTGRES_PASSWORD plus the other "
                "POSTGRES_* variables, to configure the database connection."
            )
        return (
            f"postgresql://{self.POSTGRES_USER}:{self.POSTGRES_PASSWORD}"
            f"@{self.POSTGRES_HOST}:{self.POSTGRES_PORT}/{self.POSTGRES_DB}"
        )

    @property
    def database_url(self) -> str:
        """SQLAlchemy-style DSN (with the +asyncpg driver suffix) for the app's
        async engine."""
        dsn = self._normalized_dsn()
        return "postgresql+asyncpg://" + dsn[len("postgresql://"):]

    @property
    def asyncpg_dsn(self) -> str:
        """Plain DSN for direct asyncpg connections (scripts/init_db.py),
        which doesn't understand the +asyncpg driver suffix."""
        return self._normalized_dsn()

    class Config:
        env_file = ".env"
        env_file_encoding = "utf-8"


@lru_cache
def get_settings() -> Settings:
    return Settings()
