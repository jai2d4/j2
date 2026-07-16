"""
Profile & Makeup grading — Modules 2/3 companion.

Scouts grade Size, Athletic Ability, Play History, Play Style, and Character
against the P4 rubric (the toughest standard). Those five grades average into
one overall P4-standard grade, which is then "graded down" per classification
level: the same traits read as a better grade against weaker competition.
"""
from __future__ import annotations

from typing import Optional

from app.models.schemas import GradeDown, MakeupGrades, Rank, RANK_ORDER

# Index 0 = GAME_CHANGER (best) ... index 5 = NGE (worst)
_RANK_TO_INDEX = {rank: i for i, rank in enumerate(RANK_ORDER)}

# How many ranks better the same traits grade at each level below P4.
_LEVEL_SHIFT = {
    "p4": 0,
    "group_of_5": 1,
    "fcs": 2,
    "d2_d3_naia_juco": 3,
}


def _shift(rank: Rank, steps: int) -> Rank:
    idx = max(0, _RANK_TO_INDEX[rank] - steps)
    return RANK_ORDER[idx]


def average_makeup_grade(grades: MakeupGrades) -> Optional[Rank]:
    """Average the provided category grades; ignores ungraded (None) categories."""
    provided = [g for g in grades.model_dump().values() if g is not None]
    if not provided:
        return None
    indices = [_RANK_TO_INDEX[Rank(g)] for g in provided]
    avg_index = round(sum(indices) / len(indices))
    return RANK_ORDER[avg_index]


def grade_down(overall: Rank) -> GradeDown:
    """Shift the overall P4-standard grade down for each easier classification level."""
    return GradeDown(
        overall=overall,
        p4=_shift(overall, _LEVEL_SHIFT["p4"]),
        group_of_5=_shift(overall, _LEVEL_SHIFT["group_of_5"]),
        fcs=_shift(overall, _LEVEL_SHIFT["fcs"]),
        d2_d3_naia_juco=_shift(overall, _LEVEL_SHIFT["d2_d3_naia_juco"]),
    )
