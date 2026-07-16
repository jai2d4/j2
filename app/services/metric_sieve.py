"""
Modules 2 & 3 — CV Analytics & Metric Sieve Logic.

Hard-coded positional matrices from the blueprint, mirroring db/init_schema.sql.
Laser metrics (40-yd, pro-agility shuttle) are checked first and outrank
subjective variables. Runs even with a cold DB — the DB matrix is the
system of record; this in-memory copy keeps the sieve fast and deterministic.
"""
from __future__ import annotations

from typing import Optional

from app.models.schemas import MetricCheck, Position, SieveResult, Tier

# (min, max) tuples; None = no constraint at that bound
POSITION_MATRIX: dict[tuple[str, str], dict] = {
    ("QB", "D1_FBS"): dict(height=(74.0, 78.0), weight=(200, 240), forty=(4.60, 4.90),
                           gpa_min=3.00, sat_min=1000, act_min=18),
    ("QB", "D2_D3_NAIA_JUCO"): dict(height=(72.0, 77.0), weight=(180, 225), forty=(4.70, 5.00)),
    ("RB", "D1_FBS"): dict(height=(69.0, 73.0), weight=(190, 230), forty=(4.40, 4.60),
                           bench_min=300, squat_min=450, gpa_min=3.00, sat_min=1000, act_min=18),
    ("RB", "D1_FCS_D2"): dict(height=(68.0, 72.0), weight=(180, 220), forty=(4.50, 4.70)),
    ("WR", "D1_FBS"): dict(height=(72.0, 76.0), weight=(180, 220), forty=(4.30, 4.60),
                           gpa_min=3.00, sat_min=1000, act_min=18),
    ("WR", "D1_FCS_D2"): dict(height=(70.0, 75.0), weight=(170, 210), forty=(4.40, 4.70)),
    ("DB", "D1_FBS"): dict(height=(70.0, 74.0), weight=(175, 210), forty=(4.40, 4.60),
                           shuttle_max=4.20),
    ("DB", "D1_FCS"): dict(height=(69.0, 73.0), weight=(170, 200), forty=(4.50, 4.70)),
    ("LB", "D1_FBS"): dict(height=(73.0, 76.0), weight=(220, 250), forty=(4.50, 4.70)),
    ("DE", "D1_FBS"): dict(height=(75.0, 78.0), weight=(240, 280), forty=(4.60, 4.80)),
    ("DL", "D1_FBS"): dict(height=(75.0, 78.0), weight=(250, 320), forty=(4.80, 5.10)),
    ("OL", "D1_FBS"): dict(height=(76.0, 80.0), weight=(280, 330), forty=(5.00, 5.30)),
    ("TE", "D1_FBS"): dict(height=(76.0, 79.0), weight=(230, 270), forty=(4.60, 4.80)),
}

# Tier evaluation order per position (best bracket first)
TIER_LADDER: dict[str, list[str]] = {
    "QB": ["D1_FBS", "D2_D3_NAIA_JUCO"],
    "RB": ["D1_FBS", "D1_FCS_D2"],
    "WR": ["D1_FBS", "D1_FCS_D2"],
    "DB": ["D1_FBS", "D1_FCS"],
    "LB": ["D1_FBS"],
    "DE": ["D1_FBS"],
    "DL": ["D1_FBS"],
    "OL": ["D1_FBS"],
    "TE": ["D1_FBS"],
}


def _in_range(value: Optional[float], lo: Optional[float], hi: Optional[float]) -> Optional[bool]:
    if value is None:
        return None
    if lo is not None and value < lo:
        return False
    if hi is not None and value > hi:
        return False
    return True


