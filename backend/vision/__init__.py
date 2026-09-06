"""Swappable computer-vision components for TruGrade."""

from .detector import FootballDetector
from .tracker import ByteTrackTracker

__all__ = ["FootballDetector", "ByteTrackTracker"]
