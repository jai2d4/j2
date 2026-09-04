"""Module 5 — athlete/evaluation persistence against a real PostgreSQL instance.
Skips gracefully if no DB is reachable (CI provides one via a service container)."""
import pytest


@pytest.fixture(autouse=True)
def _skip_without_db(db_available):
    available, reason = db_available
    if not available:
        pytest.skip(f"No PostgreSQL reachable — set POSTGRES_* env vars to run these tests. ({reason})")


def test_create_and_get_athlete(client, api_key_required):
    headers = {"X-API-Key": api_key_required}
    r = client.post("/api/v1/athletes", json={
        "first_name": "Marcus", "last_name": "Reed", "position": "DB",
        "height_in": 72, "weight_lbs": 190,
    }, headers=headers)
    assert r.status_code == 201
    athlete = r.json()
    assert athlete["first_name"] == "Marcus"

    r2 = client.get(f"/api/v1/athletes/{athlete['id']}", headers=headers)
    assert r2.status_code == 200
    assert r2.json()["id"] == athlete["id"]


def test_get_missing_athlete_404s(client, api_key_required):
    headers = {"X-API-Key": api_key_required}
    r = client.get("/api/v1/athletes/00000000-0000-0000-0000-000000000000", headers=headers)
    assert r.status_code == 404


def test_truth_report_persists_evaluation(client, mock_gemini):
    mock_gemini(film_grades={"Toughness": "WIN"}, flags=[])
    r = client.post("/api/v1/scout/truth-report", data={
        "position": "DB",
        "athlete_json": '{"first_name":"Persisted","last_name":"Player","position":"DB",'
                        '"height_in":72,"weight_lbs":190,"forty_s":4.45,"shuttle_s":4.15}',
        "youtube_url": "https://youtu.be/abc123",
    })
    assert r.status_code == 200
    body = r.json()
    assert body["persisted"] is True
    assert body["evaluation_id"] is not None

    r2 = client.get(f"/api/v1/evaluations/{body['evaluation_id']}")
    assert r2.status_code == 200
    ev = r2.json()
    assert ev["projected_tier"] == "D1_FBS"
    assert ev["film_grades"] == {"Toughness": "WIN"}


def test_truth_report_links_to_existing_athlete(client, mock_gemini):
    mock_gemini()
    create = client.post("/api/v1/athletes", json={
        "first_name": "Existing", "last_name": "Athlete", "position": "DB",
    })
    athlete_id = create.json()["id"]

    r = client.post("/api/v1/scout/truth-report", data={
        "position": "DB",
        "athlete_json": '{"first_name":"Existing","last_name":"Athlete","position":"DB",'
                        '"height_in":72,"weight_lbs":190,"forty_s":4.45,"shuttle_s":4.15}',
        "youtube_url": "https://youtu.be/abc123",
        "athlete_id": athlete_id,
    })
    assert r.status_code == 200
    assert r.json()["athlete_id"] == athlete_id
