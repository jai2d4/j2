"""LIVE gating, the shop, and DMs."""


async def test_a_ticketed_live_hides_its_playback_url_until_bought(client, make_creator, make_user):
    creator, handle, _ = await make_creator("Jade")
    r = await client.post(
        "/api/v1/live",
        json={
            "title": "Backstage with me",
            "access": "ticket",
            "ticket_price_cents": 1500,
            "cover_url": "https://cdn.example.test/cover.jpg",
        },
        headers=creator.headers,
    )
    assert r.status_code == 201, r.text
    event_id = r.json()["id"]
    await client.post(f"/api/v1/live/{event_id}/start", headers=creator.headers)

    fan = await make_user()
    await fan.topup(5000)

    view = (await client.get(f"/api/v1/live/{event_id}", headers=fan.headers)).json()
    assert view["can_watch"] is False
    assert view["playback_url"] is None
    assert "$15" in view["lock_reason"]

    r = await client.post(f"/api/v1/live/{event_id}/ticket", headers=fan.headers)
    assert r.status_code == 200 and r.json()["charged_cents"] == 1500
    assert (await client.get(f"/api/v1/live/{event_id}", headers=fan.headers)).json()["can_watch"]

    # A second ticket is not sold to the same fan.
    assert (
        await client.post(f"/api/v1/live/{event_id}/ticket", headers=fan.headers)
    ).json()["charged_cents"] == 0


async def test_a_subscriber_only_live_follows_the_membership(client, make_creator, make_user):
    creator, handle, tier_id = await make_creator()
    r = await client.post(
        "/api/v1/live", json={"title": "Members hang", "access": "subscribers"}, headers=creator.headers
    )
    event_id = r.json()["id"]
    await client.post(f"/api/v1/live/{event_id}/start", headers=creator.headers)

    fan = await make_user()
    await fan.topup(5000)
    assert not (await client.get(f"/api/v1/live/{event_id}", headers=fan.headers)).json()["can_watch"]

    await client.post(
        f"/api/v1/creators/{handle}/subscribe", json={"tier_id": tier_id}, headers=fan.headers
    )
    assert (await client.get(f"/api/v1/live/{event_id}", headers=fan.headers)).json()["can_watch"]


async def test_the_live_now_rail_only_lists_running_streams(client, make_creator):
    creator, handle, _ = await make_creator()
    running = (
        await client.post("/api/v1/live", json={"title": "On air"}, headers=creator.headers)
    ).json()["id"]
    await client.post("/api/v1/live", json={"title": "Later"}, headers=creator.headers)
    await client.post(f"/api/v1/live/{running}/start", headers=creator.headers)

    assert [e["title"] for e in (await client.get("/api/v1/live")).json()] == ["On air"]
    assert [e["title"] for e in (await client.get("/api/v1/live?status=scheduled")).json()] == ["Later"]

    profile = (await client.get(f"/api/v1/creators/{handle}")).json()
    assert profile["is_live"] is True

    await client.post(f"/api/v1/live/{running}/end", headers=creator.headers)
    assert (await client.get("/api/v1/live")).json() == []


async def test_merch_needs_an_address_and_draws_down_inventory(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    r = await client.post(
        "/api/v1/creators/me/products",
        json={"kind": "merch", "name": "Tour tee", "price_cents": 3000, "inventory": 1},
        headers=creator.headers,
    )
    assert r.status_code == 201, r.text
    product_id = r.json()["id"]

    fan = await make_user()
    await fan.topup(10_000)

    r = await client.post(f"/api/v1/products/{product_id}/buy", json={}, headers=fan.headers)
    assert r.status_code == 422  # no shipping address

    r = await client.post(
        f"/api/v1/products/{product_id}/buy",
        json={"shipping_address": "1 Example St"},
        headers=fan.headers,
    )
    assert r.status_code == 201
    assert r.json()["status"] == "awaiting_fulfilment"
    assert r.json()["download_url"] is None

    listed = (await client.get(f"/api/v1/creators/{handle}/products")).json()
    assert listed[0]["inventory"] == 0 and listed[0]["sold_out"] is True

    second = await make_user()
    await second.topup(10_000)
    r = await client.post(
        f"/api/v1/products/{product_id}/buy",
        json={"shipping_address": "2 Example St"},
        headers=second.headers,
    )
    assert r.status_code == 409


async def test_a_digital_download_is_only_handed_to_its_buyer(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    r = await client.post(
        "/api/v1/creators/me/products",
        json={
            "kind": "digital",
            "name": "Unreleased demo",
            "price_cents": 800,
            "digital_asset_url": "https://cdn.example.test/demo.wav",
        },
        headers=creator.headers,
    )
    product_id = r.json()["id"]

    fan = await make_user()
    await fan.topup(2000)
    order = (
        await client.post(f"/api/v1/products/{product_id}/buy", json={}, headers=fan.headers)
    ).json()
    assert order["download_url"] == f"/api/v1/orders/{order['id']}/download"

    r = await client.get(order["download_url"], headers=fan.headers)
    assert r.json()["url"] == "https://cdn.example.test/demo.wav"

    stranger = await make_user()
    assert (await client.get(order["download_url"], headers=stranger.headers)).status_code == 404


async def test_a_digital_product_must_carry_a_file(client, make_creator):
    creator, _, _ = await make_creator()
    r = await client.post(
        "/api/v1/creators/me/products",
        json={"kind": "digital", "name": "Nothing", "price_cents": 500},
        headers=creator.headers,
    )
    assert r.status_code == 422


async def test_a_dm_thread_is_private_to_its_two_people(client, make_creator, make_user):
    creator, handle, _ = await make_creator("Jade")
    fan = await make_user("Sam")

    r = await client.post(
        f"/api/v1/creators/{handle}/messages",
        json={"body": "loved the show"},
        headers=fan.headers,
    )
    assert r.status_code == 201, r.text
    thread_id = r.json()["thread_id"]

    inbox = (await client.get("/api/v1/inbox", headers=creator.headers)).json()
    assert inbox[0]["display_name"] == "Sam"
    assert inbox[0]["unread_count"] == 1
    assert inbox[0]["last_message"] == "loved the show"

    r = await client.post(
        f"/api/v1/threads/{thread_id}/messages",
        json={"body": "thank you!"},
        headers=creator.headers,
    )
    assert r.status_code == 201

    thread = (await client.get(f"/api/v1/threads/{thread_id}", headers=fan.headers)).json()
    assert [(m["body"], m["from_me"]) for m in thread] == [
        ("loved the show", True),
        ("thank you!", False),
    ]

    # Reading marks the other side's messages read.
    assert (await client.get("/api/v1/inbox", headers=fan.headers)).json()[0]["unread_count"] == 0

    stranger = await make_user()
    assert (await client.get(f"/api/v1/threads/{thread_id}", headers=stranger.headers)).status_code == 403
    assert (
        await client.post(
            f"/api/v1/threads/{thread_id}/messages", json={"body": "hi"}, headers=stranger.headers
        )
    ).status_code == 403
