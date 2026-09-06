"""Prospect profile contracts; character/academics require verified inputs."""
from __future__ import annotations

from .models import ProfileCategory, ProspectProfile


PROFILE_CATEGORIES = ("SIZE", "ATHLETIC ABILITY", "PLAY HISTORY", "PLAY STYLE", "CHARACTER")


def build_profile(verified: dict[str, tuple[ProspectProfile, str]]) -> list[ProfileCategory]:
    profile = []
    for category in PROFILE_CATEGORIES:
        supplied = verified.get(category)
        profile.append(ProfileCategory(
            category=category,
            profile=supplied[0] if supplied else None,
            available=supplied is not None,
            source=supplied[1] if supplied else None,
        ))
    return profile

