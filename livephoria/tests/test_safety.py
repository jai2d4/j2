"""Reporting, review, appeals, and age assurance."""
from tests.conftest import CONTROL_KEY

KEY = {"X-Axiome-Key": CONTROL_KEY}


async def make_post(client, creator, **overrides):
    body = {"title": "A post", "body": "content", "visibility": "public"}
    body.update(overrides)
    r = await client.post("/api/v1/posts", json=body, headers=creator.headers)
    assert r.status_code == 201, r.text
    return r.json()["id"]


async def test_reasons_are_a_closed_list_with_urgency_marked(client):
    reasons = (await client.get("/api/v1/report-reasons")).json()
    by_key = {r["reason"]: r for r in reasons}
    assert by_key["csam"]["urgent"] is True
    assert by_key["spam"]["urgent"] is False

    fan_post = None
    r = await client.post(
        "/api/v1/reports",
        json={"target_type": "post", "target_id": 1, "reason": "not-a-reason"},
    )
    assert r.status_code == 422


async def test_anyone_can_report_and_nothing_is_auto_actioned(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    post_id = await make_post(client, creator)

    # Signed out, deliberately: requiring an account would suppress the reports
    # that matter most.
    r = await client.post(
        "/api/v1/reports",
        json={"target_type": "post", "target_id": post_id, "reason": "harassment",
              "detail": "targeted abuse in the caption"},
    )
    assert r.status_code == 201
    assert r.json()["status"] == "open"
    assert r.json()["priority"] == "normal"

    # The post is untouched until someone decides.
    assert (await client.get(f"/api/v1/posts/{post_id}")).json()["moderation_status"] == "visible"
    assert len((await client.get("/api/v1/feed")).json()) == 1


async def test_urgent_reports_sort_to_the_top_of_the_queue(client, make_creator):
    creator, handle, _ = await make_creator()
    slow = await make_post(client, creator, title="Spammy")
    bad = await make_post(client, creator, title="Serious")

    await client.post("/api/v1/reports",
                      json={"target_type": "post", "target_id": slow, "reason": "spam"})
    await client.post("/api/v1/reports",
                      json={"target_type": "post", "target_id": bad, "reason": "nonconsensual"})

    queue = (await client.get("/api/v1/control/moderation/reports", headers=KEY)).json()
    assert [r["reason"] for r in queue] == ["nonconsensual", "spam"]
    assert queue[0]["priority"] == "urgent"


async def test_removing_a_post_hides_it_everywhere_but_from_its_author(
    client, make_creator, make_user
):
    creator, handle, _ = await make_creator()
    post_id = await make_post(client, creator, title="Removed later")
    fan = await make_user()
    await client.post(f"/api/v1/creators/{handle}/follow", headers=fan.headers)

    report = (await client.post(
        "/api/v1/reports",
        json={"target_type": "post", "target_id": post_id, "reason": "copyright"},
    )).json()

    r = await client.post(
        f"/api/v1/control/moderation/reports/{report['id']}",
        json={"action": "remove_content", "note": "third-party footage"},
        headers=KEY,
    )
    assert r.json()["action"] == "remove_content"
    assert r.json()["appealable"] is True

    assert (await client.get("/api/v1/feed", headers=fan.headers)).json() == []
    assert (await client.get(f"/api/v1/creators/{handle}/posts", headers=fan.headers)).json() == []

    seen_by_stranger = (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()
    assert seen_by_stranger["locked"] is True
    assert seen_by_stranger["body"] is None
    assert seen_by_stranger["unlock_label"] == "Removed after review"

    # The author still sees it, flagged, so a removal is never silent.
    own = (await client.get(f"/api/v1/creators/{handle}/posts", headers=creator.headers)).json()
    assert [(p["title"], p["moderation_status"]) for p in own] == [("Removed later", "removed")]

    # A report that is resolved cannot be decided twice.
    assert (await client.post(
        f"/api/v1/control/moderation/reports/{report['id']}",
        json={"action": "dismiss"}, headers=KEY)).status_code == 409


async def test_an_upheld_appeal_puts_the_post_back(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    post_id = await make_post(client, creator, title="Wrongly removed")
    report = (await client.post(
        "/api/v1/reports",
        json={"target_type": "post", "target_id": post_id, "reason": "copyright"},
    )).json()
    action = (await client.post(
        f"/api/v1/control/moderation/reports/{report['id']}",
        json={"action": "remove_content"}, headers=KEY)).json()

    # The creator is told what happened and that it can be appealed.
    mine = (await client.get("/api/v1/me/actions", headers=creator.headers)).json()
    assert [(a["action"], a["appealable"], a["appeal_status"]) for a in mine] == [
        ("remove_content", True, None)
    ]

    r = await client.post(
        "/api/v1/appeals",
        json={"action_id": action["action_id"], "body": "It's my own footage."},
        headers=creator.headers,
    )
    assert r.status_code == 201
    appeal_id = r.json()["id"]
    assert (await client.post(
        "/api/v1/appeals",
        json={"action_id": action["action_id"], "body": "again"},
        headers=creator.headers)).status_code == 409

    queue = (await client.get("/api/v1/control/moderation/appeals", headers=KEY)).json()
    assert [a["id"] for a in queue] == [appeal_id]

    r = await client.post(
        f"/api/v1/control/moderation/appeals/{appeal_id}",
        json={"decision": "upheld", "note": "creator owns it"},
        headers=KEY,
    )
    assert r.json()["reversed_with"] == "restore_content"
    assert len((await client.get("/api/v1/feed")).json()) == 1
    assert (await client.get(f"/api/v1/posts/{post_id}")).json()["moderation_status"] == "visible"


async def test_a_rejected_appeal_leaves_the_decision_standing(client, make_creator):
    creator, handle, _ = await make_creator()
    post_id = await make_post(client, creator)
    report = (await client.post(
        "/api/v1/reports",
        json={"target_type": "post", "target_id": post_id, "reason": "harassment"},
    )).json()
    action = (await client.post(
        f"/api/v1/control/moderation/reports/{report['id']}",
        json={"action": "remove_content"}, headers=KEY)).json()
    appeal = (await client.post(
        "/api/v1/appeals",
        json={"action_id": action["action_id"], "body": "please"},
        headers=creator.headers)).json()

    r = await client.post(
        f"/api/v1/control/moderation/appeals/{appeal['id']}",
        json={"decision": "rejected", "note": "stands"}, headers=KEY)
    assert r.json()["reversed_with"] is None
    assert (await client.get("/api/v1/feed")).json() == []

    mine = (await client.get("/api/v1/me/appeals", headers=creator.headers)).json()
    assert mine[0]["status"] == "rejected" and mine[0]["decision_note"] == "stands"


async def test_a_dismissal_is_not_appealable(client, make_creator):
    creator, handle, _ = await make_creator()
    post_id = await make_post(client, creator)
    report = (await client.post(
        "/api/v1/reports",
        json={"target_type": "post", "target_id": post_id, "reason": "spam"},
    )).json()
    action = (await client.post(
        f"/api/v1/control/moderation/reports/{report['id']}",
        json={"action": "dismiss", "note": "not spam"}, headers=KEY)).json()
    assert action["appealable"] is False
    assert (await client.get("/api/v1/feed")).json() != []

    r = await client.post(
        "/api/v1/appeals", json={"action_id": action["action_id"], "body": "?"},
        headers=creator.headers)
    assert r.status_code == 400


async def test_suspending_a_creator_from_a_report_stops_them_acting(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    post_id = await make_post(client, creator)
    report = (await client.post(
        "/api/v1/reports",
        json={"target_type": "creator", "target_id": 1, "reason": "impersonation"},
    )).json()
    await client.post(
        f"/api/v1/control/moderation/reports/{report['id']}",
        json={"action": "suspend_user", "note": "impersonation"}, headers=KEY)

    r = await client.post("/api/v1/posts", json={"title": "x", "body": "y"}, headers=creator.headers)
    assert r.status_code == 403


async def test_moderation_can_act_without_a_report(client, make_creator):
    creator, handle, _ = await make_creator()
    post_id = await make_post(client, creator)
    r = await client.post(
        "/api/v1/control/moderation/actions",
        json={"action": "remove_content", "target_type": "post", "target_id": post_id,
              "reason": "csam", "note": "legal order"},
        headers=KEY,
    )
    assert r.status_code == 200
    assert (await client.get("/api/v1/feed")).json() == []


# ---------------------------------------------------------------- age assurance


async def test_a_declared_minor_is_refused_outright(client, make_user):
    fan = await make_user()
    r = await client.post(
        "/api/v1/me/age-verification",
        json={"date_of_birth": "2015-01-01"},
        headers=fan.headers,
    )
    assert r.status_code == 403
    assert (await client.get("/api/v1/me/age-verification", headers=fan.headers)).json()["status"] == "failed"


async def test_declaring_an_adult_birthday_is_not_itself_proof(client, make_user):
    """The mock provider never approves — an auto-approving stub would fake the check."""
    fan = await make_user()
    r = await client.post(
        "/api/v1/me/age-verification",
        json={"date_of_birth": "1990-05-04"},
        headers=fan.headers,
    )
    assert r.status_code == 200
    assert r.json()["status"] == "pending"
    assert r.json()["verified_at"] is None

    queue = (await client.get("/api/v1/control/age-verifications", headers=KEY)).json()
    assert [q["user_id"] for q in queue] == [fan.id]

    await client.post(f"/api/v1/control/age-verifications/{fan.id}",
                      json={"approved": True}, headers=KEY)
    assert (await client.get("/api/v1/me/age-verification", headers=fan.headers)).json()["status"] == "verified"


async def test_adult_content_is_hidden_until_the_viewer_is_verified(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    # The author has to be verified to publish it in the first place.
    r = await client.post(
        "/api/v1/posts",
        json={"title": "18+", "body": "adult", "visibility": "public", "is_adult": True},
        headers=creator.headers,
    )
    assert r.status_code == 403

    await client.post(f"/api/v1/control/age-verifications/{creator.id}",
                      json={"approved": True}, headers=KEY)
    post_id = await make_post(client, creator, title="18+", is_adult=True)

    fan = await make_user()
    await fan.topup(5000)
    seen = (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()
    assert seen["locked"] is True
    assert seen["lock_reason"] == "age_verification_required"
    assert seen["body"] is None

    # Money does not buy past it: the post is public, so there is nothing to pay for.
    await client.post(f"/api/v1/control/age-verifications/{fan.id}",
                      json={"approved": True}, headers=KEY)
    assert (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()["locked"] is False


async def test_paying_never_opens_an_age_gate(client, make_creator, make_user):
    creator, handle, _ = await make_creator()
    await client.post(f"/api/v1/control/age-verifications/{creator.id}",
                      json={"approved": True}, headers=KEY)
    post_id = await make_post(
        client, creator, title="Paid 18+", visibility="ppv", price_cents=500, is_adult=True
    )

    fan = await make_user()
    await fan.topup(5000)
    r = await client.post(f"/api/v1/posts/{post_id}/unlock", headers=fan.headers)
    assert r.status_code == 200  # the purchase itself is allowed

    seen = (await client.get(f"/api/v1/posts/{post_id}", headers=fan.headers)).json()
    assert seen["locked"] is True
    assert seen["lock_reason"] == "age_verification_required"


async def test_an_adult_channel_needs_a_verified_owner(client, make_creator):
    creator, handle, _ = await make_creator()
    r = await client.patch(
        "/api/v1/creators/me", json={"is_adult_channel": True}, headers=creator.headers
    )
    assert r.status_code == 403

    await client.post(f"/api/v1/control/age-verifications/{creator.id}",
                      json={"approved": True}, headers=KEY)
    r = await client.patch(
        "/api/v1/creators/me", json={"is_adult_channel": True}, headers=creator.headers
    )
    assert r.status_code == 200 and r.json()["is_adult_channel"] is True


async def test_safety_backlog_shows_up_in_the_metrics_axiome_reads(client, make_creator):
    creator, handle, _ = await make_creator()
    post_id = await make_post(client, creator)
    await client.post("/api/v1/reports",
                      json={"target_type": "post", "target_id": post_id, "reason": "csam"})

    metrics = (await client.get("/api/v1/control/metrics", headers=KEY)).json()
    assert metrics["open_reports"] == 1
    assert metrics["urgent_reports"] == 1
    assert metrics["open_appeals"] == 0
    assert metrics["kyc_pending"] == 0
    assert metrics["payouts_pending_cents"] == 0
