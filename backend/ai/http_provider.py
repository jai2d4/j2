"""Shared safe HTTP behavior for external AI adapters."""
from __future__ import annotations

import json

import httpx

from .provider import AIProviderError


async def post_json(url: str, *, headers: dict, payload: dict, timeout: float = 120) -> dict:
    async with httpx.AsyncClient(timeout=timeout) as client:
        response = await client.post(url, headers=headers, json=payload)
    if response.status_code >= 400:
        raise AIProviderError(f"AI provider request failed with HTTP {response.status_code}")
    return response.json()


def decode_json(text: str) -> dict:
    cleaned = text.strip()
    if cleaned.startswith("```"):
        cleaned = cleaned.split("\n", 1)[-1].rsplit("```", 1)[0].strip()
    try:
        value = json.loads(cleaned)
    except json.JSONDecodeError as exc:
        raise AIProviderError("AI provider returned invalid JSON") from exc
    if not isinstance(value, dict):
        raise AIProviderError("AI provider JSON must be an object")
    return value

