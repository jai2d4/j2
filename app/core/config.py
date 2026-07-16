"""Environment configuration — loads .env into typed settings."""
from functools import lru_cache
from typing import Optional

from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    # --- Gemini / google-genai ---
    GEMINI_API_KEY: str
    GEMINI_MODEL: str = "gemini-3.5-flash"

    # --- PostgreSQL ---
    # DATABASE_URL takes priority when set (hosted providers like Neon/Supabase
    # hand you one ready-made connection string, sslmode and all). Falls back
    # to the discrete POSTGRES_* fields for a local install.
    DATABASE_URL: Optional[str] = None
    POSTGRES_HOST: str = "localhost"
    POSTGRES_PORT: int = 5432
    POSTGRES_DB: str = "tru_scouting"
    POSTGRES_USER: str = "tru_admin"
    POSTGRES_PASSWORD: str = "change_me"

    # --- App ---
    APP_ENV: str = "development"
    UPLOAD_TMP_DIR: str = "/tmp/tru_uploads"
    MAX_UPLOAD_MB: int = 500

    @property
    def database_url(self) -> str:
        if self.DATABASE_URL:
            return self.DATABASE_URL
        return (
            f"postgresql://{self.POSTGRES_USER}:{self.POSTGRES_PASSWORD}"
            f"@{self.POSTGRES_HOST}:{self.POSTGRES_PORT}/{self.POSTGRES_DB}"
        )

    class Config:
        env_file = ".env"
        env_file_encoding = "utf-8"


@lru_cache
def get_settings() -> Settings:
    return Settings()
