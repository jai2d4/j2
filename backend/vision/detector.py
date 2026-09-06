"""Ultralytics YOLO adapter with stable TruGrade detection JSON."""
from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Iterable


class DetectorUnavailable(RuntimeError):
    pass


class FootballDetector:
    """Lazy YOLO adapter; callers do not depend on Ultralytics result objects."""

    def __init__(self, model_path: str | None = None, confidence: float = 0.25, model: Any = None):
        self.model_path = model_path or os.getenv("YOLO_MODEL_PATH", "yolo11n.pt")
        self.confidence = confidence
        self.model = model
        self.device = "cpu"

    def load_model(self) -> None:
        if self.model is not None:
            return
        try:
            import torch
            from ultralytics import YOLO
            self.device = "cuda" if torch.cuda.is_available() else "cpu"
            self.model = YOLO(self.model_path)
        except Exception as exc:
            raise DetectorUnavailable(f"YOLO could not be loaded: {exc}") from exc

    @staticmethod
    def _football_class(raw_name: str) -> str | None:
        name = raw_name.lower().strip()
        if name == "person" or name == "player":
            return "player"
        if name in {"sports ball", "football", "ball"}:
            return "football"
        if name in {"official", "referee"}:
            return "official"
        return None

    def detect_frame(self, frame: Any, frame_number: int, timestamp_ms: int) -> dict:
        self.load_model()
        result = self.model.predict(frame, conf=self.confidence, device=self.device, verbose=False)[0]
        names = result.names
        detections = []
        for box in result.boxes:
            class_id = int(box.cls.item())
            mapped = self._football_class(str(names[class_id]))
            if mapped is None:
                continue
            coords = [round(float(value), 2) for value in box.xyxy[0].tolist()]
            detections.append({"class": mapped, "confidence": round(float(box.conf.item()), 4), "bbox": coords})
        return {"frame": frame_number, "timestamp_ms": timestamp_ms, "detections": detections}

    def detect_batch(self, frames: Iterable[tuple[Any, int, int]]) -> list[dict]:
        return [self.detect_frame(image, number, timestamp) for image, number, timestamp in frames]

