"""Two-stage ByteTrack-style association behind a replaceable tracker API."""
from __future__ import annotations

from dataclasses import dataclass, field
from math import hypot


def _iou(a: list[float], b: list[float]) -> float:
    x1, y1, x2, y2 = max(a[0], b[0]), max(a[1], b[1]), min(a[2], b[2]), min(a[3], b[3])
    intersection = max(0, x2 - x1) * max(0, y2 - y1)
    union = max(0, a[2] - a[0]) * max(0, a[3] - a[1]) + max(0, b[2] - b[0]) * max(0, b[3] - b[1]) - intersection
    return intersection / union if union else 0.0


@dataclass
class _Track:
    track_id: int
    bbox: list[float]
    last_frame: int
    history: list[dict] = field(default_factory=list)


class ByteTrackTracker:
    """Associates high-confidence detections first, then recovers with low scores."""

    def __init__(self, high_threshold: float = 0.5, low_threshold: float = 0.1,
                 match_iou: float = 0.3, max_lost_frames: int = 30):
        self.high_threshold = high_threshold
        self.low_threshold = low_threshold
        self.match_iou = match_iou
        self.max_lost_frames = max_lost_frames
        self._tracks: dict[int, _Track] = {}
        self._next_id = 1

    def _associate(self, detections: list[dict], frame: int, timestamp_ms: int, claimed: set[int]) -> list[dict]:
        output = []
        for detection in detections:
            candidates = [(track_id, _iou(track.bbox, detection["bbox"])) for track_id, track in self._tracks.items()
                          if track_id not in claimed and frame - track.last_frame <= self.max_lost_frames]
            track_id, score = max(candidates, key=lambda item: item[1], default=(None, 0))
            if track_id is None or score < self.match_iou:
                track_id = self._next_id
                self._next_id += 1
                self._tracks[track_id] = _Track(track_id, detection["bbox"], frame)
            track = self._tracks[track_id]
            old_x = (track.bbox[0] + track.bbox[2]) / 2
            old_y = (track.bbox[1] + track.bbox[3]) / 2
            center_x = (detection["bbox"][0] + detection["bbox"][2]) / 2
            center_y = (detection["bbox"][1] + detection["bbox"][3]) / 2
            elapsed = max((timestamp_ms - track.history[-1]["timestamp_ms"]) / 1000, 1e-6) if track.history else 0
            vx, vy = ((center_x - old_x) / elapsed, (center_y - old_y) / elapsed) if elapsed else (0.0, 0.0)
            point = {"frame": frame, "timestamp_ms": timestamp_ms, "center_x": round(center_x, 2),
                     "center_y": round(center_y, 2), "bbox": detection["bbox"],
                     "velocity_x": round(vx, 2), "velocity_y": round(vy, 2),
                     "speed_px": round(hypot(vx, vy), 2), "direction": round(__import__("math").degrees(__import__("math").atan2(vy, vx)), 2),
                     "confidence": detection["confidence"]}
            track.bbox, track.last_frame = detection["bbox"], frame
            track.history.append(point)
            claimed.add(track_id)
            output.append({"track_id": track_id, **point})
        return output

    def update(self, detection_frame: dict) -> list[dict]:
        players = [item for item in detection_frame["detections"] if item["class"] == "player" and item["confidence"] >= self.low_threshold]
        high = [item for item in players if item["confidence"] >= self.high_threshold]
        low = [item for item in players if item["confidence"] < self.high_threshold]
        claimed: set[int] = set()
        return self._associate(high, detection_frame["frame"], detection_frame["timestamp_ms"], claimed) + self._associate(low, detection_frame["frame"], detection_frame["timestamp_ms"], claimed)

    def export_tracks(self) -> list[dict]:
        return [{"track_id": track.track_id, "frames": [p["frame"] for p in track.history],
                 "start_time": track.history[0]["timestamp_ms"] if track.history else None,
                 "end_time": track.history[-1]["timestamp_ms"] if track.history else None,
                 "positions": track.history} for track in self._tracks.values()]

