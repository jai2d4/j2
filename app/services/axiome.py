"""Axiome control-plane client.

Axiome is the operator's controller app: it holds the roster of apps, and this
module is Livephoria's side of that link. It does two things:

  * announces this app to Axiome on boot, then sends a heartbeat with live counts
  * emits an event whenever something happens Axiome may care about

IMPORTANT — the request shapes below are *this app's proposal*, not a contract
read off Axiome. Nothing here has been verified against a running Axiome
instance. When the real endpoints are known, change the four constants and the
payload builders in this file and nothing else in the codebase moves.

The whole integration is inert unless AXIOME_BASE_URL is set, so the app runs
standalone with no controller present. Failures never propagate: a controller
that is down must not take the platform down with it.
"""
from __future__ import annotations

import asyncio
import json
import logging
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

log = logging.getLogger("livephoria.axiome")

# Paths on the Axiome side. Proposed; adjust to match the real controller.
REGISTER_PATH = "/api/apps/register"
HEARTBEAT_PATH = "/api/apps/heartbeat"
EVENTS_PATH = "/api/apps/events"

APP_SLUG = "livephoria"
APP_KIND = "creator-platform"


@dataclass
class AxiomeConfig:
    base_url: str = ""
    app_key: str = ""
    public_url: str = ""
    heartbeat_seconds: int = 60
    timeout_seconds: float = 5.0

    @property
    def enabled(self) -> bool:
        return bool(self.base_url)


@dataclass
class AxiomeClient:
    config: AxiomeConfig
    # Last outcome per operation, surfaced on /api/v1/control/status for debugging.
    last_result: dict[str, str] = field(default_factory=dict)

    def _post(self, path: str, payload: dict[str, Any]) -> dict[str, Any] | None:
        if not self.config.enabled:
            return None
        url = self.config.base_url.rstrip("/") + path
        body = json.dumps(payload).encode()
        request = urllib.request.Request(url, data=body, method="POST")
        request.add_header("Content-Type", "application/json")
        request.add_header("User-Agent", f"{APP_SLUG}/control-plane")
        if self.config.app_key:
            request.add_header("Authorization", f"Bearer {self.config.app_key}")
        with urllib.request.urlopen(request, timeout=self.config.timeout_seconds) as response:
            raw = response.read().decode() or "{}"
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {"raw": raw}

    async def _post_async(self, name: str, path: str, payload: dict[str, Any]) -> dict | None:
        """Never raises. A controller outage is logged and dropped, not propagated."""
        if not self.config.enabled:
            return None
        try:
            result = await asyncio.to_thread(self._post, path, payload)
            self.last_result[name] = "ok"
            return result
        except (urllib.error.URLError, OSError, TimeoutError) as exc:
            self.last_result[name] = f"failed: {exc}"
            log.warning("Axiome %s failed: %s", name, exc)
            return None

    async def register(self, version: str, capabilities: list[str]) -> dict | None:
        return await self._post_async(
            "register",
            REGISTER_PATH,
            {
                "slug": APP_SLUG,
                "kind": APP_KIND,
                "name": "Livephoria",
                "version": version,
                "public_url": self.config.public_url,
                "control_url": "/api/v1/control",
                "capabilities": capabilities,
                "registered_at": _now(),
            },
        )

    async def heartbeat(self, metrics: dict[str, Any]) -> dict | None:
        return await self._post_async(
            "heartbeat",
            HEARTBEAT_PATH,
            {"slug": APP_SLUG, "at": _now(), "status": "ok", "metrics": metrics},
        )

    async def emit(self, event: str, data: dict[str, Any]) -> None:
        await self._post_async(
            "event",
            EVENTS_PATH,
            {"slug": APP_SLUG, "event": event, "at": _now(), "data": data},
        )


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


_client: AxiomeClient | None = None


def configure(config: AxiomeConfig) -> AxiomeClient:
    global _client
    _client = AxiomeClient(config)
    return _client


def client() -> AxiomeClient:
    """Always returns a client; a disabled one no-ops every call."""
    global _client
    if _client is None:
        _client = AxiomeClient(AxiomeConfig())
    return _client
