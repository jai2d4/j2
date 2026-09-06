"""Conservative activity-gap play segmentation with manual correction support."""
from __future__ import annotations

from pydantic import BaseModel, Field, model_validator


class Play(BaseModel):
    play_id: str
    video_id: str
    start_time: float = Field(ge=0)
    snap_time: float = Field(ge=0)
    end_time: float = Field(ge=0)
    confidence: float = Field(ge=0, le=1)
    excluded: bool = False
    source: str = "automatic"

    @model_validator(mode="after")
    def ordered_times(self):
        if not self.start_time <= self.snap_time <= self.end_time:
            raise ValueError("Play times must satisfy start <= snap <= end")
        return self


class PlaySegmenter:
    def __init__(self, gap_ms: int = 4000):
        self.gap_ms = gap_ms

    def segment(self, video_id: str, tracks: list[dict]) -> list[Play]:
        timestamps = sorted({point["timestamp_ms"] for track in tracks for point in track.get("positions", [])})
        if not timestamps:
            return []
        groups: list[list[int]] = [[timestamps[0]]]
        for timestamp in timestamps[1:]:
            if timestamp - groups[-1][-1] > self.gap_ms:
                groups.append([])
            groups[-1].append(timestamp)
        return [Play(play_id=f"P{index:03d}", video_id=video_id,
                     start_time=max(0, group[0] / 1000 - 1), snap_time=group[0] / 1000,
                     end_time=group[-1] / 1000 + 1, confidence=.45)
                for index, group in enumerate(groups, 1)]


def correct_play(play: Play, *, start_time: float, snap_time: float, end_time: float,
                 excluded: bool, reason: str) -> tuple[Play, dict]:
    corrected = play.model_copy(update={"start_time": start_time, "snap_time": snap_time,
                                        "end_time": end_time, "excluded": excluded,
                                        "confidence": 1.0, "source": "human"})
    corrected = Play.model_validate(corrected.model_dump())
    return corrected, {"original_value": play.model_dump(), "new_value": corrected.model_dump(),
                       "reason": reason, "source": "human"}
