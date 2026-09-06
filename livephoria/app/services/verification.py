"""Age assurance.

This app does not decide anyone's age. It records a submission, then waits for a
decision from an identity provider — or, failing one, from an operator through
the control plane. `MockAgeVerificationProvider` therefore returns `pending` and
nothing else: an auto-approving stub would be worse than none, because the rest
of the code would then be enforcing a check that never actually happened.

Swap in a real IDV vendor by implementing `AgeVerificationProvider` and returning
it from `get_provider()`.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import date
from typing import Protocol

MINIMUM_AGE = 18


@dataclass(frozen=True)
class VerificationSubmission:
    status: str  # pending | verified | failed
    reference: str
    message: str


class AgeVerificationProvider(Protocol):
    async def submit(self, user_id: int, declared_dob: date) -> VerificationSubmission: ...


class MockAgeVerificationProvider:
    """Records the request and hands it to a human. Never approves on its own."""

    name = "mock"

    async def submit(self, user_id: int, declared_dob: date) -> VerificationSubmission:
        return VerificationSubmission(
            status="pending",
            reference=f"mock_idv_{user_id}",
            message=(
                "Submitted for review. No identity provider is connected in this build, "
                "so an operator has to approve it."
            ),
        )


def get_provider() -> AgeVerificationProvider:
    return MockAgeVerificationProvider()


def declared_age_is_adult(dob: date, today: date | None = None) -> bool:
    """A self-declared birthday is a gate on *submitting*, never proof of age."""
    today = today or date.today()
    years = today.year - dob.year - ((today.month, today.day) < (dob.month, dob.day))
    return years >= MINIMUM_AGE
