"""HTTP-level tests via TestClient. Gemini is always mocked — no live network calls."""


def test_health_is_always_open(client, api_key_required):
    # even with auth turned on, health stays public
    r = client.get("/api/v1/health")
    assert r.status_code == 200
    assert r.json()["status"] == "ok"


def test_metric_sieve_endpoint(client):
    r = client.post("/api/v1/scout/metric-sieve", json={
        "first_name": "Test", "last_name": "Player", "position": "DB",
        "height_in": 72, "weight_lbs": 190, "forty_s": 4.45, "shuttle_s": 4.15,
    })
    assert r.status_code == 200
    body = r.json()
    assert body["tier"] == "D1_FBS"
    assert body["qualifying_tiers"] == ["D1_FBS", "D1_FCS", "D2_D3_NAIA_JUCO"]


def test_makeup_grade_endpoint(client):
    r = client.post("/api/v1/scout/makeup-grade", json={
        "size": "WIN", "athletic_ability": "WIN_MINUS", "play_history": "WIN",
        "play_style": "WIN_PLUS", "character": "ALL_CONF",
    })
    assert r.status_code == 200
    assert r.json()["overall"] == "WIN"


def test_makeup_grade_requires_at_least_one_category(client):
    r = client.post("/api/v1/scout/makeup-grade", json={})
    assert r.status_code == 422


def test_analyze_film_rejects_neither_file_nor_url(client):
    r = client.post("/api/v1/scout/analyze-film", data={"position": "DB"})
    assert r.status_code == 400


def test_analyze_film_rejects_invalid_youtube_host(client):
    r = client.post("/api/v1/scout/analyze-film", data={
        "position": "DB", "youtube_url": "https://evil.com/video",
    })
    assert r.status_code == 400


def test_analyze_film_with_youtube_url(client, mock_gemini):
    mock_gemini(film_grades={"Play speed": "GAME_CHANGER"}, flags=["Elite play speed."])
    r = client.post("/api/v1/scout/analyze-film", data={
        "position": "DB", "youtube_url": "https://youtu.be/abc123",
    })
    assert r.status_code == 200
    body = r.json()
    assert body["film_grades"] == {"Play speed": "GAME_CHANGER"}
    assert body["flags"] == ["Elite play speed."]


def test_analyze_film_player_not_identified(client, mock_gemini):
    mock_gemini(film_grades={}, flags=[], player_identified=False,
                identification_note="No such player visible in this clip.")
    r = client.post("/api/v1/scout/analyze-film", data={
        "position": "DB", "youtube_url": "https://youtu.be/abc123",
        "player_identifier": "nonexistent jersey #99",
    })
    assert r.status_code == 200
    body = r.json()
    assert body["player_identified"] is False
    assert body["film_grades"] == {}


def test_truth_report_end_to_end(client, mock_gemini):
    mock_gemini(film_grades={"Toughness": "WIN"}, flags=[])
    r = client.post("/api/v1/scout/truth-report", data={
        "position": "DB",
        "athlete_json": '{"first_name":"Test","last_name":"Player","position":"DB",'
                        '"height_in":72,"weight_lbs":190,"forty_s":4.45,"shuttle_s":4.15}',
        "makeup_json": '{"size":"WIN","character":"ALL_CONF"}',
        "youtube_url": "https://youtu.be/abc123",
    })
    assert r.status_code == 200
    body = r.json()
    assert body["projected_tier"] == "D1_FBS"
    # average of WIN (idx 3) and ALL_CONF (idx 1) rounds to idx 2 = WIN_PLUS
    assert body["makeup_grade"]["overall"] == "WIN_PLUS"
    assert body["film_grades"] == {"Toughness": "WIN"}


# ---- Auth ----

def test_protected_route_401s_without_key(client, api_key_required):
    r = client.post("/api/v1/scout/metric-sieve", json={
        "first_name": "T", "last_name": "P", "position": "DB",
    })
    assert r.status_code == 401


def test_protected_route_401s_with_wrong_key(client, api_key_required):
    r = client.post(
        "/api/v1/scout/metric-sieve",
        json={"first_name": "T", "last_name": "P", "position": "DB"},
        headers={"X-API-Key": "wrong"},
    )
    assert r.status_code == 401


def test_protected_route_200s_with_correct_key(client, api_key_required):
    r = client.post(
        "/api/v1/scout/metric-sieve",
        json={"first_name": "T", "last_name": "P", "position": "DB"},
        headers={"X-API-Key": api_key_required},
    )
    assert r.status_code == 200


def test_auth_disabled_by_default(client):
    # no api_key_required fixture used here -> API_KEY is unset -> no auth required
    r = client.post("/api/v1/scout/metric-sieve", json={
        "first_name": "T", "last_name": "P", "position": "DB",
    })
    assert r.status_code == 200
