"""The paywall rules, tested without a database."""
from app.services.access import decide_live_access, decide_post_access
from app.services.payments import split_revenue


def test_public_post_is_open_to_a_stranger():
    access = decide_post_access(is_owner=False, visibility="public")
    assert access.granted and access.reason == "public"


def test_subscriber_post_is_locked_without_a_subscription():
    access = decide_post_access(is_owner=False, visibility="subscribers")
    assert not access.granted
    assert access.reason == "subscribers_only"
    assert access.unlock_label == "Subscribe to unlock"


def test_a_higher_tier_unlocks_a_lower_gate():
    access = decide_post_access(
        is_owner=False,
        visibility="subscribers",
        min_tier_price_cents=1000,
        viewer_tier_price_cents=2500,
    )
    assert access.granted and access.reason == "subscription"


def test_a_cheaper_tier_does_not_unlock_a_vip_gate():
    access = decide_post_access(
        is_owner=False,
        visibility="subscribers",
        min_tier_price_cents=2500,
        viewer_tier_price_cents=500,
    )
    assert not access.granted
    assert access.reason == "tier_too_low"
    assert "Upgrade to $25" in access.unlock_label


def test_ppv_needs_a_purchase_and_shows_the_price():
    locked = decide_post_access(is_owner=False, visibility="ppv", price_cents=1200)
    assert not locked.granted and locked.unlock_label == "Unlock for $12"
    bought = decide_post_access(
        is_owner=False, visibility="ppv", price_cents=1200, has_unlock=True
    )
    assert bought.granted and bought.reason == "purchase"


def test_a_subscription_alone_does_not_unlock_a_ppv_post():
    access = decide_post_access(
        is_owner=False, visibility="ppv", price_cents=500, viewer_tier_price_cents=9999
    )
    assert not access.granted


def test_the_author_always_sees_their_own_work():
    for visibility in ("public", "subscribers", "ppv"):
        assert decide_post_access(is_owner=True, visibility=visibility).granted


def test_unknown_visibility_fails_closed():
    assert not decide_post_access(is_owner=False, visibility="something-new").granted


def test_ticketed_live_needs_a_ticket():
    locked = decide_live_access(is_owner=False, access="ticket", ticket_price_cents=2500)
    assert not locked.granted and "$25" in locked.unlock_label
    assert decide_live_access(
        is_owner=False, access="ticket", ticket_price_cents=2500, has_ticket=True
    ).granted


def test_fee_split_rounds_in_the_creators_favour():
    split = split_revenue(999, fee_bps=1000)
    assert (split.gross_cents, split.fee_cents, split.net_cents) == (999, 99, 900)
    assert split.fee_cents + split.net_cents == split.gross_cents


def test_a_free_transaction_splits_to_nothing():
    split = split_revenue(0)
    assert (split.fee_cents, split.net_cents) == (0, 0)
