"""AI provider contract and environment-driven provider factory."""
from __future__ import annotations

import os
from abc import ABC, abstractmethod
from typing import Any


class AIProviderError(RuntimeError):
    pass


class AIProvider(ABC):
    @abstractmethod
    async def generate_json(self, prompt: str, schema: dict[str, Any]) -> dict[str, Any]:
        """Return decoded JSON matching the requested schema."""


class DemoProvider(AIProvider):
    async def generate_json(self, prompt: str, schema: dict[str, Any]) -> dict[str, Any]:
        return {"observations": [], "provider_note": "DEMO: no external AI provider was called."}


def create_provider() -> AIProvider:
    from .anthropic_provider import AnthropicProvider
    from .google_provider import GoogleProvider
    from .openai_provider import OpenAIProvider

    requested = os.getenv("AI_PROVIDER", "").strip().lower()
    if not requested:
        requested = next((name for name, key in (("openai", "OPENAI_API_KEY"), ("google", "GOOGLE_API_KEY"),
                                                  ("anthropic", "ANTHROPIC_API_KEY")) if os.getenv(key)), "demo")
    providers = {"openai": OpenAIProvider, "google": GoogleProvider, "anthropic": AnthropicProvider,
                 "demo": DemoProvider}
    if requested not in providers:
        raise AIProviderError(f"Unsupported AI_PROVIDER: {requested}")
    if requested == "demo" or os.getenv("TRUGRADE_DEMO_MODE", "false").lower() == "true" and not {
        "openai": os.getenv("OPENAI_API_KEY"), "google": os.getenv("GOOGLE_API_KEY"),
        "anthropic": os.getenv("ANTHROPIC_API_KEY")}.get(requested):
        return DemoProvider()
    return providers[requested]()

