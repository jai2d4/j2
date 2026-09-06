"""Movement metrics derived only from tracked coordinates."""
from __future__ import annotations

from math import hypot


def movement_metrics(positions: list[dict]) -> dict:
    if len(positions) < 2:
        return {"value": "unknown", "confidence": 0.0, "reason": "Insufficient tracked points."}
    speeds = [float(point.get("speed_px", 0)) for point in positions]
    directions = [float(point.get("direction", 0)) for point in positions]
    accelerations = []
    for before, after in zip(positions, positions[1:]):
        elapsed = (after["timestamp_ms"] - before["timestamp_ms"]) / 1000
        if elapsed > 0:
            accelerations.append((float(after.get("speed_px", 0)) - float(before.get("speed_px", 0))) / elapsed)
    displacement = hypot(positions[-1]["center_x"] - positions[0]["center_x"],
                         positions[-1]["center_y"] - positions[0]["center_y"])
    direction_change = max(directions) - min(directions) if directions else 0
    return {"max_speed_px": round(max(speeds), 2),
            "max_acceleration_px_s2": round(max(accelerations, default=0), 2),
            "max_deceleration_px_s2": round(min(accelerations, default=0), 2),
            "direction_change_deg": round(direction_change, 2),
            "displacement_px": round(displacement, 2),
            "confidence": round(sum(float(p.get("confidence", 0)) for p in positions) / len(positions), 4)}
