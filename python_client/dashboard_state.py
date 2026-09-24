"""State model for the browser dashboard."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

RiskLevel = Literal["low", "medium", "high"]
ESP32_INT_MAX = 2_147_483_647


@dataclass(frozen=True)
class Thresholds:
    medium_smoking: int = 1
    high_smoking: int = 2

    def __post_init__(self) -> None:
        if type(self.medium_smoking) is not int:
            raise ValueError("medium_smoking must be an integer")
        if type(self.high_smoking) is not int:
            raise ValueError("high_smoking must be an integer")
        if self.medium_smoking < 1:
            raise ValueError("medium_smoking must be at least 1")
        if self.high_smoking <= self.medium_smoking:
            raise ValueError("high_smoking must be greater than medium_smoking")

    def classify(self, smoking: int) -> RiskLevel:
        if type(smoking) is not int or smoking < 0:
            raise ValueError("smoking must be a non-negative integer")
        if smoking >= self.high_smoking:
            return "high"
        if smoking >= self.medium_smoking:
            return "medium"
        return "low"


@dataclass(frozen=True)
class SaveResult:
    image_path: Path
    metadata_path: Path


def build_risk_command(
    command_id: int,
    level: RiskLevel,
    people: int,
    smoking: int = 0,
    smoking_confidence: float = 0.0,
) -> str:
    if type(command_id) is not int or not 0 <= command_id <= ESP32_INT_MAX:
        raise ValueError(f"command_id must be an integer from 0 to {ESP32_INT_MAX}")
    if type(people) is not int or not 0 <= people <= ESP32_INT_MAX:
        raise ValueError(f"people must be an integer from 0 to {ESP32_INT_MAX}")
    if type(smoking) is not int or not 0 <= smoking <= ESP32_INT_MAX:
        raise ValueError(f"smoking must be an integer from 0 to {ESP32_INT_MAX}")
    if (
        isinstance(smoking_confidence, bool)
        or not isinstance(smoking_confidence, (int, float))
        or not 0.0 <= smoking_confidence <= 1.0
    ):
        raise ValueError("smoking_confidence must be in [0, 1]")
    if level not in {"low", "medium", "high"}:
        raise ValueError("invalid risk level")
    return json.dumps(
        {
            "type": "risk",
            "command_id": command_id,
            "level": level,
            "people": people,
            "smoking": smoking,
            "smoking_confidence": round(smoking_confidence, 4),
        },
        separators=(",", ":"),
    )


@dataclass
class DashboardState:
    thresholds: Thresholds = field(default_factory=Thresholds)
    running: bool = False
    connected: bool = False
    processing: str = "idle"
    people: int = 0
    smoking: int = 0
    smoking_confidence: float = 0.0
    risk: RiskLevel = "low"
    warning: str = "等待启动"
    receive_fps: float = 0.0
    inference_fps: float = 0.0
    last_error: str | None = None
    last_saved: str | None = None
    updated_at: float = field(default_factory=time.time)
    latest_frame: bytes | None = field(default=None, repr=False)
    frame_sequence: int = 0

    def set_thresholds(self, thresholds: Thresholds) -> None:
        self.thresholds = thresholds
        self.updated_at = time.time()

    def update_preview_frame(self, jpeg: bytes, receive_fps: float) -> None:
        self.latest_frame = jpeg
        self.frame_sequence += 1
        self.receive_fps = receive_fps
        self.last_error = None
        self.updated_at = time.time()

    def update_detection(
        self,
        people: int,
        smoking: int,
        smoking_confidence: float,
        risk: RiskLevel,
        inference_fps: float,
        warning: str,
    ) -> None:
        self.people = people
        self.smoking = smoking
        self.smoking_confidence = smoking_confidence
        self.risk = risk
        self.inference_fps = inference_fps
        self.warning = warning
        self.last_error = None
        self.updated_at = time.time()

    def update_frame(
        self,
        jpeg: bytes,
        people: int,
        smoking: int,
        smoking_confidence: float,
        risk: RiskLevel,
        receive_fps: float,
        inference_fps: float,
        warning: str,
    ) -> None:
        self.update_preview_frame(jpeg, receive_fps)
        self.update_detection(
            people, smoking, smoking_confidence, risk, inference_fps, warning
        )

    def snapshot(self) -> dict[str, object]:
        return {
            "running": self.running,
            "connected": self.connected,
            "processing": self.processing,
            "people": self.people,
            "smoking": self.smoking,
            "smoking_confidence": self.smoking_confidence,
            "risk": self.risk,
            "warning": self.warning,
            "receive_fps": self.receive_fps,
            "inference_fps": self.inference_fps,
            "medium_smoking": self.thresholds.medium_smoking,
            "high_smoking": self.thresholds.high_smoking,
            "last_error": self.last_error,
            "last_saved": self.last_saved,
            "updated_at": self.updated_at,
            "has_frame": self.latest_frame is not None,
            "frame_sequence": self.frame_sequence,
        }

    def save_latest_frame(self, directory: Path, stem: str) -> SaveResult:
        if self.latest_frame is None:
            raise RuntimeError("no frame available")
        directory.mkdir(parents=True, exist_ok=True)
        image_path = directory / f"{stem}.jpg"
        metadata_path = directory / f"{stem}.json"
        image_path.write_bytes(self.latest_frame)
        metadata_path.write_text(
            json.dumps(
                {
                    "people": self.people,
                    "smoking": self.smoking,
                    "smoking_confidence": self.smoking_confidence,
                    "risk": self.risk,
                    "warning": self.warning,
                    "receive_fps": self.receive_fps,
                    "inference_fps": self.inference_fps,
                    "updated_at": self.updated_at,
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
        self.last_saved = str(image_path)
        self.updated_at = time.time()
        return SaveResult(image_path=image_path, metadata_path=metadata_path)
