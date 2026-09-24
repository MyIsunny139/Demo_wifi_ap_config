import pytest

from python_client.risk_policy import RiskPolicy, classify_risk


@pytest.mark.parametrize(
    ("people", "expected"),
    [
        (0, "low"),
        (1, "medium"),
        (2, "high"),
        (6, "high"),
        (20, "high"),
    ],
)
def test_classify_risk_uses_smoking_thresholds(people, expected):
    assert classify_risk(people) == expected


def test_classify_risk_rejects_negative_people_count():
    with pytest.raises(ValueError):
        classify_risk(-1)


def test_policy_changes_after_three_consecutive_frames():
    policy = RiskPolicy(required_frames=3)

    assert policy.observe(1) is None
    assert policy.observe(1) is None
    assert policy.observe(1) == "medium"
    assert policy.current_level == "medium"
    assert policy.observe(1) is None


def test_policy_resets_candidate_when_frame_level_changes():
    policy = RiskPolicy(required_frames=3)

    assert policy.observe(1) is None
    assert policy.observe(1) is None
    assert policy.observe(2) is None
    assert policy.observe(2) is None
    assert policy.observe(2) == "high"


def test_policy_returns_to_low_immediately():
    policy = RiskPolicy(required_frames=3)

    for smoking in (2, 3, 2):
        changed = policy.observe(smoking)
    assert changed == "high"

    assert policy.observe(0) == "low"
    assert policy.current_level == "low"


def test_policy_rejects_invalid_required_frame_count():
    with pytest.raises(ValueError):
        RiskPolicy(required_frames=0)
