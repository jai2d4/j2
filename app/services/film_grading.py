"""Film grading prompt for the five universal TruGrade football metrics."""
from __future__ import annotations

from typing import Optional

from app.models.schemas import Position

FILM_METRICS = ["Field Speed", "Contact", "Play Recognition", "Tackling", "Versatility"]

RANK_SCALE_PROMPT = (
    "GAME_CHANGER (elite, a winning trait), ALL_CONF, WIN_PLUS, WIN, WIN_MINUS, "
    "or NGE (a real deficiency)"
)


def build_scouting_prompt(position: Position, player_identifier: Optional[str] = None) -> str:
    factor_list = "\n".join(f"    - {f}" for f in FILM_METRICS)

    if player_identifier:
        target_block = f"""
    IMPORTANT — this film may show multiple players (a highlight reel, a full
    game, a scrimmage). You are evaluating ONE specific player, identified as:
    "{player_identifier}"

    Find and track ONLY this player across every rep they appear in. Ignore
    every other player on the field, even during exciting or highlight-worthy
    plays that belong to someone else. If you cannot confidently locate this
    player anywhere in the clip, do not substitute a different player or
    guess — say so in identification_note and return empty film_grades and
    flags instead of grading the wrong person.
    """
    else:
        target_block = """
    This film is assumed to be focused on a single prospect throughout. If it
    actually shows multiple players and it's unclear who the subject is, say
    so in identification_note rather than blending grades across players.
    """

    return f"""
    You are an elite collegiate personnel director evaluating a prospect's film clip.
{target_block}
    Grade the athlete on each of the following universal football film metrics,
    using ONLY what you can actually observe on this film. If a factor cannot be
    judged from this clip, omit it rather than guessing.
{factor_list}

    For each factor you can assess, assign a rank from this scale (best to worst):
    {RANK_SCALE_PROMPT}

    Return ONLY a JSON object with exactly these keys:
    1. player_identified: true or false — whether you could confidently locate
       and track the correct player throughout the film.
    2. identification_note: one short sentence — how you identified the player
       (e.g. jersey number/color, position alignment) or why you could not.
    3. film_grades: an object mapping each assessed factor name to its rank.
       Leave this empty if player_identified is false.
    4. flags: an array of short strings — include an entry ONLY when a factor
       graded GAME_CHANGER (call out the elite trait) or NGE (call out the
       deficiency). Do NOT add a flag for ordinary WIN_PLUS / WIN / WIN_MINUS
       grades. Return an empty array if nothing hit either extreme, or if
       player_identified is false.
    No prose outside the JSON object.
    """
