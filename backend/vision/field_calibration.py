"""Manual-first field calibration with explicit confidence."""
from __future__ import annotations

from pydantic import BaseModel, Field, model_validator


class KnownFieldPoint(BaseModel):
    pixel_x: float
    pixel_y: float
    x_yards: float
    y_yards: float


class FieldCalibration(BaseModel):
    points: list[KnownFieldPoint] = Field(min_length=2)
    confidence: float = Field(ge=0, le=1)
    method: str = "manual"

    @model_validator(mode="after")
    def distinct_points(self):
        first, last = self.points[0], self.points[-1]
        if first.pixel_x == last.pixel_x or first.pixel_y == last.pixel_y:
            raise ValueError("Calibration points must span both field axes")
        return self

    def pixel_to_field(self, pixel_x: float, pixel_y: float) -> tuple[float, float]:
        first, last = self.points[0], self.points[-1]
        x_scale = (last.x_yards - first.x_yards) / (last.pixel_x - first.pixel_x)
        y_scale = (last.y_yards - first.y_yards) / (last.pixel_y - first.pixel_y)
        return (
            round(first.x_yards + (pixel_x - first.pixel_x) * x_scale, 3),
            round(first.y_yards + (pixel_y - first.pixel_y) * y_scale, 3),
        )


def automatic_calibration_unavailable() -> dict:
    """Honest Phase 5 fallback until line/hash recognition is implemented."""
    return {"method": "automatic", "status": "needs_manual_points", "confidence": 0.0}

