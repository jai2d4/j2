from backend.grading.confidence import analysis_confidence
from backend.grading.engine import TruGradeFilmEngine
from backend.grading.models import GradingEvent, ObservationValue, ProspectProfile
from backend.grading.profile_engine import build_profile
from backend.grading.rules_loader import load_position_rules, normalize_position


def event(trait: str, value: float, observation=ObservationValue.POSITIVE_EXECUTION):
    return GradingEvent(rule_id="TEST", trait=trait, value=value, confidence=.8,
                        timestamp=1, play_id="P1", reason="verified test", observation=observation)


def test_all_position_rules_load():
    for position in ("QB", "RB", "WR", "H", "Y", "OT", "IOL", "DT", "DE", "JACK", "LB", "SAFETY", "CB"):
        assert load_position_rules(position).traits


def test_position_mapping():
    assert normalize_position("db") == "CB"
    assert normalize_position("DL") == "DT"
    assert normalize_position("OL") == "IOL"
    assert normalize_position("TE") == "Y"


def test_score_aggregation_and_unknown_exclusion():
    result = TruGradeFilmEngine().grade("LB", [event("read_react", 10), event("read_react", 99, ObservationValue.UNKNOWN)])
    assert result.traits["read_react"].score == 60
    assert result.traits["read_react"].sample_count == 1
    assert result.grade == 60
    assert "instincts" in result.unknown_traits


def test_all_unknown_produces_no_grade():
    result = TruGradeFilmEngine().grade("LB", [event("read_react", 10, ObservationValue.UNKNOWN)])
    assert result.grade is None
    assert result.confidence == 0


def test_confidence_is_separate_and_bounded():
    value = analysis_confidence(film_quality=.9, view_angle=.8, player_identity=.7,
                                tracking=.6, field_calibration=.5, relevant_snaps=10,
                                ai_observation=.4)
    assert 0 <= value <= 1


def test_character_unavailable_without_verified_source():
    profile = {item.category: item for item in build_profile({"SIZE": (ProspectProfile.WIN, "verified roster")})}
    assert profile["SIZE"].available
    assert not profile["CHARACTER"].available
