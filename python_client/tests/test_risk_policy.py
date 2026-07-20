import pytest

from python_client.risk_policy import RiskPolicy, classify_risk


@pytest.mark.parametrize(
    ("people", "expected"),
    [
        (0, "low"),
        (2, "low"),
        (3, "medium"),
        (5, "medium"),
        (6, "high"),
        (20, "high"),
    ],
)
def test_classify_risk_uses_people_thresholds(people, expected):
    assert classify_risk(people) == expected


def test_classify_risk_rejects_negative_people_count():
    with pytest.raises(ValueError):
        classify_risk(-1)


def test_policy_changes_after_three_consecutive_frames():
    policy = RiskPolicy(required_frames=3)

    assert policy.observe(4) is None
    assert policy.observe(4) is None
    assert policy.observe(4) == "medium"
    assert policy.current_level == "medium"
    assert policy.observe(4) is None


def test_policy_resets_candidate_when_frame_level_changes():
    policy = RiskPolicy(required_frames=3)

    assert policy.observe(4) is None
    assert policy.observe(4) is None
    assert policy.observe(7) is None
    assert policy.observe(7) is None
    assert policy.observe(7) == "high"


def test_policy_debounces_return_to_low():
    policy = RiskPolicy(required_frames=3)

    for people in (7, 6, 8):
        changed = policy.observe(people)
    assert changed == "high"

    assert policy.observe(0) is None
    assert policy.observe(2) is None
    assert policy.observe(1) == "low"


def test_policy_rejects_invalid_required_frame_count():
    with pytest.raises(ValueError):
        RiskPolicy(required_frames=0)
