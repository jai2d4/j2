"""Phase 1 health-contract coverage."""


def test_phase_one_health_alias(client):
    response = client.get("/api/health")
    assert response.status_code == 200
    assert response.json()["status"] == "ok"
