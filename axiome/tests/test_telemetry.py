"""Heartbeats, events, and what the dashboard reads back."""
from tests.conftest import ADMIN


async def auth(added):
    return {"Authorization": f"Bearer {added['app_key']}"}


async def test_a_heartbeat_updates_status_and_metrics(client, connected):
    added = await connected()
    metrics = {"users": 7, "creators": 3, "open_reports": 2, "gross_volume_cents": 3200}

    r = await client.post("/api/apps/heartbeat",
                          json={"slug": "livephoria", "status": "ok", "metrics": metrics},
                          headers=await auth(added))
    assert r.status_code == 200

    app = (await client.get("/api/admin/apps/livephoria", headers=ADMIN)).json()
    assert app["status"] == "ok"
    assert app["metrics"] == metrics
    assert app["seconds_since_seen"] is not None and app["seconds_since_seen"] < 5


async def test_an_app_can_report_itself_degraded(client, connected):
    added = await connected()
    await client.post("/api/apps/heartbeat",
                      json={"slug": "livephoria", "status": "degraded", "metrics": {}},
                      headers=await auth(added))
    app = (await client.get("/api/admin/apps/livephoria", headers=ADMIN)).json()
    assert app["status"] == "degraded"
    assert app["status_detail"] == "Reported by the app"


async def test_heartbeats_build_history(client, connected):
    added = await connected()
    for n in range(3):
        await client.post("/api/apps/heartbeat",
                          json={"slug": "livephoria", "metrics": {"users": n}},
                          headers=await auth(added))
    samples = (await client.get("/api/admin/apps/livephoria/samples", headers=ADMIN)).json()
    assert [s["metrics"]["users"] for s in samples] == [0, 1, 2]
    assert {s["source"] for s in samples} == {"push"}


async def test_events_arrive_and_are_readable(client, connected):
    added = await connected()
    await client.post(
        "/api/apps/events",
        json={"slug": "livephoria", "event": "moderation.urgent_report",
              "data": {"report_id": 4, "reason": "nonconsensual"}},
        headers=await auth(added),
    )
    events = (await client.get("/api/admin/events", headers=ADMIN)).json()
    kinds = [e["event"] for e in events]
    assert "moderation.urgent_report" in kinds
    urgent = next(e for e in events if e["event"] == "moderation.urgent_report")
    assert urgent["data"]["reason"] == "nonconsensual"
    assert urgent["direction"] == "inbound"

    # Connecting the app was itself recorded, so the log explains how it got here.
    assert "app.connected" in kinds


async def test_the_overview_counts_what_is_registered(client, connected):
    await connected("Livephoria", "http://127.0.0.1:9")
    await connected("Other", "http://127.0.0.1:10")
    o = (await client.get("/api/admin/overview", headers=ADMIN)).json()
    assert o["apps"] == 2
    assert o["by_status"] == {"unreachable": 2}
    assert o["open_registration"] is False


async def test_driving_an_app_that_is_down_says_so_plainly(client, connected):
    await connected()
    r = await client.post("/api/admin/apps/livephoria/maintenance",
                          json={"enabled": True}, headers=ADMIN)
    assert r.status_code == 502
    assert "refused" in r.json()["detail"]
