"""OpenAI Responses API adapter."""
from __future__ import annotations

import os

from .http_provider import decode_json, post_json
from .provider import AIProvider, AIProviderError


class OpenAIProvider(AIProvider):
    def __init__(self):
        self.api_key = os.getenv("OPENAI_API_KEY")
        self.model = os.getenv("OPENAI_MODEL", "gpt-5-mini")
        if not self.api_key:
            raise AIProviderError("OPENAI_API_KEY is required when AI_PROVIDER=openai")

    async def generate_json(self, prompt: str, schema: dict) -> dict:
        payload = {"model": self.model, "input": prompt,
                   "text": {"format": {"type": "json_schema", "name": "football_reasoning",
                                         "strict": True, "schema": schema}}}
        data = await post_json("https://api.openai.com/v1/responses",
                               headers={"Authorization": f"Bearer {self.api_key}"}, payload=payload)
        text = data.get("output_text")
        if not text:
            text = next((part.get("text") for item in data.get("output", []) for part in item.get("content", [])
                         if part.get("type") == "output_text"), None)
        if not text:
            raise AIProviderError("OpenAI response did not contain output text")
        return decode_json(text)

