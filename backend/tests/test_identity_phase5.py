from backend.vision.field_calibration import FieldCalibration, KnownFieldPoint, automatic_calibration_unavailable
from backend.vision.jersey_identifier import JerseyCandidate, TrackIdentity, confirm_identity


def test_manual_calibration_converts_pixels_to_yards():
    calibration = FieldCalibration(points=[
        KnownFieldPoint(pixel_x=100, pixel_y=50, x_yards=0, y_yards=0),
        KnownFieldPoint(pixel_x=1100, pixel_y=550, x_yards=100, y_yards=53.3),
    ], confidence=.9)
    assert calibration.pixel_to_field(600, 300) == (50.0, 26.65)
    assert calibration.confidence == .9


def test_automatic_calibration_does_not_pretend_success():
    result = automatic_calibration_unavailable()
    assert result["status"] == "needs_manual_points"
    assert result["confidence"] == 0


def test_jersey_candidates_are_unconfirmed():
    identity = TrackIdentity(track_id=17, candidates=[JerseyCandidate(jersey_number="19", confidence=.82)], confidence=.82)
    assert identity.jersey_number is None
    assert not identity.confirmed


def test_human_confirmation_retains_audit_values():
    original = TrackIdentity(track_id=17, candidates=[JerseyCandidate(jersey_number="19", confidence=.6)], confidence=.6)
    updated, audit = confirm_identity(original, player_id="player-19", jersey_number="19",
                                      team="red", position="LB", reason="Scout verified jersey on film")
    assert updated.confirmed and updated.confidence == 1
    assert updated.jersey_number == "19"
    assert audit["original_value"]["jersey_number"] is None
    assert audit["new_value"]["jersey_number"] == "19"
    assert audit["source"] == "human"

