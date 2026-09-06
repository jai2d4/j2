"""Request and response shapes for both halves of the control plane."""
from __future__ import annotations

from datetime import datetime
from typing import Any

from pydantic import BaseModel, Field, field_validator

SLUG_RE = r"^[a-z0-9][a-z0-9_-]{1,58}[a-z0-9]$"


# ------------------------------------------------------------ inbound (apps)


class RegisterRequest(BaseModel):
    """What an app sends on boot. Matches the payload Livephoria's client builds."""

    slug: str = Field(pattern=SLUG_RE)
    name: str = Field(default="", max_length=120)
    kind: str = Field(default="app", max_length=60)
    version: str = Field(default="", max_length=40)
    public_url: str = Field(default="", max_length=400)
    control_url: str = Field(default="/api/v1/control", max_length=200)
    capabilities: list[str] = Field(default_factory=list)
    registered_at: datetime | None = None

    @field_validator("slug")
    @classmethod
    def _lower(cls, v: str) -> str:
        return v.lower()


class HeartbeatRequest(BaseModel):
    slug: str
    at: datetime | None = None
    status: str = "ok"
    metrics: dict[str, Any] = Field(default_factory=dict)


class EventRequest(BaseModel):
    slug: str
    event: str = Field(max_length=80)
    at: datetime | None = None
    data: dict[str, Any] = Field(default_factory=dict)


class Ack(BaseModel):
    ok: bool = True
    app: str
    detail: str = ""


# ------------------------------------------------------------ admin


class AppOut(BaseModel):
    id: int
    slug: str
    name: str
    kind: str
    version: str
    base_url: str
    public_url: str
    control_path: str
    health_path: str
    capabilities: list[str]
    status: str
    status_detail: str
    maintenance: bool
    enabled: bool
    metrics: dict[str, Any]
    last_seen_at: datetime | None
    last_latency_ms: float | None
    seconds_since_seen: int | None
    has_control_key: bool
    app_key_hint: str
    created_at: datetime


class AppCreate(BaseModel):
    """Adding an app by hand — the 'connect' flow in the dashboard."""

    name: str = Field(min_length=1, max_length=120)
    base_url: str = Field(min_length=1, max_length=400)
    control_key: str = Field(default="", max_length=200)
    slug: str | None = Field(default=None, pattern=SLUG_RE)
    control_path: str = "/api/v1/control"
    health_path: str = "/api/v1/health"

    @field_validator("base_url")
    @classmethod
    def _trim(cls, v: str) -> str:
        v = v.strip().rstrip("/")
        if not v.startswith(("http://", "https://")):
            raise ValueError("base_url must start with http:// or https://")
        return v


class AppUpdate(BaseModel):
    name: str | None = None
    base_url: str | None = None
    control_key: str | None = None
    control_path: str | None = None
    health_path: str | None = None
    enabled: bool | None = None


class ConnectResult(BaseModel):
    """What a connection attempt found, so a failure explains itself."""

    ok: bool
    reachable: bool
    authenticated: bool
    detail: str
    app: AppOut | None = None
    # Shown once. Paste into the app's AXIOME_APP_KEY so it can push to Axiome.
    app_key: str | None = None
    probe: dict[str, Any] = Field(default_factory=dict)


class ProbeRequest(BaseModel):
    base_url: str
    control_key: str = ""
    control_path: str = "/api/v1/control"
    health_path: str = "/api/v1/health"


class MaintenanceRequest(BaseModel):
    enabled: bool
    message: str = Field(default="", max_length=300)


class EventOut(BaseModel):
    id: int
    app_slug: str
    event: str
    data: dict[str, Any]
    direction: str
    at: datetime


class SampleOut(BaseModel):
    at: datetime
    status: str
    latency_ms: float | None
    source: str
    metrics: dict[str, Any]


class ActionRequest(BaseModel):
    """Generic passthrough to an app's own control surface."""

    method: str = "POST"
    path: str = Field(min_length=1, max_length=200)
    body: dict[str, Any] = Field(default_factory=dict)

    @field_validator("method")
    @classmethod
    def _known(cls, v: str) -> str:
        if v.upper() not in {"GET", "POST"}:
            raise ValueError("method must be GET or POST")
        return v.upper()
