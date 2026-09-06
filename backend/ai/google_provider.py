"""Google Gemini generateContent adapter."""
from __future__ import annotations

import os

from .http_provider import decode_json, post_json
from .provider import AIProvider, AIProviderError


class GoogleProvider(AIProvider):
    def __init__(self):
        self.api_key = os.getenv("GOOGLE_API_KEY") or os.getenv("GEMINI_API_KEY")
        self.model = os.getenv("GOOGLE_MODEL", "gemini-2.5-flash")
        if not self.api_key:
            raise AIProviderError("GOOGLE_API_KEY is required when AI_PROVIDER=google")

    async def generate_json(self, prompt: str, schema: dict) -> dict:
        url = f"https://generativelanguage.googleapis.com/v1beta/models/{self.model}:generateContent"
        payload = {"contents": [{"parts": [{"text": prompt}]}],
                   "generationConfig": {"responseMimeType": "application/json", "responseSchema": schema}}
        data = await post_json(url, headers={"x-goog-api-key": self.api_key}, payload=payload)
        try:
            text = data["candidates"][0]["content"]["parts"][0]["text"]
        except (KeyError, IndexError, TypeError) as exc:
            raise AIProviderError("Google response did not contain output text") from exc
        return decode_json(text)

