"""Local evidence persistence linking reports back to exact film."""
from __future__ import annotations

import json
from pathlib import Path

from backend.grading.models import Evidence


class EvidenceStore:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def save(self, evidence: Evidence) -> None:
        path = self.root / f"{evidence.player_id or 'unknown'}.json"
        records = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else []
        records.append(evidence.model_dump())
        path.write_text(json.dumps(records, indent=2), encoding="utf-8")

    def for_player(self, player_id: str) -> list[dict]:
        path = self.root / f"{player_id}.json"
        return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else []

