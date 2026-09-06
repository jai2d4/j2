"""Phase 3 video upload/list API."""
from __future__ import annotations

import os
from pathlib import Path

from fastapi import APIRouter, File, UploadFile

from backend.video.ingestion import VideoStore


router = APIRouter(prefix="/api/videos", tags=["videos"])
storage_root = Path(os.getenv("TRUGRADE_STORAGE_DIR", Path(__file__).parents[1] / "storage"))
video_store = VideoStore(storage_root, int(os.getenv("MAX_UPLOAD_MB", "500")))


@router.post("/upload", status_code=201)
async def upload_video(file: UploadFile = File(...)):
    metadata = await video_store.save(file)
    return {key: metadata[key] for key in ("video_id", "filename", "status")}


@router.get("")
async def list_videos():
    return {"videos": video_store.list()}

