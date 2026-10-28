import json
from pathlib import Path

import pytest

from python_client.dashboard_state import (
    DashboardState,
    Thresholds,
    build_risk_command,
)

INT_MAX = 2_147_483_647


def test_thresholds_classify_configurable_smoking_counts():
    thresholds = Thresholds(medium_smoking=2, high_smoking=5)

    assert thresholds.classify(0) == "low"
    assert thresholds.classify(2) == "medium"
    assert thresholds.classify(5) == "high"


def test_environment_thresholds_classify_sensor_alerts():
    thresholds = Thresholds(temperature_max=35.0, humidity_max=70.0, mq2_max=1800)
    assert thresholds.classify_environment(20.0, 50.0, 900, True) == "low"
    assert thresholds.classify_environment(36.0, 50.0, 900, True) == "high"
    assert thresholds.classify_environment(20.0, 50.0, 1900, False) == "high"


@pytest.mark.parametrize("smoking", [True, 2.5, -1])
def test_thresholds_classify_rejects_invalid_smoking_counts(smoking):
    thresholds = Thresholds(medium_smoking=2, high_smoking=5)

    with pytest.raises(ValueError):
        thresholds.classify(smoking)


@pytest.mark.parametrize(
    ("medium", "high"),
    [(0, 3), (3, 3), (5, 2)],
)
def test_thresholds_reject_invalid_boundaries(medium, high):
    with pytest.raises(ValueError):
        Thresholds(medium_smoking=medium, high_smoking=high)


@pytest.mark.parametrize(
    ("medium", "high"),
    [(True, 3), (2.5, 6), (2, False), (2, 6.5)],
)
def test_thresholds_reject_non_integer_boundaries(medium, high):
    with pytest.raises(ValueError):
        Thresholds(medium_smoking=medium, high_smoking=high)


def test_dashboard_state_keeps_latest_frame_and_snapshot():
    state = DashboardState()

    state.update_frame(
        jpeg=b"frame",
        people=4,
        smoking=1,
        smoking_confidence=0.83,
        risk="medium",
        receive_fps=9.5,
        inference_fps=3.2,
        warning="中风险：检测到 4 人",
    )

    assert state.latest_frame == b"frame"
    snapshot = state.snapshot()
    assert snapshot["people"] == 4
    assert snapshot["smoking"] == 1
    assert snapshot["smoking_confidence"] == 0.83
    assert snapshot["risk"] == "medium"
    assert snapshot["receive_fps"] == 9.5
    assert snapshot["inference_fps"] == 3.2
    assert snapshot["warning"] == "中风险：检测到 4 人"
    assert snapshot["frame_sequence"] == 1


def test_dashboard_state_accepts_explicit_frame_sequence_with_gaps():
    state = DashboardState()

    state.update_preview_frame(b"frame", 8.0, frame_sequence=7)

    assert state.latest_frame == b"frame"
    assert state.frame_sequence == 7


def test_build_risk_command_matches_esp32_protocol():
    message = build_risk_command(
        command_id=7,
        level="high",
        people=6,
        smoking=2,
        smoking_confidence=0.91234,
    )

    assert json.loads(message) == {
        "type": "risk",
        "command_id": 7,
        "level": "high",
        "people": 6,
        "smoking": 2,
        "smoking_confidence": 0.9123,
    }


@pytest.mark.parametrize(
    ("command_id", "people"),
    [(True, 0), (1.5, 0), (1, True), (1, 1.5)],
)
def test_build_risk_command_rejects_non_integer_counts(command_id, people):
    with pytest.raises(ValueError):
        build_risk_command(command_id=command_id, level="low", people=people)


@pytest.mark.parametrize(
    ("command_id", "people"),
    [(INT_MAX, 0), (1, INT_MAX)],
)
def test_build_risk_command_accepts_esp32_integer_limit(command_id, people):
    message = build_risk_command(
        command_id=command_id,
        level="low",
        people=people,
    )

    payload = json.loads(message)
    assert payload["command_id"] == command_id
    assert payload["people"] == people


@pytest.mark.parametrize(
    ("command_id", "people"),
    [(INT_MAX + 1, 0), (1, INT_MAX + 1), (-1, 0), (1, -1)],
)
def test_build_risk_command_rejects_counts_outside_esp32_range(
    command_id, people
):
    with pytest.raises(ValueError):
        build_risk_command(command_id=command_id, level="low", people=people)


def test_build_risk_command_rejects_invalid_level():
    with pytest.raises(ValueError):
        build_risk_command(command_id=1, level="critical", people=0)


def test_save_latest_frame_writes_image_and_metadata(tmp_path: Path):
    state = DashboardState()
    state.update_frame(
        jpeg=b"jpeg-bytes",
        people=1,
        smoking=0,
        smoking_confidence=0.0,
        risk="low",
        receive_fps=10.0,
        inference_fps=4.0,
        warning="低风险：检测到 1 人",
    )

    result = state.save_latest_frame(tmp_path, "capture")

    assert result.image_path.exists()
    assert result.metadata_path.exists()
    assert result.image_path.read_bytes() == b"jpeg-bytes"
    metadata = json.loads(result.metadata_path.read_text(encoding="utf-8"))
    assert metadata["people"] == 1
    assert metadata["smoking"] == 0
    assert metadata["smoking_confidence"] == 0.0
    assert metadata["risk"] == "low"
