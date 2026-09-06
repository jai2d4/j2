"""Wallet, tips, the platform's cut, the earnings dashboard, and getting paid out."""
from tests.conftest import CONTROL_KEY

KEY = {"X-Axiome-Key": CONTROL_KEY}


async def test_a_tip_splits_between_creator_and_platform(client, make_creator, make_user):
    creator, handle, _ = await make_creator("Jade")
    fan = await make_user()
    await fan.topup(5000)

    r = await client.post(
        f"/api/v1/creators/{handle}/tip",
        json={"amount_cents": 1000, "note": "loved the set"},
        headers=fan.headers,
    )
    assert r.status_code == 200, r.text
    assert r.json()["wallet_balance_cents"] == 4000

    # 10% default fee: the creator keeps 900 of the 1000.
    assert (await creator.wallet())["earnings_balance_cents"] == 900

    earnings = (await client.get("/api/v1/me/earnings", headers=creator.headers)).json()
    assert earnings["gross_cents"] == 1000
    assert earnings["fee_cents"] == 100
    assert earnings["net_cents"] == 900
    assert earnings["platform_fee_bps"] == 1000
    assert [line["kind"] for line in earnings["by_kind"]] == ["tip"]
    assert earnings["recent"][0]["note"] == "loved the set"


async def test_earnings_add_up_across_every_revenue_type(client, make_creator, make_user):
    creator, handle, tier_id = await make_creator("Marcus", price_cents=1000)
    r = await client.post(
        "/api/v1/posts",
        json={"title": "PPV", "body": "x", "visibility": "ppv", "price_cents": 2000},
        headers=creator.headers,
    )
    post_id = r.json()["id"]
    r = await client.post(
        "/api/v1/live",
        json={"title": "Listening party", "access": "ticket", "ticket_price_cents": 500},
        headers=creator.headers,
    )
    event_id = r.json()["id"]

    fan = await make_user()
    await fan.topup(10_000)
    await client.post(
        f"/api/v1/creators/{handle}/subscribe", json={"tier_id": tier_id}, headers=fan.headers
    )
    await client.post(f"/api/v1/posts/{post_id}/unlock", headers=fan.headers)
    await client.post(f"/api/v1/live/{event_id}/ticket", headers=fan.headers)
    await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 300}, headers=fan.headers
    )

    earnings = (await client.get("/api/v1/me/earnings", headers=creator.headers)).json()
    assert earnings["gross_cents"] == 1000 + 2000 + 500 + 300
    assert earnings["net_cents"] == 900 + 1800 + 450 + 270
    assert earnings["active_subscribers"] == 1
    assert {line["kind"] for line in earnings["by_kind"]} == {
        "subscription",
        "ppv",
        "ticket",
        "tip",
    }
    assert (await fan.wallet())["wallet_balance_cents"] == 10_000 - 3800


async def test_withdrawing_needs_identity_checks_then_a_balance(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    fan = await make_user()
    await fan.topup(5000)
    await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 2000}, headers=fan.headers
    )
    assert (await creator.wallet())["earnings_balance_cents"] == 1800

    # No payout account at all.
    r = await client.post(
        "/api/v1/me/earnings/payout", json={"amount_cents": 1000}, headers=creator.headers
    )
    assert r.status_code == 403
    assert "payout details" in r.json()["detail"]

    # Submitted, not yet reviewed.
    r = await client.post(
        "/api/v1/me/payout-account",
        json={"legal_name": "A Creator", "country": "us"},
        headers=creator.headers,
    )
    assert r.status_code == 201 and r.json()["status"] == "pending"
    assert r.json()["can_withdraw"] is False
    r = await client.post(
        "/api/v1/me/earnings/payout", json={"amount_cents": 1000}, headers=creator.headers
    )
    assert r.status_code == 403
    assert (await creator.wallet())["earnings_balance_cents"] == 1800  # nothing moved

    account_id = (await client.get("/api/v1/control/kyc", headers=KEY)).json()[0]["id"]
    await client.post(
        f"/api/v1/control/kyc/{account_id}", json={"approved": True}, headers=KEY
    )

    # Approved, but still can't take out more than is there.
    r = await client.post(
        "/api/v1/me/earnings/payout", json={"amount_cents": 5000}, headers=creator.headers
    )
    assert r.status_code == 402

    r = await client.post(
        "/api/v1/me/earnings/payout", json={"amount_cents": 1800}, headers=creator.headers
    )
    assert r.status_code == 200
    assert r.json()["status"] == "pending"  # requested, not paid
    assert (await creator.wallet())["earnings_balance_cents"] == 0


