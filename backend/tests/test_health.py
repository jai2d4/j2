"""Phase 1 health-contract coverage."""

from fastapi.testclient import TestClient

from backend.main import app


def test_phase_one_health_alias():
    with TestClient(app) as client:
        response = client.get("/api/health")

    assert response.status_code == 200
    assert response.json()["status"] == "ok"
