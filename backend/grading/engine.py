"""Official deterministic TruGrade film engine.

External AI may supply validated observations, but this module alone calculates
official trait and position grades.
"""
from __future__ import annotations

from collections import defaultdict

from .confidence import weighted_confidence
from .models import GradingEvent, ObservationValue, PositionGrade, TraitScore
from .rules_loader import load_position_rules


class TruGradeFilmEngine:
    BASE_SCORE = 50.0

    def grade(self, position: str, events: list[GradingEvent]) -> PositionGrade:
        rules = load_position_rules(position)
        grouped: dict[str, list[GradingEvent]] = defaultdict(list)
        for event in events:
            if event.observation != ObservationValue.UNKNOWN and event.trait in rules.traits:
                grouped[event.trait].append(event)

        trait_results: dict[str, TraitScore | None] = {}
        unknown_traits: list[str] = []
        for trait, rule in rules.traits.items():
            if not rule.enabled:
                continue
            samples = grouped.get(trait, [])
            if not samples:
                trait_results[trait] = None
                unknown_traits.append(trait)
                continue
            score = min(100.0, max(0.0, self.BASE_SCORE + sum(item.value for item in samples)))
            confidence = weighted_confidence([(item.confidence, 1.0) for item in samples])
            trait_results[trait] = TraitScore(
                trait=trait, score=round(score, 2), confidence=confidence,
                sample_count=len(samples),
                evidence=[item.evidence for item in samples if item.evidence is not None],
            )

        scored = [(trait_results[name], rule.weight) for name, rule in rules.traits.items()
                  if rule.enabled and trait_results.get(name) is not None]
        if not scored:
            return PositionGrade(position=rules.position, grade=None, confidence=0, traits=trait_results,
                                 unknown_traits=unknown_traits)
        total_weight = sum(weight for _, weight in scored)
        grade = sum(result.score * weight for result, weight in scored) / total_weight
        confidence = weighted_confidence([(result.confidence, weight) for result, weight in scored])
        return PositionGrade(position=rules.position, grade=round(grade, 2), confidence=confidence,
                             traits=trait_results, unknown_traits=unknown_traits)

