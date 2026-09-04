"""Module 6 — Profile & Makeup averaging and grade-down, direct unit tests."""
from app.models.schemas import MakeupGrades, Rank
from app.services.makeup_grade import average_makeup_grade, grade_down


def test_average_rounds_to_nearest_rank():
    g = MakeupGrades(size=Rank.WIN, athletic_ability=Rank.WIN_MINUS, play_history=Rank.WIN,
                      play_style=Rank.WIN_PLUS, character=Rank.ALL_CONF)
    assert average_makeup_grade(g) == Rank.WIN


def test_average_ignores_ungraded_categories():
    g = MakeupGrades(size=Rank.GAME_CHANGER)
    assert average_makeup_grade(g) == Rank.GAME_CHANGER


def test_average_returns_none_when_nothing_graded():
    assert average_makeup_grade(MakeupGrades()) is None


def test_grade_down_shifts_one_rank_per_level():
    gd = grade_down(Rank.WIN_MINUS)
    assert gd.p4 == Rank.WIN_MINUS
    assert gd.group_of_5 == Rank.WIN
    assert gd.fcs == Rank.WIN_PLUS
    assert gd.d2_d3_naia_juco == Rank.ALL_CONF


def test_grade_down_caps_at_game_changer():
    gd = grade_down(Rank.ALL_CONF)
    assert gd.group_of_5 == Rank.GAME_CHANGER
    assert gd.fcs == Rank.GAME_CHANGER
    assert gd.d2_d3_naia_juco == Rank.GAME_CHANGER

    gd_top = grade_down(Rank.GAME_CHANGER)
    assert gd_top.p4 == gd_top.group_of_5 == gd_top.fcs == gd_top.d2_d3_naia_juco == Rank.GAME_CHANGER
