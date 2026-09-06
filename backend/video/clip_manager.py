"""Reserved clip-management boundary for later play/evidence phases."""
from pathlib import Path


class ClipManager:
    def __init__(self, root: Path):
        self.root = Path(root)

