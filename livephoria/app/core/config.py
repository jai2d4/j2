"""Typed environment config. Every value has a dev-safe default so the app boots cold."""
from __future__ import annotations

import secrets
from functools import lru_cache

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", extra="ignore")

    app_env: str = "development"
    app_name: str = "Livephoria"
    tagline: str = "Where fans get closer."

    database_url: str = "sqlite+aiosqlite:///./livephoria.db"

    jwt_secret: str = ""
    jwt_ttl_minutes: int = 60 * 24 * 7
    jwt_algorithm: str = "HS256"

    # Platform's cut of creator revenue, in basis points (1000 = 10%).
    platform_fee_bps: int = 1000
    payments_provider: str = "mock"

    # Guardrails on user-supplied amounts, in cents.
    min_tip_cents: int = 100
    max_transaction_cents: int = 100_000

    # ---- Axiome control plane ----
    # Empty base URL disables the integration entirely; the app runs standalone.
    axiome_base_url: str = ""
    axiome_app_key: str = ""
    # Shared secret Axiome must present on /api/v1/control/*. Without it those
    # routes are refused outright rather than left open.
    axiome_control_key: str = ""
    axiome_public_url: str = ""
    axiome_heartbeat_seconds: int = 60

    @property
    def is_production(self) -> bool:
        return self.app_env.lower() in {"production", "prod"}

    def resolved_database_url(self) -> str:
        """Accept the bare postgres URLs hosts hand out and make them async-driver URLs."""
        url = self.database_url.strip()
        if url.startswith("postgres://"):
            url = "postgresql+asyncpg://" + url[len("postgres://") :]
        elif url.startswith("postgresql://"):
            url = "postgresql+asyncpg://" + url[len("postgresql://") :]
        elif url.startswith("sqlite:///"):
            url = "sqlite+aiosqlite:///" + url[len("sqlite:///") :]
        return url

    def resolved_jwt_secret(self) -> str:
        """Fall back to a per-process secret in dev; refuse to guess in production."""
        if self.jwt_secret:
            return self.jwt_secret
        if self.is_production:
            raise RuntimeError("JWT_SECRET must be set when APP_ENV=production")
        return _DEV_SECRET


# Regenerated on every boot: dev tokens do not survive a restart, by design.
_DEV_SECRET = secrets.token_urlsafe(48)


@lru_cache
def get_settings() -> Settings:
    return Settings()
