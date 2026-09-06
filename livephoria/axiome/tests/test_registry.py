"""Adding apps, and the keys that keep the registry honest."""
from tests.conftest import ADMIN


async def test_admin_routes_need_the_admin_key(client):
    assert (await client.get("/api/admin/apps")).status_code == 401
    assert (await client.get("/api/admin/apps", headers={"X-Admin-Key": "wrong"})).status_code == 401
    assert (await client.get("/api/admin/apps", headers=ADMIN)).status_code == 200


async def test_health_stays_open(client):
    assert (await client.get("/api/health")).json()["app"] == "axiome"


async def test_adding_an_app_saves_it_even_when_it_is_not_up(client, connected):
    result = await connected("Livephoria", "http://127.0.0.1:9")
    assert result["reachable"] is False
    assert result["ok"] is False
    assert "nothing answered" in result["detail"].lower()
    # Saved anyway: an app that is down now can come up later.
    assert result["app"]["slug"] == "livephoria"
    assert result["app"]["status"] == "unreachable"
    assert result["app_key"].startswith("axk_")

    listed = (await client.get("/api/admin/apps", headers=ADMIN)).json()
    assert [a["slug"] for a in listed] == ["livephoria"]


async def test_a_slug_is_only_claimed_once(client, connected):
    await connected("Livephoria")
    r = await client.post(
        "/api/admin/apps",
        json={"name": "Livephoria", "base_url": "http://127.0.0.1:9"},
        headers=ADMIN,
    )
    assert r.status_code == 409


async def test_an_unknown_app_cannot_register_itself(client):
    """Default is closed: anything that can reach the port must not plant itself."""
    r = await client.post(
        "/api/apps/register",
        json={"slug": "stranger", "name": "Stranger"},
        headers={"Authorization": "Bearer made-up"},
    )
    assert r.status_code == 403
    assert "Add it in Axiome first" in r.json()["detail"]
    assert (await client.get("/api/admin/apps", headers=ADMIN)).json() == []


async def test_a_known_app_registers_with_its_issued_key(client, connected):
    added = await connected()
    key = added["app_key"]

    r = await client.post(
        "/api/apps/register",
        json={
            "slug": "livephoria", "name": "Livephoria", "kind": "creator-platform",
            "version": "0.1.0", "public_url": "https://livephoria.example",
            "control_url": "/api/v1/control", "capabilities": ["subscriptions", "kyc"],
        },
        headers={"Authorization": f"Bearer {key}"},
    )
    assert r.status_code == 200 and r.json()["app"] == "livephoria"

    app = (await client.get("/api/admin/apps/livephoria", headers=ADMIN)).json()
    assert app["version"] == "0.1.0"
    assert app["capabilities"] == ["subscriptions", "kyc"]
    assert app["status"] == "ok"
    assert app["public_url"] == "https://livephoria.example"


async def test_the_wrong_key_is_refused_everywhere(client, connected):
    await connected()
    bad = {"Authorization": "Bearer axk_wrong"}
    assert (await client.post("/api/apps/register", json={"slug": "livephoria"}, headers=bad)).status_code == 401
    assert (await client.post("/api/apps/heartbeat", json={"slug": "livephoria"}, headers=bad)).status_code == 401
    assert (await client.post("/api/apps/events",
                              json={"slug": "livephoria", "event": "x"}, headers=bad)).status_code == 401
    # No key at all is refused too.
    assert (await client.post("/api/apps/heartbeat", json={"slug": "livephoria"})).status_code == 401


async def test_rotating_a_key_invalidates_the_old_one(client, connected):
    added = await connected()
    old = added["app_key"]
    new = (await client.post("/api/admin/apps/livephoria/rotate-key", headers=ADMIN)).json()["app_key"]
    assert new != old

    beat = {"slug": "livephoria", "metrics": {"users": 1}}
    assert (await client.post("/api/apps/heartbeat", json=beat,
                              headers={"Authorization": f"Bearer {old}"})).status_code == 401
    assert (await client.post("/api/apps/heartbeat", json=beat,
                              headers={"Authorization": f"Bearer {new}"})).status_code == 200


async def test_the_plain_key_is_never_readable_afterwards(client, connected):
    added = await connected()
    app = (await client.get("/api/admin/apps/livephoria", headers=ADMIN)).json()
    assert added["app_key"] not in str(app)
    assert app["app_key_hint"] == added["app_key"][:8]


async def test_forgetting_an_app_leaves_no_trace(client, connected):
    added = await connected()
    await client.post("/api/apps/heartbeat", json={"slug": "livephoria", "metrics": {"users": 3}},
                      headers={"Authorization": f"Bearer {added['app_key']}"})
    assert (await client.delete("/api/admin/apps/livephoria", headers=ADMIN)).status_code == 204
    assert (await client.get("/api/admin/apps", headers=ADMIN)).json() == []
    assert (await client.get("/api/admin/apps/livephoria", headers=ADMIN)).status_code == 404
