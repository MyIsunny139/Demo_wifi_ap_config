import asyncio
import json

import pytest

from python_client.video_ai import (
    RiskReporter,
    offer_latest,
    parse_ack,
    resolve_detection_classes,
)


def test_resolve_detection_classes_accepts_person_and_smoking_aliases():
    people, smoking = resolve_detection_classes(
        {0: "person", 1: "not_smoking", 2: "Smoking-Person"}
    )

    assert people == {0}
    assert smoking == {2}


def test_resolve_detection_classes_accepts_downloaded_model_typo():
    people, smoking = resolve_detection_classes({0: "Smooking"})

    assert people == set()
    assert smoking == {0}


def test_resolve_detection_classes_rejects_generic_coco_model():
    with pytest.raises(ValueError, match="不包含抽烟类别"):
        resolve_detection_classes({0: "person", 1: "bicycle", 2: "car"})


def test_offer_latest_discards_stale_frame():
    queue = asyncio.Queue(maxsize=1)

    offer_latest(queue, b"old")
    offer_latest(queue, b"new")

    assert queue.get_nowait() == b"new"
    assert queue.empty()


def test_risk_reporter_sends_initial_and_periodic_commands():
    reporter = RiskReporter(refresh_seconds=1.0)

    first = reporter.next_message("low", people=1, now=10.0)
    assert json.loads(first) == {
        "type": "risk",
        "command_id": 1,
        "level": "low",
        "people": 1,
        "smoking": 0,
        "smoking_confidence": 0.0,
    }
    assert reporter.next_message("low", people=2, now=10.9) is None

    refreshed = reporter.next_message("low", people=2, now=11.0)
    assert json.loads(refreshed)["command_id"] == 2


def test_risk_reporter_sends_changed_level_immediately():
    reporter = RiskReporter(refresh_seconds=1.0)
    reporter.next_message("low", people=0, now=10.0)

    message = reporter.next_message(
        "medium",
        people=4,
        now=10.1,
        changed=True,
        smoking=1,
        smoking_confidence=0.87654,
    )

    assert json.loads(message) == {
        "type": "risk",
        "command_id": 2,
        "level": "medium",
        "people": 4,
        "smoking": 1,
        "smoking_confidence": 0.8765,
    }


@pytest.mark.parametrize("confidence", [-0.1, 1.1, True, "0.8"])
def test_risk_reporter_rejects_invalid_smoking_confidence(confidence):
    reporter = RiskReporter()

    with pytest.raises(ValueError):
        reporter.next_message(
            "medium",
            people=1,
            smoking=1,
            smoking_confidence=confidence,
            now=0.0,
        )


@pytest.mark.parametrize("level", ["unknown", "", None])
def test_risk_reporter_rejects_invalid_level(level):
    reporter = RiskReporter()

    with pytest.raises(ValueError):
        reporter.next_message(level, people=0, now=0.0)


def test_parse_ack_accepts_protocol_ack():
    ack = parse_ack('{"type":"ack","command_id":7,"applied":true}')

    assert ack == {"command_id": 7, "applied": True, "error": None}


@pytest.mark.parametrize(
    "message",
    [
        "not json",
        '{"type":"other"}',
        '{"type":"ack","command_id":1.5,"applied":true}',
        '{"type":"ack","command_id":1,"applied":"yes"}',
    ],
)
def test_parse_ack_ignores_invalid_messages(message):
    assert parse_ack(message) is None
