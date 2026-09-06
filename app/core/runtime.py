"""Process-local runtime state the control plane can flip.

Deliberately in-memory: a maintenance switch has to work when the database is
the thing that is broken. In a multi-instance deployment Axiome flips each
instance, or this moves to a shared store.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone

VERSION = "0.1.0"


@dataclass
class Runtime:
    started_at: datetime
    maintenance: bool = False
    maintenance_message: str = "Livephoria is briefly down for maintenance."

    @property
    def uptime_seconds(self) -> int:
        return int((datetime.now(timezone.utc) - self.started_at).total_seconds())


runtime = Runtime(started_at=datetime.now(timezone.utc))