def run_sieve(
    position: Position,
    *,
    height_in: Optional[float] = None,
    weight_lbs: Optional[int] = None,
    forty_s: Optional[float] = None,
    shuttle_s: Optional[float] = None,
    bench_lbs: Optional[int] = None,
    squat_lbs: Optional[int] = None,
    gpa: Optional[float] = None,
    sat: Optional[int] = None,
    act: Optional[int] = None,
) -> SieveResult:
    """Walk the tier ladder top-down; project the highest tier where all
    provided hard metrics pass. Laser metrics evaluated first."""
    best: Optional[SieveResult] = None

    for tier_name in TIER_LADDER[position.value]:
        matrix = POSITION_MATRIX[(position.value, tier_name)]
        checks: list[MetricCheck] = []

        # --- Laser metrics FIRST (priority over subjective variables) ---
        if "forty" in matrix:
            lo, hi = matrix["forty"]
            checks.append(MetricCheck(
                metric="forty_yard_dash", athlete_value=forty_s,
                threshold=f"{lo}–{hi}s", passed=_in_range(forty_s, None, hi)))
        if "shuttle_max" in matrix:
            checks.append(MetricCheck(
                metric="pro_agility_shuttle", athlete_value=shuttle_s,
                threshold=f"<= {matrix['shuttle_max']}s",
                passed=_in_range(shuttle_s, None, matrix["shuttle_max"])))

        # --- Frame ---
        if "height" in matrix:
            lo, hi = matrix["height"]
            checks.append(MetricCheck(
                metric="height_in", athlete_value=height_in,
                threshold=f"{lo}–{hi} in", passed=_in_range(height_in, lo, hi)))
        if "weight" in matrix:
            lo, hi = matrix["weight"]
            checks.append(MetricCheck(
                metric="weight_lbs", athlete_value=weight_lbs,
                threshold=f"{lo}–{hi} lbs", passed=_in_range(weight_lbs, lo, hi)))

        # --- Strength ---
        if "bench_min" in matrix:
            checks.append(MetricCheck(
                metric="bench_lbs", athlete_value=bench_lbs,
                threshold=f">= {matrix['bench_min']} lbs",
                passed=_in_range(bench_lbs, matrix["bench_min"], None)))
        if "squat_min" in matrix:
            checks.append(MetricCheck(
                metric="squat_lbs", athlete_value=squat_lbs,
                threshold=f">= {matrix['squat_min']} lbs",
                passed=_in_range(squat_lbs, matrix["squat_min"], None)))

        # --- Academics ---
        if "gpa_min" in matrix:
            checks.append(MetricCheck(
                metric="gpa", athlete_value=gpa,
                threshold=f">= {matrix['gpa_min']}",
                passed=_in_range(gpa, matrix["gpa_min"], None)))
        if "sat_min" in matrix and sat is not None:
            checks.append(MetricCheck(
                metric="sat", athlete_value=sat,
                threshold=f">= {matrix['sat_min']}",
                passed=_in_range(sat, matrix["sat_min"], None)))
        if "act_min" in matrix and act is not None:
            checks.append(MetricCheck(
                metric="act", athlete_value=act,
                threshold=f">= {matrix['act_min']}",
                passed=_in_range(act, matrix["act_min"], None)))

        # SAT/ACT are either/or: pass if at least one qualifies
        hard_fail = any(c.passed is False for c in checks
                        if c.metric not in ("sat", "act"))
        academic_scores = [c for c in checks if c.metric in ("sat", "act")]
        if academic_scores and not any(c.passed for c in academic_scores):
            hard_fail = True

        result = SieveResult(
            position=position, tier=Tier(tier_name), checks=checks,
            hard_metrics_passed=not hard_fail)

        # Guard: no provided data at all → cannot project a tier
        if all(c.passed is None for c in checks):
            result.hard_metrics_passed = False
            result.tier = Tier.UNRANKED
            return result

        if not hard_fail:
            return result  # highest passing tier — done
        if best is None:
            best = result  # keep top-tier detail for game-changer analysis

    # No tier passed → check for out-of-bracket "Game Changer"
    # (e.g., undersized athlete whose laser metrics beat the top bracket)
    assert best is not None
    laser = [c for c in best.checks if c.metric in ("forty_yard_dash", "pro_agility_shuttle")]
    frame = [c for c in best.checks if c.metric in ("height_in", "weight_lbs")]
    laser_provided = [c for c in laser if c.passed is not None]
    if laser_provided and all(c.passed for c in laser_provided) \
            and any(c.passed is False for c in frame):
        best.is_game_changer = True
        best.game_changer_reason = (
            "Laser metrics meet or beat D1 FBS thresholds despite out-of-bracket frame. "
            "Flag for Truth Report Panel manual review."
        )

    best.tier = Tier.UNRANKED if not best.is_game_changer else best.tier
    return best
