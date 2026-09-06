"""Provider-neutral AI observation layer."""

from .football_reasoner import FootballReasoner
from .provider import create_provider

__all__ = ["FootballReasoner", "create_provider"]
