"""Axiome calling an app.

Deliberately stdlib-only and always non-raising: a controller that crashes because
one app is down is worse than no controller. Every call returns a Result the
caller can render.
"""
from __future__ import annotations

import asyncio
import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any


@dataclass
class Result:
    ok: bool
    status_code: int | None
    data: Any = None
    error: str = ""
    latency_ms: float | None = None

    @property
    def unauthorized(self) -> bool:
        return self.status_code in (401, 403)

    @property
    def reachable(self) -> bool:
        """A 401 still proves something answered — that is worth distinguishing."""
        return self.status_code is not None


def _call(
    url: str, *, method: str, key: str, body: dict | None, timeout: float
) -> Result:
    payload = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(url, data=payload, method=method)
    request.add_header("Accept", "application/json")
    request.add_header("User-Agent", "axiome/control-plane")
    if payload is not None:
        request.add_header("Content-Type", "application/json")
    if key:
        request.add_header("X-Axiome-Key", key)

    started = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read().decode() or "{}"
            elapsed = (time.perf_counter() - started) * 1000
            try:
                data = json.loads(raw)
            except json.JSONDecodeError:
                data = {"raw": raw[:500]}
            return Result(True, response.status, data, latency_ms=round(elapsed, 1))
    except urllib.error.HTTPError as exc:
        elapsed = (time.perf_counter() - started) * 1000
        detail = ""
        try:
            detail = json.loads(exc.read().decode()).get("detail", "")
        except Exception:
            pass
        return Result(False, exc.code, None, detail or exc.reason, round(elapsed, 1))
    except (urllib.error.URLError, OSError, TimeoutError, ValueError) as exc:
        return Result(False, None, None, str(getattr(exc, "reason", exc)))


async def call(
    base_url: str,
    path: str,
    *,
    method: str = "GET",
    key: str = "",
    body: dict | None = None,
    timeout: float = 5.0,
) -> Result:
    url = base_url.rstrip("/") + "/" + path.lstrip("/")
    return await asyncio.to_thread(_call, url, method=method, key=key, body=body, timeout=timeout)
