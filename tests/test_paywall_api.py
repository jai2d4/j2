"""End-to-end: what a fan can and cannot see, and what unlocks it."""


async def test_a_stranger_sees_the_teaser_but_not_the_content(client, make_creator):
    creator, handle, _ = await make_creator("Aaliyah")
    r = await client.post(
        "/api/v1/posts",
        json={
            "kind": "video",
            "title": "Rehearsal footage",
            "body": "the secret part",
            "media_url": "https://cdn.example.test/rehearsal.mp4",
            "preview_url": "https://cdn.example.test/rehearsal-blur.jpg",
            "visibility": "subscribers",
        },
        headers=creator.headers,
    )
    assert r.status_code == 201
    post_id = r.json()["id"]

    r = await client.get(f"/api/v1/posts/{post_id}")
    body = r.json()
    assert body["locked"] is True
    assert body["title"] == "Rehearsal footage"  # the hook is public
    assert body["body"] is None  # the content is not
    assert body["media_url"] is None
    assert body["preview_url"] == "https://cdn.example.test/rehearsal-blur.jpg"


async def test_subscribing_unlocks_subscriber_posts(client, make_creator, make_user):
    creator, handle, tier_id = await make_creator("Aaliyah", price_cents=999)
    r = await client.post(
        "/api/v1/posts",
        json={"title": "Backstage", "body": "the good stuff", "visibility": "subscribers"},
        headers=creator.headers,
    )
    post_id = r.json()["id"]

    fan = await make_user("Fan")
    await fan.topup(5000)

    assert (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()["locked"]

    r = await client.post(
        f"/api/v1/creators/{handle}/subscribe",
        json={"tier_id": tier_id},
        headers=fan.headers,
    )
    assert r.status_code == 201, r.text

    unlocked = (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()
    assert unlocked["locked"] is False
    assert unlocked["body"] == "the good stuff"


async def test_a_cheap_tier_cannot_open_a_vip_gated_post(client, make_creator, make_user):
    creator, handle, basic_tier = await make_creator("Marcus", price_cents=500)
    r = await client.post(
        "/api/v1/creators/me/tiers",
        json={"name": "VIP", "price_cents": 5000, "is_vip": True},
        headers=creator.headers,
    )
    vip_tier = r.json()["id"]

    r = await client.post(
        "/api/v1/posts",
        json={"title": "VIP only", "body": "vip", "visibility": "subscribers", "min_tier_id": vip_tier},
        headers=creator.headers,
    )
    post_id = r.json()["id"]

    fan = await make_user()
    await fan.topup(10_000)
    await client.post(
        f"/api/v1/creators/{handle}/subscribe", json={"tier_id": basic_tier}, headers=fan.headers
    )

    body = (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()
    assert body["locked"] is True
    assert body["lock_reason"] == "tier_too_low"

    # Upgrading to VIP opens it.
    r = await client.post(
        f"/api/v1/creators/{handle}/subscribe", json={"tier_id": vip_tier}, headers=fan.headers
    )
    assert r.status_code == 201, r.text
    assert (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()["locked"] is False


async def test_unlocking_a_ppv_post_is_permanent_and_charged_once(client, make_creator, make_user):
    creator, handle, _ = await make_creator("Jade")
    r = await client.post(
        "/api/v1/posts",
        json={"title": "Studio session", "body": "full take", "visibility": "ppv", "price_cents": 1500},
        headers=creator.headers,
    )
    post_id = r.json()["id"]

    fan = await make_user()
    await fan.topup(2000)

    r = await client.post(f"/api/v1/posts/{post_id}/unlock", headers=fan.headers)
    assert r.status_code == 200, r.text
    assert r.json()["charged_cents"] == 1500
    assert r.json()["wallet_balance_cents"] == 500

    # Buying again is a no-op, not a second charge.
    r = await client.post(f"/api/v1/posts/{post_id}/unlock", headers=fan.headers)
    assert r.json()["charged_cents"] == 0
    assert (await fan.wallet())["wallet_balance_cents"] == 500
    assert (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()["locked"] is False


async def test_an_empty_wallet_cannot_unlock_anything(client, make_creator, make_user):
    creator, handle, tier_id = await make_creator()
    r = await client.post(
        "/api/v1/posts",
        json={"title": "Paid", "body": "x", "visibility": "ppv", "price_cents": 1500},
        headers=creator.headers,
    )
    post_id = r.json()["id"]

    broke = await make_user()
    await broke.topup(100)

    r = await client.post(f"/api/v1/posts/{post_id}/unlock", headers=broke.headers)
    assert r.status_code == 402
    assert (await broke.wallet())["wallet_balance_cents"] == 100  # nothing was taken

    r = await client.post(
        f"/api/v1/creators/{creator.handle}/subscribe", json={"tier_id": tier_id}, headers=broke.headers
    )
    assert r.status_code == 402


async def test_commenting_through_a_paywall_is_refused(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    r = await client.post(
        "/api/v1/posts",
        json={"title": "Locked", "body": "x", "visibility": "subscribers"},
        headers=creator.headers,
    )
    post_id = r.json()["id"]

    fan = await make_user()
    r = await client.post(
        f"/api/v1/posts/{post_id}/comments", json={"body": "let me in"}, headers=fan.headers
    )
    assert r.status_code == 403


async def test_the_feed_falls_back_to_public_posts_for_a_new_fan(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    await client.post(
        "/api/v1/posts",
        json={"title": "Hello world", "body": "public", "visibility": "public"},
        headers=creator.headers,
    )
    await client.post(
        "/api/v1/posts",
        json={"title": "Members", "body": "private", "visibility": "subscribers"},
        headers=creator.headers,
    )

    fan = await make_user()
    feed = (await client.get("/api/v1/feed", headers=fan.headers)).json()
    assert [p["title"] for p in feed] == ["Hello world"]

    # Once following, the members post appears too — as a locked teaser.
    await client.post(f"/api/v1/creators/{handle}/follow", headers=fan.headers)
    feed = (await client.get("/api/v1/feed", headers=fan.headers)).json()
    assert {p["title"]: p["locked"] for p in feed} == {"Members": True, "Hello world": False}


async def test_signed_out_browsing_works(client, make_creator):
    creator, handle, _ = await make_creator("Public Person")
    await client.post(
        "/api/v1/posts",
        json={"title": "Open", "body": "anyone", "visibility": "public"},
        headers=creator.headers,
    )
    feed = (await client.get("/api/v1/feed")).json()
    assert feed[0]["body"] == "anyone"

    profile = (await client.get(f"/api/v1/creators/{handle}")).json()
    assert profile["viewer_subscription"] is None
    assert profile["tiers"][0]["price_cents"] == 999
