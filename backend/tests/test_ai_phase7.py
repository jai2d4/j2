import asyncio

import pytest
from pydantic import ValidationError

from backend.ai.football_reasoner import FootballReasoner, FootballReasoningResult, SYSTEM_RULE
from backend.ai.provider import AIProvider, DemoProvider, create_provider
from backend.football.observations import FootballObservation


class FakeProvider(AIProvider):
    def __init__(self, response):
        self.response = response
        self.prompt = None

    async def generate_json(self, prompt, schema):
        self.prompt = prompt
        assert "official TruGrade score" in prompt
        assert schema["additionalProperties"] is False
        return self.response


def source_observation():
    return FootballObservation(observation_id="O1", play_id="P1", player_id="19", position="LB")


def test_reasoner_validates_strict_structured_json():
    provider = FakeProvider({"observations": [{"trait": "read_react", "value": "correct_assignment",
                                                "confidence": .9, "evidence_timestamp": 12.5,
                                                "reason": "Triggered after seeing the puller."}]})
    result = asyncio.run(FootballReasoner(provider).reason(source_observation()))
    assert result.observations[0].trait == "read_react"
    assert SYSTEM_RULE in provider.prompt


def test_final_score_is_rejected_from_ai_output():
    with pytest.raises(ValidationError):
        FootballReasoningResult.model_validate({"observations": [], "official_trugrade_score": 99})


def test_invalid_confidence_is_rejected():
    with pytest.raises(ValidationError):
        FootballReasoningResult.model_validate({"observations": [{"trait": "range", "value": "unknown",
            "confidence": 2, "evidence_timestamp": None, "reason": "Film unclear"}]})


def test_demo_mode_never_calls_external_provider(monkeypatch):
    monkeypatch.setenv("TRUGRADE_DEMO_MODE", "true")
    monkeypatch.setenv("AI_PROVIDER", "google")
    monkeypatch.delenv("GOOGLE_API_KEY", raising=False)
    monkeypatch.delenv("GEMINI_API_KEY", raising=False)
    assert isinstance(create_provider(), DemoProvider)


def test_provider_auto_selection_prefers_openai_key(monkeypatch):
    monkeypatch.delenv("AI_PROVIDER", raising=False)
    monkeypatch.setenv("TRUGRADE_DEMO_MODE", "false")
    monkeypatch.setenv("OPENAI_API_KEY", "test-key")
    monkeypatch.delenv("GOOGLE_API_KEY", raising=False)
    assert create_provider().__class__.__name__ == "OpenAIProvider"

