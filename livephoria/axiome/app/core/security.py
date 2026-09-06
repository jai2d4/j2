"""Keys. Two kinds, pointing opposite ways.

  * app key   — an app presents this to Axiome. Axiome issues it, keeps only a
                hash, and shows the plain value once.
  * control key — Axiome presents this to an app. It has to be replayed, so it is
                stored as given. In production that belongs in a secret manager.
"""
from __future__ import annotations

import hashlib
import hmac
import secrets


def new_app_key() -> str:
    return "axk_" + secrets.token_urlsafe(32)


def hash_key(key: str) -> str:
    return hashlib.sha256(key.encode()).hexdigest()


def key_matches(presented: str, stored_hash: str) -> bool:
    if not stored_hash:
        return False
    return hmac.compare_digest(hash_key(presented), stored_hash)


def hint(key: str) -> str:
    """A few characters so a person can tell two keys apart in a list."""
    return key[:8] if len(key) > 8 else key
