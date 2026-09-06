"""Frame-manifest to detection/track JSON pipeline."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Callable

from .detector import FootballDetector
from .tracker import ByteTrackTracker


def analyze_frames(video_id: str, frames: list[dict], output_root: Path,
                   progress: Callable[[int], None] | None = None) -> tuple[list[dict], list[dict]]:
    import cv2

    detector, tracker = FootballDetector(), ByteTrackTracker()
    detections = []
    total = max(len(frames), 1)
    for index, metadata in enumerate(frames):
        image = cv2.imread(metadata["file_path"])
        if image is None:
            continue
        result = detector.detect_frame(image, metadata["frame_number"], metadata["timestamp_ms"])
        detections.append(result)
        tracker.update(result)
        if progress:
            progress(int((index + 1) * 100 / total))
    tracks = tracker.export_tracks()
    target = Path(output_root) / video_id
    target.mkdir(parents=True, exist_ok=True)
    (target / "detections.json").write_text(json.dumps(detections, indent=2), encoding="utf-8")
    (target / "tracks.json").write_text(json.dumps(tracks, indent=2), encoding="utf-8")
    return detections, tracks

