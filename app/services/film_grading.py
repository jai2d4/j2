"""
Module 1 companion — Position Specifics // Critical Factors.

The critical-factors checklist per position, straight from the P4 scouting
sheet. Gemini grades every factor it can actually observe on film against
the same Rank scale used for Profile & Makeup (Game-Changer -> NGE), and
only surfaces a flag when a factor lands at the extremes: GAME_CHANGER
(an elite, winning trait) or NGE (a real deficiency) — ordinary Win/Win+/
Win- grades are recorded but don't get flagged. Traits that aren't visible
on film (Character, Play History, etc.) are handled by Module 6 instead.
"""
from __future__ import annotations

from app.models.schemas import Position

CRITICAL_FACTORS: dict[str, list[str]] = {
    "QB": [
        "Accuracy", "Anticipation", "Pocket awareness / mobility",
        "Reads defenses pre- and post-snap", "Release quickness / delivery",
        "Arm strength / throw off platform", "Ball security",
        "Clutch / red-zone / 3rd-down production", "Toughness",
    ],
    "RB": [
        "Vision", "Contact balance", "Elusiveness / make-miss",
        "Burst / acceleration", "Ball security", "Pass-catching ability",
        "Balance / fall-forward", "Durability", "Toughness",
    ],
    "WR": [
        "Release / separation", "Route-running quickness",
        "Ball skills / contested catch", "Yards after catch",
        "Awareness vs. coverage", "Contact balance", "Point-of-attack blocking",
        "Toughness",
    ],
    "TE": [
        "Size & length", "Point-of-attack blocking", "Hands / ball skills in traffic",
        "Route flexibility", "Athletic ability at the second level", "Toughness",
    ],
    "OL": [
        "Size & length", "Pass-pro quickness / kick-slide", "Hand placement / punch",
        "Ability to pull and block in space", "Anchor / balance vs. bull-rush",
        "Recognition of fronts and stunts", "Toughness",
    ],
    "DL": [
        "Get-off / 1st step", "Bend / flex", "Strike / shed ability",
        "Balance / base", "Lateral quickness / change of direction",
        "Pursuit / motor / effort", "Rush power", "Reactionary athletic ability",
        "Toughness",
    ],
    "DE": [
        "Get-off / 1st step", "Bend / flex around the edge", "Heavy hands",
        "Rush power / speed", "Lateral quickness", "Range / speed-to-close",
        "Tackling in space", "Coverage / drop ability", "Blitz / rush ability",
        "Reactionary athletic ability", "Toughness",
    ],
    "LB": [
        "Instincts / football IQ", "Read and react", "Play speed",
        "Burst / acceleration", "Tackling in space", "Coverage ability",
        "Blitz / rush ability", "Teams value", "Reactionary athletic ability",
        "Toughness",
    ],
    "DB": [
        "Play speed", "Recovery speed / range", "Man coverage (mirror / trail / press)",
        "Zone / off-coverage awareness", "Ball skills / judgment",
        "Space athletic ability / balance", "Tackling in space",
        "Position / plant & drive", "Urgency", "Reactionary athletic ability",
        "Teams value", "Toughness",
    ],
}

RANK_SCALE_PROMPT = (
    "GAME_CHANGER (elite, a winning trait), ALL_CONF, WIN_PLUS, WIN, WIN_MINUS, "
    "or NGE (a real deficiency)"
)


def build_scouting_prompt(position: Position) -> str:
    factors = CRITICAL_FACTORS[position.value]
    factor_list = "\n".join(f"    - {f}" for f in factors)
    return f"""
    You are an elite collegiate personnel director evaluating a prospect's film clip.
    The athlete plays the position of: {position.value}.

    Grade the athlete on each of the following position-specific critical factors,
    using ONLY what you can actually observe on this film. If a factor cannot be
    judged from this clip, omit it rather than guessing.
{factor_list}

    For each factor you can assess, assign a rank from this scale (best to worst):
    {RANK_SCALE_PROMPT}

    Return ONLY a JSON object with exactly these keys:
    1. film_grades: an object mapping each assessed factor name to its rank.
    2. flags: an array of short strings — include an entry ONLY when a factor
       graded GAME_CHANGER (call out the elite trait) or NGE (call out the
       deficiency). Do NOT add a flag for ordinary WIN_PLUS / WIN / WIN_MINUS
       grades. Return an empty array if nothing hit either extreme.
    No prose outside the JSON object.
    """
