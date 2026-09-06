"""Confidence calculations remain separate from grade calculations."""
from __future__ import annotations


def weighted_confidence(values: list[tuple[float, float]]) -> float:
    """Return a 0..1 weighted confidence, excluding unavailable samples."""
    usable = [(confidence, weight) for confidence, weight in values if weight > 0]
    if not usable:
        return 0.0
    return round(sum(c * w for c, w in usable) / sum(w for _, w in usable), 4)


def analysis_confidence(
    *, film_quality: float, view_angle: float, player_identity: float,
    tracking: float, field_calibration: float, relevant_snaps: int,
    ai_observation: float,
) -> float:
    snap_factor = min(max(relevant_snaps, 0) / 20, 1.0)
    inputs = [film_quality, view_angle, player_identity, tracking, field_calibration, snap_factor, ai_observation]
    return round(sum(min(max(item, 0), 1) for item in inputs) / len(inputs), 4)

