"""Receive ESP32 JPEG frames, run person detection, and report risk."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import time
from dataclasses import dataclass
from typing import Any, Optional

from .risk_policy import RiskPolicy


LOGGER = logging.getLogger("video_ai")
RISK_LEVELS = {"low", "medium", "high"}


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
    ) -> Optional[str]:
        if level not in RISK_LEVELS:
            raise ValueError(f"invalid risk level: {level}")
        if type(people) is not int or people < 0:
            raise ValueError("people must be a non-negative integer")

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
    image: Any


class PersonDetector:
    """Own the YOLO model and OpenCV decoding used by the worker thread."""

    def __init__(self, model_path: str, confidence: float, device: Optional[str]) -> None:
        import cv2
        import numpy as np
        from ultralytics import YOLO

        self.cv2 = cv2
        self.np = np
        self.model = YOLO(model_path)
        self.confidence = confidence
        self.device = device

    def detect(self, jpeg: bytes) -> Detection:
        encoded = self.np.frombuffer(jpeg, dtype=self.np.uint8)
        image = self.cv2.imdecode(encoded, self.cv2.IMREAD_COLOR)
        if image is None:
            raise ValueError("received an invalid JPEG frame")

        result = self.model.predict(
            source=image,
            classes=[0],
            conf=self.confidence,
            device=self.device,
            verbose=False,
        )[0]
        people = 0 if result.boxes is None else len(result.boxes)
        return Detection(people=people, image=result.plot())


async def receive_stream(websocket: Any, frames: asyncio.Queue[bytes], rate: StreamRate) -> None:
    async for message in websocket:
        if isinstance(message, bytes):
            rate.record_frame(time.monotonic())
            offer_latest(frames, message)
            continue

        ack = parse_ack(message)
        if ack and not ack["applied"]:
            LOGGER.warning(
                "Command %d rejected: %s", ack["command_id"], ack["error"] or "unknown"
            )


def draw_status(detector: PersonDetector, detection: Detection, level: str,
                receive_fps: float, inference_fps: float) -> None:
    text = (
        f"People: {detection.people}  Risk: {level.upper()}  "
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
        changed_level = policy.observe(detection.people)
        message = reporter.next_message(
            policy.current_level,
            detection.people,
            time.monotonic(),
            changed=changed_level is not None,
        )
        if message is not None:
            await websocket.send(message)
            if changed_level is not None:
                LOGGER.info("Risk changed to %s (%d people)", changed_level, detection.people)

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
    parser.add_argument("--model", default="yolo11n.pt", help="Ultralytics model path")
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
