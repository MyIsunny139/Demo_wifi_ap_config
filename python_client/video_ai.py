"""Receive ESP32 JPEG frames, run person detection, and report risk."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Any, Iterable, Optional

from .risk_policy import RiskPolicy


LOGGER = logging.getLogger("video_ai")
RISK_LEVELS = {"low", "medium", "high"}
SMOKING_CLASS_ALIASES = frozenset(
    {
        "smoking",
        "smooking",
        "smoke",
        "smoker",
        "cigarette",
        "person smoking",
        "smoking person",
        "smoking behavior",
        "cigarette smoking",
        "抽烟",
        "吸烟",
    }
)
_NATIVE_STDERR_LOCK = threading.Lock()


def normalize_class_label(label: str) -> str:
    return " ".join(label.strip().lower().replace("_", "-").replace("-", " ").split())


def resolve_detection_classes(
    names: dict[int, str] | list[str] | tuple[str, ...],
    smoking_class_names: Iterable[str] = SMOKING_CLASS_ALIASES,
) -> tuple[set[int], set[int]]:
    if isinstance(names, dict):
        model_names = {int(class_id): str(name) for class_id, name in names.items()}
    else:
        model_names = {class_id: str(name) for class_id, name in enumerate(names)}

    normalized_aliases = {normalize_class_label(name) for name in smoking_class_names}
    smoking_class_ids = {
        class_id
        for class_id, name in model_names.items()
        if normalize_class_label(name) in normalized_aliases
    }
    person_class_ids = {
        class_id
        for class_id, name in model_names.items()
        if normalize_class_label(name) == "person"
    }
    if not smoking_class_ids:
        available = ", ".join(model_names.values())
        raise ValueError(
            "模型不包含抽烟类别（支持 smoking/smooking/smoke/smoker/cigarette/person smoking/抽烟/吸烟）；"
            f"当前类别：{available}"
        )
    return person_class_ids, smoking_class_ids


def configure_inference_logging() -> None:
    """Keep model libraries quiet during continuous frame inference."""
    os.environ.setdefault("YOLO_VERBOSE", "False")
    try:
        from ultralytics.utils import LOGGER as ultralytics_logger
    except Exception:
        return
    ultralytics_logger.setLevel(logging.ERROR)


@contextmanager
def _suppress_native_stderr():
    with _NATIVE_STDERR_LOCK:
        saved_stderr = os.dup(2)
        try:
            with open(os.devnull, "w") as devnull:
                os.dup2(devnull.fileno(), 2)
                yield
        finally:
            os.dup2(saved_stderr, 2)
            os.close(saved_stderr)


def decode_jpeg_frame(cv2: Any, np: Any, jpeg: bytes) -> Any:
    encoded = np.frombuffer(jpeg, dtype=np.uint8)
    with _suppress_native_stderr():
        return cv2.imdecode(encoded, cv2.IMREAD_COLOR)


def sanitize_jpeg_frame(frame: bytes) -> Optional[bytes]:
    start = frame.find(b"\xff\xd8\xff")
    if start < 0:
        return None
    end = frame.rfind(b"\xff\xd9")
    if end <= start:
        return None
    return frame[start : end + 2]


def is_probably_jpeg(frame: bytes) -> bool:
    return sanitize_jpeg_frame(frame) == frame


def offer_latest(queue: asyncio.Queue[bytes], frame: bytes) -> None:
    """Add a frame without blocking, replacing an unprocessed older frame."""
    if queue.full():
        try:
            queue.get_nowait()
        except asyncio.QueueEmpty:
            pass
    try:
        queue.put_nowait(frame)
    except asyncio.QueueFull:
        pass


def parse_ack(message: str) -> Optional[dict[str, Any]]:
    """Parse a valid ESP32 acknowledgement, ignoring other text messages."""
    try:
        payload = json.loads(message)
    except (json.JSONDecodeError, TypeError):
        return None

    command_id = payload.get("command_id") if isinstance(payload, dict) else None
    applied = payload.get("applied") if isinstance(payload, dict) else None
    if (
        not isinstance(payload, dict)
        or payload.get("type") != "ack"
        or type(command_id) is not int
        or command_id < 0
        or type(applied) is not bool
    ):
        return None

    error = payload.get("error")
    if error is not None and not isinstance(error, str):
        return None
    return {"command_id": command_id, "applied": applied, "error": error}


class RiskReporter:
    """Create immediate state-change commands and periodic refresh commands."""

    def __init__(self, refresh_seconds: float = 1.0) -> None:
        if refresh_seconds <= 0:
            raise ValueError("refresh_seconds must be positive")
        self._refresh_seconds = refresh_seconds
        self._next_command_id = 1
        self._last_sent_at: Optional[float] = None

    def start_connection(self) -> None:
        self._last_sent_at = None

    def next_message(
        self,
        level: str,
        people: int,
        now: float,
        changed: bool = False,
        smoking: int = 0,
        smoking_confidence: float = 0.0,
    ) -> Optional[str]:
        if level not in RISK_LEVELS:
            raise ValueError(f"invalid risk level: {level}")
        if type(people) is not int or people < 0:
            raise ValueError("people must be a non-negative integer")
        if type(smoking) is not int or smoking < 0:
            raise ValueError("smoking must be a non-negative integer")
        if (
            isinstance(smoking_confidence, bool)
            or not isinstance(smoking_confidence, (int, float))
            or not 0.0 <= smoking_confidence <= 1.0
        ):
            raise ValueError("smoking_confidence must be in [0, 1]")

        refresh_due = (
            self._last_sent_at is None
            or now - self._last_sent_at >= self._refresh_seconds
        )
        if not changed and not refresh_due:
            return None

        command_id = self._next_command_id
        self._next_command_id = 1 if command_id >= 2_147_483_647 else command_id + 1
        self._last_sent_at = now
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
class StreamRate:
    receive_fps: float = 0.0
    _last_frame_at: Optional[float] = None

    def record_frame(self, now: float) -> None:
        if self._last_frame_at is not None and now > self._last_frame_at:
            instantaneous = 1.0 / (now - self._last_frame_at)
            self.receive_fps = (
                instantaneous
                if self.receive_fps == 0.0
                else self.receive_fps * 0.8 + instantaneous * 0.2
            )
        self._last_frame_at = now


@dataclass(frozen=True)
class Detection:
    people: int
    smoking: int
    smoking_confidence: float
    image: Any


class PersonDetector:
    """Run a smoking-trained YOLO model and summarize its behavior detections."""

    def __init__(
        self,
        model_path: str,
        confidence: float,
        device: Optional[str],
        smoking_class_names: Iterable[str] = SMOKING_CLASS_ALIASES,
    ) -> None:
        configure_inference_logging()
        import cv2
        import numpy as np
        from ultralytics import YOLO

        configure_inference_logging()
        self.cv2 = cv2
        self.np = np
        self.model = YOLO(model_path)
        self.confidence = confidence
        self.device = device
        self.person_class_ids, self.smoking_class_ids = resolve_detection_classes(
            self.model.names, smoking_class_names
        )
        self.inference_class_ids = sorted(self.smoking_class_ids | self.person_class_ids)

    @staticmethod
    def _tensor_values(values: Any) -> list[float]:
        if values is None:
            return []
        if hasattr(values, "detach"):
            values = values.detach()
        if hasattr(values, "cpu"):
            values = values.cpu()
        if hasattr(values, "tolist"):
            values = values.tolist()
        return [float(value) for value in values]

    def detect(self, jpeg: bytes) -> Detection:
        image = decode_jpeg_frame(self.cv2, self.np, jpeg)
        if image is None:
            raise ValueError("received an invalid JPEG frame")

        result = self.model.predict(
            source=image,
            classes=self.inference_class_ids,
            conf=self.confidence,
            device=self.device,
            verbose=False,
        )[0]
        if result.boxes is None:
            class_ids: list[int] = []
            confidences: list[float] = []
        else:
            class_ids = [int(value) for value in self._tensor_values(result.boxes.cls)]
            confidences = self._tensor_values(result.boxes.conf)

        smoking_confidences = [
            confidence
            for class_id, confidence in zip(class_ids, confidences)
            if class_id in self.smoking_class_ids
        ]
        smoking = len(smoking_confidences)
        detected_people = sum(class_id in self.person_class_ids for class_id in class_ids)
        people = max(detected_people, smoking)
        return Detection(
            people=people,
            smoking=smoking,
            smoking_confidence=max(smoking_confidences, default=0.0),
            image=result.plot(),
        )


async def receive_stream(websocket: Any, frames: asyncio.Queue[bytes], rate: StreamRate) -> None:
    async for message in websocket:
        if isinstance(message, bytes):
            jpeg = sanitize_jpeg_frame(message)
            if jpeg is None:
                LOGGER.warning("Dropped invalid JPEG frame from ESP32")
                continue
            if len(jpeg) != len(message):
                LOGGER.warning("Trimmed invalid bytes from ESP32 JPEG frame")
            rate.record_frame(time.monotonic())
            offer_latest(frames, jpeg)
            continue

        ack = parse_ack(message)
        if ack and not ack["applied"]:
            LOGGER.warning(
                "Command %d rejected: %s", ack["command_id"], ack["error"] or "unknown"
            )


def draw_status(detector: PersonDetector, detection: Detection, level: str,
                receive_fps: float, inference_fps: float) -> None:
    text = (
        f"People: {detection.people}  Smoking: {detection.smoking}  "
        f"Confidence: {detection.smoking_confidence:.0%}  Risk: {level.upper()}  "
        f"RX: {receive_fps:.1f} FPS  AI: {inference_fps:.1f} FPS"
    )
    detector.cv2.rectangle(detection.image, (0, 0), (detection.image.shape[1], 34),
                           (24, 24, 24), -1)
    detector.cv2.putText(detection.image, text, (10, 23), detector.cv2.FONT_HERSHEY_SIMPLEX,
                         0.55, (255, 255, 255), 1, detector.cv2.LINE_AA)


async def process_stream(
    websocket: Any,
    frames: asyncio.Queue[bytes],
    rate: StreamRate,
    detector: PersonDetector,
    policy: RiskPolicy,
    reporter: RiskReporter,
    stop_event: asyncio.Event,
) -> None:
    while not stop_event.is_set():
        jpeg = await frames.get()
        started = time.monotonic()
        try:
            detection = await asyncio.to_thread(detector.detect, jpeg)
        except ValueError as error:
            LOGGER.warning("Frame decode failed: %s", error)
            continue

        elapsed = max(time.monotonic() - started, 1e-6)
        changed_level = policy.observe(detection.smoking)
        message = reporter.next_message(
            policy.current_level,
            detection.people,
            time.monotonic(),
            changed=changed_level is not None,
            smoking=detection.smoking,
            smoking_confidence=detection.smoking_confidence,
        )
        if message is not None:
            await websocket.send(message)
            if changed_level is not None:
                LOGGER.info(
                    "Risk changed to %s (people=%d smoking=%d confidence=%.3f)",
                    changed_level,
                    detection.people,
                    detection.smoking,
                    detection.smoking_confidence,
                )

        draw_status(detector, detection, policy.current_level, rate.receive_fps, 1.0 / elapsed)
        detector.cv2.imshow("video-ai", detection.image)
        if detector.cv2.waitKey(1) & 0xFF == ord("q"):
            stop_event.set()
            return


async def run_connection(
    websocket: Any,
    detector: PersonDetector,
    policy: RiskPolicy,
    reporter: RiskReporter,
    stop_event: asyncio.Event,
) -> None:
    frames: asyncio.Queue[bytes] = asyncio.Queue(maxsize=1)
    rate = StreamRate()
    reporter.start_connection()
    tasks = {
        asyncio.create_task(receive_stream(websocket, frames, rate)),
        asyncio.create_task(
            process_stream(websocket, frames, rate, detector, policy, reporter, stop_event)
        ),
    }
    done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    for task in pending:
        task.cancel()
    await asyncio.gather(*pending, return_exceptions=True)
    for task in done:
        task.result()


async def run_client(args: argparse.Namespace) -> None:
    import websockets

    uri = f"ws://{args.host}:{args.port}/video"
    detector = PersonDetector(args.model, args.confidence, args.device)
    policy = RiskPolicy(required_frames=3)
    reporter = RiskReporter(refresh_seconds=1.0)
    stop_event = asyncio.Event()

    try:
        while not stop_event.is_set():
            try:
                LOGGER.info("Connecting to %s", uri)
                async with websockets.connect(
                    uri,
                    max_size=4 * 1024 * 1024,
                    open_timeout=10,
                    close_timeout=2,
                    ping_interval=20,
                    ping_timeout=20,
                ) as websocket:
                    LOGGER.info("Connected")
                    await run_connection(websocket, detector, policy, reporter, stop_event)
            except asyncio.CancelledError:
                raise
            except Exception as error:
                if not stop_event.is_set():
                    LOGGER.warning("Connection ended: %s", error)

            if not stop_event.is_set():
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=args.reconnect_delay)
                except asyncio.TimeoutError:
                    pass
    finally:
        detector.cv2.destroyAllWindows()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="ESP32 IPv4 address")
    parser.add_argument("--port", type=int, default=81, help="WebSocket port")
    parser.add_argument(
        "--model", default="smoking.onnx", help="Smoking-trained Ultralytics model path"
    )
    parser.add_argument("--confidence", type=float, default=0.4, help="Detection confidence")
    parser.add_argument("--device", default=None, help="Ultralytics device, for example 0 or cpu")
    parser.add_argument("--reconnect-delay", type=float, default=2.0)
    args = parser.parse_args()
    if not 0.0 < args.confidence <= 1.0:
        parser.error("--confidence must be in (0, 1]")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in [1, 65535]")
    if args.reconnect_delay <= 0:
        parser.error("--reconnect-delay must be positive")
    return args


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        asyncio.run(run_client(parse_args()))
    except KeyboardInterrupt:
        LOGGER.info("Stopped")


if __name__ == "__main__":
    main()
