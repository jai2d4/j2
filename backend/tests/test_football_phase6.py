from backend.football.movement import movement_metrics
from backend.football.observations import FootballObservation, ObservationDatum
from backend.football.play_segmenter import Play, PlaySegmenter, correct_play
from backend.grading.models import Evidence


def test_segmenter_splits_activity_gaps_and_labels_low_confidence():
    tracks = [{"positions": [{"timestamp_ms": 1000}, {"timestamp_ms": 2000}, {"timestamp_ms": 8000}]}]
    plays = PlaySegmenter(gap_ms=4000).segment("V1", tracks)
    assert [play.play_id for play in plays] == ["P001", "P002"]
    assert all(play.confidence < .5 and play.source == "automatic" for play in plays)


def test_manual_play_correction_preserves_original():
    play = Play(play_id="P001", video_id="V1", start_time=1, snap_time=2, end_time=5, confidence=.4)
    corrected, audit = correct_play(play, start_time=1.5, snap_time=2.5, end_time=5.5,
                                    excluded=True, reason="Scout corrected whistle")
    assert corrected.excluded and corrected.source == "human" and corrected.confidence == 1
    assert audit["original_value"]["snap_time"] == 2
    assert audit["source"] == "human"


def test_movement_metrics_are_derived_from_tracks():
    result = movement_metrics([
        {"timestamp_ms": 0, "center_x": 0, "center_y": 0, "speed_px": 0, "direction": 0, "confidence": .8},
        {"timestamp_ms": 1000, "center_x": 3, "center_y": 4, "speed_px": 5, "direction": 45, "confidence": .6},
    ])
    assert result["displacement_px"] == 5
    assert result["max_acceleration_px_s2"] == 5
    assert result["confidence"] == .7


def test_insufficient_movement_is_unknown_not_guessed():
    assert movement_metrics([])["value"] == "unknown"


def test_structured_observation_links_evidence():
    evidence = Evidence(video_id="V1", play_id="P001", timestamp_start=1,
                        timestamp_end=2, frame_ids=[10], track_id=17,
                        player_id="19", description="Triggered downhill")
    observation = FootballObservation(
        observation_id="O1", play_id="P001", player_id="19", position="LB",
        movement={"initial_direction": ObservationDatum(value="forward_inside", confidence=.9,
                                                          evidence_timestamp=1.2, reason="Tracked movement")},
        evidence=[evidence],
    )
    assert observation.evidence[0].track_id == 17
    assert observation.movement["initial_direction"].confidence == .9

