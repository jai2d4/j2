import json
import asyncio
import sys
from io import BytesIO
from types import SimpleNamespace

import pytest
from fastapi import UploadFile

from backend.video.frame_extractor import FrameExtractor
from backend.video.ingestion import VideoStore


def test_chunked_video_ingestion_and_metadata(tmp_path):
    store = VideoStore(tmp_path, max_upload_mb=1)
    upload = UploadFile(filename="Game Film.MKV", file=BytesIO(b"football-film"))
    result = asyncio.run(store.save(upload))
    assert result["status"] == "uploaded"
    assert result["filename"] == "Game Film.MKV"
    assert store.get(result["video_id"])["size_bytes"] == 13
    assert len(store.list()) == 1


def test_rejects_unsupported_video(tmp_path):
    store = VideoStore(tmp_path)
    upload = UploadFile(filename="notes.txt", file=BytesIO(b"not video"))
    with pytest.raises(Exception) as error:
        asyncio.run(store.save(upload))
    assert error.value.status_code == 400


def test_frame_extraction_retains_frame_and_timestamp(monkeypatch, tmp_path):
    frames = [SimpleNamespace(shape=(720, 1280, 3)) for _ in range(6)]

    class Capture:
        def __init__(self, _): self.index = 0
        def isOpened(self): return True
        def get(self, prop): return 20 if prop == 1 else len(frames)
        def read(self):
            if self.index == len(frames): return False, None
            frame = frames[self.index]
            self.index += 1
            return True, frame
        def release(self): pass

    fake_cv2 = SimpleNamespace(
        CAP_PROP_FPS=1, CAP_PROP_FRAME_COUNT=2, VideoCapture=Capture,
        imwrite=lambda path, frame: True,
    )
    monkeypatch.setitem(sys.modules, "cv2", fake_cv2)
    result = FrameExtractor(tmp_path / "frames", analysis_fps=10).extract("video-1", tmp_path / "film.mp4")
    assert [item["frame_number"] for item in result] == [0, 2, 4]
    assert [item["timestamp_ms"] for item in result] == [0, 100, 200]
    assert result[0]["width"] == 1280
    assert json.loads((tmp_path / "frames" / "video-1" / "frames.json").read_text())[0]["video_id"] == "video-1"
