"""State model for the browser dashboard."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

RiskLevel = Literal["low", "medium", "high"]
ESP32_INT_MAX = 2_147_483_647


@dataclass(frozen=True, init=False)
class Thresholds:
    max_people: int = 3
    temperature_max: float = 35.0
    humidity_max: float = 70.0
    mq2_max: int = 1800
    _legacy_medium: int | None = field(default=None, repr=False, compare=False)
    _legacy_high: int | None = field(default=None, repr=False, compare=False)

    def __init__(
        self,
        max_people: int = 3,
        *,
        temperature_max: float = 35.0,
        humidity_max: float = 70.0,
        mq2_max: int = 1800,
        medium_smoking: int | None = None,
        high_smoking: int | None = None,
    ) -> None:
        if medium_smoking is not None or high_smoking is not None:
            if type(medium_smoking) is not int or type(high_smoking) is not int:
                raise ValueError("legacy thresholds must be integers")
            if medium_smoking < 1 or high_smoking <= medium_smoking:
                raise ValueError("legacy thresholds must satisfy 1 <= medium < high")
            max_people = medium_smoking - 1
        object.__setattr__(self, "max_people", max_people)
        object.__setattr__(self, "temperature_max", temperature_max)
        object.__setattr__(self, "humidity_max", humidity_max)
        object.__setattr__(self, "mq2_max", mq2_max)
        object.__setattr__(self, "_legacy_medium", medium_smoking)
        object.__setattr__(self, "_legacy_high", high_smoking)
        self.__post_init__()

    def __post_init__(self) -> None:
        if type(self.max_people) is not int:
            raise ValueError("max_people must be an integer")
        if not 0 <= self.max_people < ESP32_INT_MAX:
            raise ValueError(f"max_people must be from 0 to {ESP32_INT_MAX - 1}")
        for name, value, upper in (("temperature_max", self.temperature_max, 125.0),
                                   ("humidity_max", self.humidity_max, 100.0)):
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise ValueError(f"{name} must be numeric")
            if not 0.0 <= float(value) <= upper:
                raise ValueError(f"{name} must be from 0 to {upper:g}")
        if type(self.mq2_max) is not int or not 0 <= self.mq2_max <= 4095:
            raise ValueError("mq2_max must be an integer from 0 to 4095")

    def classify(self, people: int) -> RiskLevel:
        if type(people) is not int or people < 0:
            raise ValueError("people must be a non-negative integer")
        if self._legacy_medium is not None and self._legacy_high is not None:
            if people >= self._legacy_high:
                return "high"
            if people >= self._legacy_medium:
                return "medium"
            return "low"
        if people > self.max_people + 1:
            return "high"
        if people > self.max_people:
            return "medium"
        return "low"

    def classify_environment(
        self,
        temperature_c: float | None,
        humidity_pct: float | None,
        mq2_adc: int | None,
        dht_valid: bool,
    ) -> RiskLevel:
        if ((dht_valid and temperature_c is not None and temperature_c > self.temperature_max)
                or (dht_valid and humidity_pct is not None and humidity_pct > self.humidity_max)
                or (mq2_adc is not None and mq2_adc > self.mq2_max)):
            return "high"
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
    dht_valid: bool = False
    temperature_c: float | None = None
    humidity_pct: float | None = None
    mq2_adc: int | None = None
    environment_sampled_at_ms: int | None = None
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

    def update_preview_frame(
        self,
        jpeg: bytes,
        receive_fps: float,
        frame_sequence: int | None = None,
    ) -> None:
        if frame_sequence is not None and (
            type(frame_sequence) is not int or frame_sequence < 0
        ):
            raise ValueError("frame_sequence must be a non-negative integer")
        self.latest_frame = jpeg
        self.frame_sequence = (
            self.frame_sequence + 1 if frame_sequence is None else frame_sequence
        )
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

    def update_environment(
        self,
        dht_valid: bool,
        temperature_c: float | None,
        humidity_pct: float | None,
        mq2_adc: int | None,
        sampled_at_ms: int | None = None,
    ) -> None:
        self.dht_valid = dht_valid
        self.temperature_c = temperature_c
        self.humidity_pct = humidity_pct
        self.mq2_adc = mq2_adc
        self.environment_sampled_at_ms = sampled_at_ms
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
        snapshot = {
            "running": self.running,
            "connected": self.connected,
            "processing": self.processing,
            "people": self.people,
            "smoking": self.smoking,
            "smoking_confidence": self.smoking_confidence,
            "dht_valid": self.dht_valid,
            "temperature_c": self.temperature_c,
            "humidity_pct": self.humidity_pct,
            "mq2_adc": self.mq2_adc,
            "environment_sampled_at_ms": self.environment_sampled_at_ms,
            "risk": self.risk,
            "warning": self.warning,
            "receive_fps": self.receive_fps,
            "inference_fps": self.inference_fps,
            "max_people": self.thresholds.max_people,
            "temperature_max": self.thresholds.temperature_max,
            "humidity_max": self.thresholds.humidity_max,
            "mq2_max": self.thresholds.mq2_max,
            "last_error": self.last_error,
            "last_saved": self.last_saved,
            "updated_at": self.updated_at,
            "has_frame": self.latest_frame is not None,
            "frame_sequence": self.frame_sequence,
        }
        if self.thresholds._legacy_medium is not None:
            snapshot["medium_smoking"] = self.thresholds._legacy_medium
            snapshot["high_smoking"] = self.thresholds._legacy_high
        return snapshot

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
