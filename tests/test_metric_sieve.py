"""Modules 2 & 3 — hard-metric sieve, direct unit tests (no HTTP, no mocks needed)."""
from app.models.schemas import Position, Tier
from app.services.metric_sieve import run_sieve


def test_fbs_db_clears_every_tier():
    r = run_sieve(Position.DB, height_in=72, weight_lbs=190, forty_s=4.45,
                  shuttle_s=4.15, gpa=3.4, sat=1100)
    assert r.tier == Tier.D1_FBS
    assert r.hard_metrics_passed is True
    assert [t.value for t in r.qualifying_tiers] == ["D1_FBS", "D1_FCS", "D2_D3_NAIA_JUCO"]


def test_dl_falls_back_to_fcs_when_forty_misses_fbs():
    # 5.15s clears D1_FCS (4.90-5.20) but misses D1_FBS (4.80-5.10)
    r = run_sieve(Position.DL, height_in=75, weight_lbs=255, forty_s=5.15, bench_lbs=360)
    assert r.tier == Tier.D1_FCS
    assert r.hard_metrics_passed is True
    assert "D1_FBS" not in [t.value for t in r.qualifying_tiers]
    assert [t.value for t in r.qualifying_tiers] == ["D1_FCS", "D2_D3_NAIA_JUCO"]


def test_below_every_bracket_with_elite_laser_metrics_is_game_changer():
    # Below DB's lowest bracket (D2_D3_NAIA_JUCO: height >= 68, weight >= 165)
    # on frame, but laser metrics clear the top bracket outright.
    r = run_sieve(Position.DB, height_in=66, weight_lbs=155, forty_s=4.38, shuttle_s=4.10)
    assert r.is_game_changer is True
    assert r.hard_metrics_passed is False
    assert r.game_changer_reason is not None
    assert [t.value for t in r.qualifying_tiers] == ["D1_FBS"]


def test_fits_lowest_bracket_is_not_a_game_changer():
    # Same laser metrics as above, but frame now fits D2_D3_NAIA_JUCO for
    # real — a legitimate bottom-bracket tier now exists, so no exception
    # flag is needed.
    r = run_sieve(Position.DB, height_in=68, weight_lbs=165, forty_s=4.38, shuttle_s=4.10)
    assert r.is_game_changer is False
    assert r.hard_metrics_passed is True
    assert r.tier == Tier.D2_D3_NAIA_JUCO


def test_no_data_is_unranked():
    r = run_sieve(Position.QB)
    assert r.tier == Tier.UNRANKED
    assert r.hard_metrics_passed is False
    assert r.qualifying_tiers == []


def test_ol_lower_tier_has_no_forty_check():
    # OL's D1_FCS_D2 bracket has no forty threshold in the source data
    r = run_sieve(Position.OL, height_in=75, weight_lbs=290)
    assert r.tier == Tier.D1_FCS_D2
    assert r.hard_metrics_passed is True
    assert not any(c.metric == "forty_yard_dash" for c in r.checks)


def test_every_position_has_a_working_top_tier():
    """Smoke test: every position in the matrix produces D1_FBS for a
    plainly elite, dead-center-of-range athlete at that position."""
    from app.services.metric_sieve import POSITION_MATRIX

    for position, tier in POSITION_MATRIX:
        if tier != "D1_FBS":
            continue
        m = POSITION_MATRIX[(position, tier)]
        kwargs = {}
        if "height" in m:
            kwargs["height_in"] = sum(m["height"]) / 2
        if "weight" in m:
            kwargs["weight_lbs"] = sum(m["weight"]) / 2
        if "forty" in m:
            kwargs["forty_s"] = sum(m["forty"]) / 2
        if "shuttle_max" in m:
            kwargs["shuttle_s"] = m["shuttle_max"] - 0.01
        if "bench_min" in m:
            kwargs["bench_lbs"] = m["bench_min"] + 10
        if "squat_min" in m:
            kwargs["squat_lbs"] = m["squat_min"] + 10
        if "gpa_min" in m:
            kwargs["gpa"] = m["gpa_min"] + 0.1
        if "sat_min" in m:
            kwargs["sat"] = m["sat_min"] + 10

        r = run_sieve(Position(position), **kwargs)
        assert r.tier == Tier.D1_FBS, f"{position} should clear D1_FBS with mid-range values, got {r.tier}"
        assert r.hard_metrics_passed is True
