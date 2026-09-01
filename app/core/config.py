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
    POSTGRES_HOST: str = "localhost"
    POSTGRES_PORT: int = 5432
    POSTGRES_DB: str = "tru_scouting"
    POSTGRES_USER: str = "tru_admin"
    POSTGRES_PASSWORD: str

    # --- App ---
    APP_ENV: str = "development"
    UPLOAD_TMP_DIR: str = "/tmp/tru_uploads"
    MAX_UPLOAD_MB: int = 500

    @property
    def database_url(self) -> str:
        return (
            f"postgresql+asyncpg://{self.POSTGRES_USER}:{self.POSTGRES_PASSWORD}"
            f"@{self.POSTGRES_HOST}:{self.POSTGRES_PORT}/{self.POSTGRES_DB}"
        )

    class Config:
        env_file = ".env"
        env_file_encoding = "utf-8"


@lru_cache
def get_settings() -> Settings:
    return Settings()
