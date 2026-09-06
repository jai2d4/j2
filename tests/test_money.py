"""Wallet, tips, the platform's cut, and the earnings dashboard."""


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


async def test_a_creator_can_withdraw_only_what_they_have(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    fan = await make_user()
    await fan.topup(5000)
    await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 2000}, headers=fan.headers
    )

    r = await client.post(
        "/api/v1/me/earnings/payout", json={"amount_cents": 5000}, headers=creator.headers
    )
    assert r.status_code == 402
    assert (await creator.wallet())["earnings_balance_cents"] == 1800

    r = await client.post(
        "/api/v1/me/earnings/payout", json={"amount_cents": 1800}, headers=creator.headers
    )
    assert r.status_code == 200
    assert r.json()["earnings_balance_cents"] == 0


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
