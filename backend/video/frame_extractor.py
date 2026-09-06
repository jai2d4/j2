"""OpenCV frame extraction retaining original frame numbers and timestamps."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Callable


class FrameExtractionError(RuntimeError):
    pass


class FrameExtractor:
    def __init__(self, frames_root: Path, analysis_fps: float = 10.0):
        if analysis_fps <= 0:
            raise ValueError("analysis_fps must be positive")
        self.frames_root = Path(frames_root)
        self.analysis_fps = analysis_fps

    def extract(self, video_id: str, video_path: Path, progress: Callable[[int], None] | None = None) -> list[dict]:
        import cv2

        output_dir = self.frames_root / video_id
        output_dir.mkdir(parents=True, exist_ok=True)
        capture = cv2.VideoCapture(str(video_path))
        if not capture.isOpened():
            raise FrameExtractionError("OpenCV could not open the uploaded video.")
        source_fps = float(capture.get(cv2.CAP_PROP_FPS) or 0)
        total_frames = int(capture.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
        if source_fps <= 0:
            capture.release()
            raise FrameExtractionError("Video frame rate could not be determined.")
        interval = max(source_fps / self.analysis_fps, 1.0)
        next_sample = 0.0
        frame_number = 0
        frames: list[dict] = []
        try:
            while True:
                ok, frame = capture.read()
                if not ok:
                    break
                if frame_number + 1e-9 >= next_sample:
                    height, width = frame.shape[:2]
                    file_path = output_dir / f"frame-{frame_number:09d}.jpg"
                    if not cv2.imwrite(str(file_path), frame):
                        raise FrameExtractionError(f"Could not write frame {frame_number}.")
                    frames.append({
                        "frame_number": frame_number,
                        "timestamp_ms": round(frame_number * 1000 / source_fps),
                        "video_id": video_id, "width": width, "height": height,
                        "file_path": str(file_path),
                    })
                    next_sample += interval
                frame_number += 1
                if progress and total_frames:
                    progress(min(99, int(frame_number * 100 / total_frames)))
        finally:
            capture.release()
        manifest = output_dir / "frames.json"
        manifest.write_text(json.dumps(frames, indent=2), encoding="utf-8")
        if progress:
            progress(100)
        return frames

