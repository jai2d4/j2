"""The Axiome-facing control surface."""
from tests.conftest import CONTROL_KEY

KEY = {"X-Axiome-Key": CONTROL_KEY}


async def test_control_routes_refuse_callers_without_the_key(client):
    assert (await client.get("/api/v1/control/status")).status_code == 401
    assert (
        await client.get("/api/v1/control/status", headers={"X-Axiome-Key": "wrong"})
    ).status_code == 401
    assert (await client.get("/api/v1/control/status", headers=KEY)).status_code == 200


async def test_status_reports_version_database_and_axiome_wiring(client):
    body = (await client.get("/api/v1/control/status", headers=KEY)).json()
    assert body["app"] == "livephoria"
    assert body["database"] == "ok"
    assert body["maintenance"] is False
    # No AXIOME_BASE_URL in tests, so the outbound half is off.
    assert body["axiome"]["configured"] is False


async def test_metrics_count_what_actually_happened(client, make_creator, make_user):
    creator, handle, tier_id = await make_creator()
    await client.post(
        "/api/v1/posts", json={"title": "Hi", "body": "x"}, headers=creator.headers
    )
    fan = await make_user()
    await fan.topup(5000)
    await client.post(
        f"/api/v1/creators/{handle}/subscribe", json={"tier_id": tier_id}, headers=fan.headers
    )

    metrics = (await client.get("/api/v1/control/metrics", headers=KEY)).json()
    assert metrics["creators"] == 1
    assert metrics["users"] == 2
    assert metrics["posts"] == 1
    assert metrics["active_subscriptions"] == 1
    # Top-ups are not revenue and must not inflate volume.
    assert metrics["gross_volume_cents"] == 999
    assert metrics["platform_fees_cents"] == 99


async def test_config_never_leaks_secrets(client):
    body = (await client.get("/api/v1/control/config", headers=KEY)).json()
    text = str(body)
    assert CONTROL_KEY not in text
    assert "test-secret" not in text
    assert body["platform_fee_bps"] == 1000


async def test_maintenance_closes_the_app_but_not_the_control_plane(client, make_user):
    fan = await make_user()
    r = await client.post(
        "/api/v1/control/maintenance",
        json={"enabled": True, "message": "Back in ten."},
        headers=KEY,
    )
    assert r.json()["maintenance"] is True

    blocked = await client.get("/api/v1/feed")
    assert blocked.status_code == 503
    assert blocked.json()["detail"] == "Back in ten."
    assert (await client.get("/api/v1/health")).json()["status"] == "maintenance"
    assert (await client.get("/api/v1/control/status", headers=KEY)).status_code == 200

    await client.post("/api/v1/control/maintenance", json={"enabled": False}, headers=KEY)
    assert (await client.get("/api/v1/feed")).status_code == 200


async def test_a_suspended_account_cannot_act(client, make_user, make_creator):
    creator, handle, _ = await make_creator()
    fan = await make_user()
    await fan.topup(5000)

    r = await client.post(f"/api/v1/control/users/{fan.id}/suspend", headers=KEY)
    assert r.json() == {"user_id": fan.id, "suspended": True}

    r = await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 500}, headers=fan.headers
    )
    assert r.status_code == 403
    # Public browsing still works — suspension is not a ban from reading.
    assert (await client.get("/api/v1/feed")).status_code == 200

    await client.post(f"/api/v1/control/users/{fan.id}/restore", headers=KEY)
    r = await client.post(
        f"/api/v1/creators/{handle}/tip", json={"amount_cents": 500}, headers=fan.headers
    )
    assert r.status_code == 200


async def test_discover_ranks_and_searches(client, make_creator, make_user):
    quiet, quiet_handle, _ = await make_creator("Quiet Act")
    loud, loud_handle, loud_tier = await make_creator("Loud Act")

    fan = await make_user()
    await fan.topup(20_000)
    await client.post(
        f"/api/v1/creators/{loud_handle}/subscribe", json={"tier_id": loud_tier}, headers=fan.headers
    )

    trending = (await client.get("/api/v1/discover")).json()
    assert trending[0]["handle"] == loud_handle

    found = (await client.get("/api/v1/discover?q=quiet")).json()
    assert [c["handle"] for c in found] == [quiet_handle]

    stats = (await client.get("/api/v1/discover/stats")).json()
    assert stats["creators"] == 2 and stats["fans"] == 3
