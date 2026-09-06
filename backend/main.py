"""Stable Phase 1 backend entrypoint.

The existing FastAPI application remains the source of truth during the safe
restructure. Later phases can move routers behind this entrypoint without
changing the Windows launcher or public server command.
"""

from app.main import app
from backend.api.analysis import router as analysis_router
from backend.api.videos import router as videos_router

app.include_router(videos_router)
app.include_router(analysis_router)

__all__ = ["app"]