async def test_a_failed_payout_gives_the_money_back(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    fan = await make_user()
    await fan.topup(5000)
    await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 1000}, headers=fan.headers
    )
    await client.post(
        "/api/v1/me/payout-account",
        json={"legal_name": "A Creator", "country": "GB"},
        headers=creator.headers,
    )
    account_id = (await client.get("/api/v1/control/kyc", headers=KEY)).json()[0]["id"]
    await client.post(f"/api/v1/control/kyc/{account_id}", json={"approved": True}, headers=KEY)

    payout = (
        await client.post(
            "/api/v1/me/earnings/payout", json={"amount_cents": 900}, headers=creator.headers
        )
    ).json()
    assert (await creator.wallet())["earnings_balance_cents"] == 0

    queue = (await client.get("/api/v1/control/payouts", headers=KEY)).json()
    assert [p["id"] for p in queue] == [payout["id"]]

    r = await client.post(
        f"/api/v1/control/payouts/{payout['id']}",
        json={"status": "failed", "note": "bank rejected"},
        headers=KEY,
    )
    assert r.json()["earnings_balance_cents"] == 900

    # The failed attempt stays on the record rather than vanishing.
    settled = (await client.get("/api/v1/me/payouts", headers=creator.headers)).json()
    assert [(p["status"], p["note"]) for p in settled] == [("failed", "bank rejected")]


async def test_earnings_are_not_spendable_as_fan_credit(client, make_creator, make_user):
    """A creator's take sits in a separate balance; it can't silently pay for a purchase."""
    creator, handle, _ = await make_creator()
    fan = await make_user()
    await fan.topup(5000)
    await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 5000}, headers=fan.headers
    )

    other, other_handle, _ = await make_creator("Other")
    r = await client.post(
        f"/api/v1/creators/{other_handle}/tip",
        json={"amount_cents": 1000},
        headers=creator.headers,
    )
    assert r.status_code == 402  # 4500 in earnings, 0 in wallet


async def test_tips_respect_the_creators_minimum_and_switch(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    await client.patch(
        "/api/v1/creators/me",
        json={"min_tip_cents": 500},
        headers=creator.headers,
    )
    fan = await make_user()
    await fan.topup(5000)

    r = await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 100}, headers=fan.headers
    )
    assert r.status_code == 400
    assert "$5" in r.json()["detail"]

    await client.patch("/api/v1/creators/me", json={"tips_enabled": False}, headers=creator.headers)
    r = await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 500}, headers=fan.headers
    )
    assert r.status_code == 400


async def test_nobody_can_pay_themselves(client, make_creator):
    creator, handle, tier_id = await make_creator()
    await creator.topup(5000)
    assert (
        await client.post(
            f"/api/v1/creators/{handle}/tip", json={"amount_cents": 500}, headers=creator.headers
        )
    ).status_code == 400
    assert (
        await client.post(
            f"/api/v1/creators/{handle}/subscribe",
            json={"tier_id": tier_id},
            headers=creator.headers,
        )
    ).status_code == 400


async def test_cancelling_keeps_access_until_the_period_ends(client, make_creator, make_user):
    creator, handle, tier_id = await make_creator()
    r = await client.post(
        "/api/v1/posts",
        json={"title": "Members", "body": "still mine", "visibility": "subscribers"},
        headers=creator.headers,
    )
    post_id = r.json()["id"]

    fan = await make_user()
    await fan.topup(5000)
    sub = (
        await client.post(
            f"/api/v1/creators/{handle}/subscribe", json={"tier_id": tier_id}, headers=fan.headers
        )
    ).json()

    r = await client.delete(f"/api/v1/me/subscriptions/{sub['id']}", headers=fan.headers)
    assert r.status_code == 200
    assert r.json()["auto_renew"] is False
    assert (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()["locked"] is False
