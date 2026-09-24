"""Smoking-risk classification and frame-to-frame debounce policy."""

from dataclasses import dataclass, field
from typing import Optional


def classify_risk(smoking: int) -> str:
    """Map the number of detected smokers to a risk level."""
    if smoking < 0:
        raise ValueError("smoking must not be negative")
    if smoking >= 2:
        return "high"
    if smoking >= 1:
        return "medium"
    return "low"


@dataclass
class RiskPolicy:
    """Commit a new risk level only after it remains stable for N frames."""

    required_frames: int = 3
    current_level: str = field(default="low", init=False)
    _candidate_level: Optional[str] = field(default=None, init=False)
    _candidate_frames: int = field(default=0, init=False)

    def __post_init__(self) -> None:
        if self.required_frames < 1:
            raise ValueError("required_frames must be at least 1")

    def observe(self, smoking: int) -> Optional[str]:
        """Return a newly committed level, or None when state is unchanged."""
        level = classify_risk(smoking)
        if level == self.current_level:
            self._candidate_level = None
            self._candidate_frames = 0
            return None

        if level == "low":
            self.current_level = "low"
            self._candidate_level = None
            self._candidate_frames = 0
            return level

        if level != self._candidate_level:
            self._candidate_level = level
            self._candidate_frames = 1
            return None

        self._candidate_frames += 1
        if self._candidate_frames < self.required_frames:
            return None

        self.current_level = level
        self._candidate_level = None
        self._candidate_frames = 0
        return level
