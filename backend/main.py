"""Stable Phase 1 backend entrypoint.

The existing FastAPI application remains the source of truth during the safe
restructure. Later phases can move routers behind this entrypoint without
changing the Windows launcher or public server command.
"""

from app.main import app

__all__ = ["app"]
