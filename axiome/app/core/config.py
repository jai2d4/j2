"""Axiome's configuration. Dev-safe defaults; nothing open by accident."""
from __future__ import annotations

import secrets
from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", extra="ignore")

    app_env: str = "development"
    database_url: str = "sqlite+aiosqlite:///./axiome.db"

    admin_key: str = ""
    # Off by default: an app has to present a key Axiome issued.
    open_registration: bool = False

    poll_seconds: int = 60
    poll_timeout_seconds: float = 5.0

    @property
    def is_production(self) -> bool:
        return self.app_env.lower() in {"production", "prod"}

    def resolved_database_url(self) -> str:
        url = self.database_url.strip()
        if url.startswith("postgres://"):
            return "postgresql+asyncpg://" + url[len("postgres://") :]
        if url.startswith("postgresql://"):
            return "postgresql+asyncpg://" + url[len("postgresql://") :]
        if url.startswith("sqlite:///"):
            return "sqlite+aiosqlite:///" + url[len("sqlite:///") :]
        return url

    def resolved_admin_key(self) -> str:
        if self.admin_key:
            return self.admin_key
        if self.is_production:
            raise RuntimeError("ADMIN_KEY must be set when APP_ENV=production")
        return _DEV_ADMIN_KEY


# Printed once at boot so a dev can get in; regenerated every restart.
_DEV_ADMIN_KEY = secrets.token_urlsafe(24)


@lru_cache
def get_settings() -> Settings:
    return Settings()
