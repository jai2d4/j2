"""Anthropic Messages API adapter."""
from __future__ import annotations

import os

from .http_provider import decode_json, post_json
from .provider import AIProvider, AIProviderError


class AnthropicProvider(AIProvider):
    def __init__(self):
        self.api_key = os.getenv("ANTHROPIC_API_KEY")
        self.model = os.getenv("ANTHROPIC_MODEL", "claude-sonnet-4-5")
        if not self.api_key:
            raise AIProviderError("ANTHROPIC_API_KEY is required when AI_PROVIDER=anthropic")

    async def generate_json(self, prompt: str, schema: dict) -> dict:
        schema_prompt = f"Return only JSON matching this schema: {schema}"
        payload = {"model": self.model, "max_tokens": 4096,
                   "system": schema_prompt, "messages": [{"role": "user", "content": prompt}]}
        data = await post_json("https://api.anthropic.com/v1/messages",
                               headers={"x-api-key": self.api_key, "anthropic-version": "2023-06-01"}, payload=payload)
        try:
            text = next(item["text"] for item in data["content"] if item["type"] == "text")
        except (KeyError, StopIteration, TypeError) as exc:
            raise AIProviderError("Anthropic response did not contain output text") from exc
        return decode_json(text)

