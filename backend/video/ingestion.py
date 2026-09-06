"""Chunked, validated local video storage."""
from __future__ import annotations

import json
import re
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4

from fastapi import HTTPException, UploadFile


ALLOWED_EXTENSIONS = {".mp4", ".mov", ".avi", ".mkv"}


class VideoStore:
    def __init__(self, root: Path, max_upload_mb: int = 500):
        self.root = Path(root)
        self.video_dir = self.root / "videos"
        self.metadata_dir = self.root / "metadata"
        self.video_dir.mkdir(parents=True, exist_ok=True)
        self.metadata_dir.mkdir(parents=True, exist_ok=True)
        self.max_bytes = max_upload_mb * 1024 * 1024

    async def save(self, upload: UploadFile) -> dict:
        original = Path(upload.filename or "").name
        suffix = Path(original).suffix.lower()
        if suffix not in ALLOWED_EXTENSIONS:
            raise HTTPException(400, "Video must be MP4, MOV, AVI, or MKV.")
        video_id = str(uuid4())
        safe_stem = re.sub(r"[^A-Za-z0-9._-]+", "_", Path(original).stem).strip("._") or "film"
        target = self.video_dir / f"{video_id}-{safe_stem}{suffix}"
        size = 0
        try:
            with target.open("wb") as output:
                while chunk := await upload.read(1024 * 1024):
                    size += len(chunk)
                    if size > self.max_bytes:
                        raise HTTPException(413, "File exceeds upload limit.")
                    output.write(chunk)
        except Exception:
            target.unlink(missing_ok=True)
            raise
        metadata = {
            "video_id": video_id, "filename": original, "status": "uploaded",
            "size_bytes": size, "file_path": str(target),
            "created_at": datetime.now(timezone.utc).isoformat(),
        }
        self.write_metadata(video_id, metadata)
        return metadata

    def write_metadata(self, video_id: str, metadata: dict) -> None:
        (self.metadata_dir / f"{video_id}.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")

    def get(self, video_id: str) -> dict | None:
        path = self.metadata_dir / f"{video_id}.json"
        return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else None

    def list(self) -> list[dict]:
        return [json.loads(path.read_text(encoding="utf-8")) for path in sorted(self.metadata_dir.glob("*.json"))]

