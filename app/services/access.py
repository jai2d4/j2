"""The paywall.

Every decision about whether a viewer may see something lives here, as pure
functions over plain values. The routers do the database work and then ask these
functions the question, so the rules can be tested without a database and cannot
drift between the feed, the profile, and a single-post fetch.

Tiers are ranked by price. Subscribing at $25 unlocks everything gated at $10.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Access:
    granted: bool
    # Why it was granted (owner/public/subscription/purchase/ticket) or why it is locked.
    reason: str
    unlock_label: str | None = None


def money(cents: int) -> str:
    return f"${cents / 100:,.2f}".replace(".00", "")


def decide_post_access(
    *,
    is_owner: bool,
    visibility: str,
    price_cents: int = 0,
    min_tier_price_cents: int | None = None,
    viewer_tier_price_cents: int | None = None,
    has_unlock: bool = False,
) -> Access:
    """`viewer_tier_price_cents` is None when the viewer has no active subscription."""
    if is_owner:
        return Access(True, "owner")
    if visibility == "public":
        return Access(True, "public")

    if visibility == "subscribers":
        if viewer_tier_price_cents is None:
            return Access(False, "subscribers_only", "Subscribe to unlock")
        required = min_tier_price_cents or 0
        if viewer_tier_price_cents >= required:
            return Access(True, "subscription")
        return Access(False, "tier_too_low", f"Upgrade to {money(required)}/mo to unlock")

    if visibility == "ppv":
        if has_unlock:
            return Access(True, "purchase")
        return Access(False, "locked", f"Unlock for {money(price_cents)}")

    # Unknown visibility fails closed rather than leaking the post.
    return Access(False, "locked", None)


def decide_live_access(
    *,
    is_owner: bool,
    access: str,
    ticket_price_cents: int = 0,
    min_tier_price_cents: int | None = None,
    viewer_tier_price_cents: int | None = None,
    has_ticket: bool = False,
) -> Access:
    if is_owner:
        return Access(True, "owner")
    if access == "free":
        return Access(True, "public")

    if access == "subscribers":
        if viewer_tier_price_cents is None:
            return Access(False, "subscribers_only", "Subscribe to watch")
        required = min_tier_price_cents or 0
        if viewer_tier_price_cents >= required:
            return Access(True, "subscription")
        return Access(False, "tier_too_low", f"Upgrade to {money(required)}/mo to watch")

    if access == "ticket":
        if has_ticket:
            return Access(True, "ticket")
        return Access(False, "ticket_required", f"Get a ticket — {money(ticket_price_cents)}")

    return Access(False, "locked", None)
