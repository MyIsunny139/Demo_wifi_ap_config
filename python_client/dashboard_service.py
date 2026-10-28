"""Runtime service behind the browser dashboard."""

from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import time
import weakref
from pathlib import Path
from typing import Any, Callable

from .dashboard_state import DashboardState, SaveResult, Thresholds
from .video_ai import (
    Detection,
    PersonDetector,
    RiskReporter,
    StreamRate,
    offer_latest,
    parse_ack,
    sanitize_jpeg_frame,
)


DIAGNOSTIC_LOGGER = logging.getLogger("video_ai.stream_diagnostics")


class _LevelPolicy:
    """Debounce already classified levels without owning threshold values."""

    def __init__(self, required_frames: int = 3) -> None:
        self._required_frames = required_frames
        self.reset()

    def reset(self) -> None:
        self.current_level = "low"
        self._candidate_level: str | None = None
        self._candidate_frames = 0

    def observe(self, level: str) -> bool:
        if level == self.current_level:
            self._candidate_level = None
            self._candidate_frames = 0
            return False
        if level == "low":
            self.current_level = "low"
            self._candidate_level = None
            self._candidate_frames = 0
            return True
        if level != self._candidate_level:
            self._candidate_level = level
            self._candidate_frames = 1
            return False
        self._candidate_frames += 1
        if self._candidate_frames < self._required_frames:
            return False
        self.current_level = level
        self._candidate_level = None
        self._candidate_frames = 0
        return True


class DashboardService:
    BROWSER_SEND_TIMEOUT = 1.0

    def __init__(
        self,
        host: str,
        port: int = 81,
        model: str = "yolo11n.pt",
        confidence: float = 0.4,
        device: str | None = None,
        reconnect_delay: float = 2.0,
        capture_dir: Path | str = "captures",
        connector: Callable[..., Any] | None = None,
        detector_factory: Callable[..., PersonDetector] | None = None,
    ) -> None:
        self.host = host
        self.port = port
        self.model = model
        self.confidence = confidence
        self.device = device
        self.reconnect_delay = reconnect_delay
        self.capture_dir = Path(capture_dir)
        self.state = DashboardState()
        self._browser_clients: set[Any] = set()
        self._stop_event = asyncio.Event()
        self._runner: asyncio.Task[None] | None = None
        self._command_lock = asyncio.Lock()
        self._broadcast_lock = asyncio.Lock()
        self._detached_browser_tasks: set[asyncio.Task[Any]] = set()
        self._detached_browser_owners: dict[asyncio.Task[Any], int] = {}
        self._blocked_browser_ids: set[int] = set()
        self._worker_tasks: set[asyncio.Task[Any]] = set()
        self._reporter = RiskReporter(refresh_seconds=1.0)
        self._policy = _LevelPolicy(required_frames=3)
        self._capture_sequence = 0
        self._rx_sequence = 0
        self._last_rx_at: float | None = None
        self._frame_metadata: dict[str, tuple[int, float]] = {}
        self._preview_frames: dict[int, bytes] = {}
        self._browser_frame_requests = 0
        self._pending_commands: dict[int, float] = {}
        self._connector = connector
        self._detector_factory = detector_factory or PersonDetector

    @property
    def uri(self) -> str:
        return f"ws://{self.host}:{self.port}/video"

    def add_browser(self, websocket: Any) -> bool:
        if id(websocket) in self._blocked_browser_ids:
            self._browser_clients.discard(websocket)
            return False
        self._browser_clients.add(websocket)
        return True

    def remove_browser(self, websocket: Any) -> None:
        self._browser_clients.discard(websocket)

    async def start(self) -> None:
        if self.state.running:
            return
        DIAGNOSTIC_LOGGER.info("SERVICE start requested uri=%s", self.uri)
        self._stop_event = asyncio.Event()
        self.state.running = True
        self.state.processing = "starting"
        self.state.warning = "正在连接 ESP32"
        self._runner = asyncio.create_task(self._run_forever())
        await self.broadcast()

    async def stop(self) -> None:
        DIAGNOSTIC_LOGGER.info("SERVICE stop requested")
        self._stop_event.set()
        if self._runner is not None:
            self._runner.cancel()
            await asyncio.gather(self._runner, return_exceptions=True)
            self._runner = None
        await self._wait_workers()
        self.state.running = False
        self.state.connected = False
        self.state.processing = "idle"
        self.state.warning = "已停止"
        await self.broadcast()
        for task in tuple(self._detached_browser_tasks):
            task.cancel()
        self._detached_browser_tasks.clear()
        self._detached_browser_owners.clear()

    async def update_thresholds(
        self,
        max_people: int | None = None,
        *,
        temperature_max: float = 35.0,
        humidity_max: float = 70.0,
        mq2_max: int = 1800,
        medium_smoking: int | None = None,
        high_smoking: int | None = None,
    ) -> None:
        if max_people is None and medium_smoking is not None:
            thresholds = Thresholds(
                medium_smoking=medium_smoking,
                high_smoking=high_smoking,
                temperature_max=temperature_max,
                humidity_max=humidity_max,
                mq2_max=mq2_max,
            )
        else:
            thresholds = Thresholds(
                max_people=max_people if max_people is not None else 3,
                temperature_max=temperature_max,
                humidity_max=humidity_max,
                mq2_max=mq2_max,
            )
        self.state.set_thresholds(thresholds)
        await self.broadcast()

    async def update_connection(self, host: str, port: int) -> None:
        if self.state.running:
            raise RuntimeError("请先停止处理，再修改 ESP32 地址")
        host = host.strip()
        if (not host or len(host) > 253 or any(char.isspace() for char in host) or
                ":" in host or "/" in host):
            raise ValueError("请输入有效的 ESP32 IP 地址或主机名")
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("ESP32 端口必须在 1 到 65535 之间")
        self.host = host
        self.port = port
        self.state.last_error = None
        self.state.warning = f"ESP32 地址已设置为 {host}:{port}"
        self.state.updated_at = time.time()
        await self.broadcast()

    async def save_current_frame(self, prefix: str = "capture") -> SaveResult:
        now_ns = time.time_ns()
        seconds = now_ns // 1_000_000_000
        microseconds = now_ns // 1_000 % 1_000_000
        stamp = (
            f"{time.strftime('%Y%m%d-%H%M%S', time.localtime(seconds))}"
            f"-{microseconds:06d}"
        )
        while True:
            self._capture_sequence += 1
            stem = f"{prefix}-{stamp}-{self._capture_sequence:06d}"
            image_path = self.capture_dir / f"{stem}.jpg"
            metadata_path = self.capture_dir / f"{stem}.json"
            if not image_path.exists() and not metadata_path.exists():
                break
        result = self.state.save_latest_frame(self.capture_dir, stem)
        await self.broadcast()
        return result

    async def broadcast(self) -> None:
        state = self.state.snapshot()
        state.update({"esp32_host": self.host, "esp32_port": self.port})
        message = json.dumps(
            {"type": "state", "state": state},
            ensure_ascii=False,
            separators=(",", ":"),
        )
        async with self._broadcast_lock:
            clients = tuple(self._browser_clients)
            results = await asyncio.gather(
                *(self._send_browser(client, message) for client in clients),
                return_exceptions=True,
            )
            for client, result in zip(clients, results):
                if result is not True:
                    self.remove_browser(client)

    async def _send_browser(self, client: Any, message: str) -> bool:
        owner_id = id(client)
        if owner_id in self._blocked_browser_ids:
            return False
        sender = getattr(client, "send_text", None)
        if sender is None:
            sender = client.send
        task = asyncio.create_task(sender(message))
        service_ref = weakref.ref(self)

        def consume_done(done_task: asyncio.Task[Any]) -> None:
            try:
                done_task.exception()
            except asyncio.CancelledError:
                pass
            service = service_ref()
            if service is not None:
                service._detached_browser_tasks.discard(done_task)
                service._detached_browser_owners.pop(done_task, None)
                service._blocked_browser_ids.discard(owner_id)

        task.add_done_callback(consume_done)
        try:
            _, pending = await asyncio.wait(
                {task}, timeout=self.BROWSER_SEND_TIMEOUT
            )
            if pending:
                self._detach_browser_task(task, owner_id)
                return False
            try:
                task.result()
            except BaseException:
                return False
            return True
        finally:
            if not task.done() and task not in self._detached_browser_tasks:
                self._detach_browser_task(task, owner_id)

    def _detach_browser_task(self, task: asyncio.Task[Any], owner_id: int) -> None:
        self._detached_browser_tasks.add(task)
        self._detached_browser_owners[task] = owner_id
        self._blocked_browser_ids.add(owner_id)
        task.cancel()

    async def _run_worker(self, function: Callable[..., Any], *args: Any) -> Any:
        task = asyncio.create_task(asyncio.to_thread(function, *args))
        self._worker_tasks.add(task)
        task.add_done_callback(self._worker_tasks.discard)
        # Cancelling an awaiter cannot stop its thread, so retain the real task.
        return await asyncio.shield(task)

    async def _wait_workers(self) -> None:
        while self._worker_tasks:
            tasks = tuple(self._worker_tasks)
            await asyncio.gather(*tasks, return_exceptions=True)
            self._worker_tasks.difference_update(tasks)

    async def _run_forever(self) -> None:
        startup_failed = False
        try:
            try:
                detector = await self._run_worker(
                    self._detector_factory, self.model, self.confidence, self.device
                )
                DIAGNOSTIC_LOGGER.info(
                    "MODEL ready path=%s confidence=%.3f device=%s",
                    self.model,
                    self.confidence,
                    self.device or "auto",
                )
            except asyncio.CancelledError:
                raise
            except Exception as error:
                DIAGNOSTIC_LOGGER.exception("MODEL initialization failed: %s", error)
                startup_failed = True
                self.state.running = False
                self.state.connected = False
                self.state.processing = "idle"
                self.state.last_error = str(error)
                self.state.warning = f"YOLO 模型初始化失败：{error}"
                return
            self._policy.reset()
            self._reporter.start_connection()
            while not self._stop_event.is_set():
                try:
                    self.state.processing = "connecting"
                    DIAGNOSTIC_LOGGER.info("WS connecting uri=%s", self.uri)
                    await self.broadcast()
                    connection = self._make_connection()
                    async with connection as websocket:
                        DIAGNOSTIC_LOGGER.info("WS connected uri=%s", self.uri)
                        self.state.connected = True
                        self.state.processing = "running"
                        self.state.warning = "视频流已连接"
                        self.state.last_error = None
                        self._policy.reset()
                        self._reporter.start_connection()
                        await self.broadcast()
                        await self._run_connection(websocket, detector)
                    self.state.connected = False
                    DIAGNOSTIC_LOGGER.warning("WS closed uri=%s", self.uri)
                    if not self._stop_event.is_set():
                        await self._wait_for_reconnect(
                            warning="ESP32 视频流已关闭，等待重连",
                            error="ESP32 视频流已关闭",
                        )
                except asyncio.CancelledError:
                    raise
                except Exception as error:
                    DIAGNOSTIC_LOGGER.exception("WS connection failed uri=%s error=%s", self.uri, error)
                    await self._wait_for_reconnect(
                        warning="ESP32 连接中断，等待重连",
                        error=str(error),
                    )
        finally:
            await self._wait_workers()
            self.state.running = False
            self.state.connected = False
            self.state.processing = "idle"
            if not startup_failed:
                self.state.warning = "已停止"
            await self.broadcast()

    async def _wait_for_reconnect(self, warning: str, error: str) -> None:
        self.state.connected = False
        self.state.processing = "reconnecting"
        self.state.last_error = error
        self.state.warning = warning
        await self.broadcast()
        try:
            await asyncio.wait_for(
                self._stop_event.wait(), timeout=self.reconnect_delay
            )
        except asyncio.TimeoutError:
            pass

    def _make_connection(self) -> Any:
        if self._connector is not None:
            return self._connector(
                self.uri,
                max_size=4 * 1024 * 1024,
                open_timeout=10,
                close_timeout=2,
                ping_interval=20,
                ping_timeout=20,
            )
        import websockets

        return websockets.connect(
            self.uri,
            max_size=4 * 1024 * 1024,
            open_timeout=10,
            close_timeout=2,
            ping_interval=20,
            ping_timeout=20,
        )

    async def _run_connection(self, websocket: Any, detector: PersonDetector) -> None:
        self._last_rx_at = None
        self._frame_metadata.clear()
        self._preview_frames.clear()
        self._pending_commands.clear()
        frames: asyncio.Queue[bytes] = asyncio.Queue(maxsize=1)
        rate = StreamRate()
        receive_task = asyncio.create_task(self._receive_stream(websocket, frames, rate))
        process_task = asyncio.create_task(self._process_frames(websocket, frames, rate, detector))
        tasks = {receive_task, process_task}
        try:
            done, _ = await asyncio.wait(
                tasks, return_when=asyncio.FIRST_COMPLETED
            )
            for task in done:
                task.result()
        finally:
            for task in tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)

    async def _receive_stream(
        self, websocket: Any, frames: asyncio.Queue[bytes], rate: StreamRate
    ) -> None:
        async for message in websocket:
            if isinstance(message, bytes):
                received_at = time.monotonic()
                gap_ms = (
                    0.0
                    if self._last_rx_at is None
                    else (received_at - self._last_rx_at) * 1000.0
                )
                self._last_rx_at = received_at
                raw_length = len(message)
                jpeg = sanitize_jpeg_frame(message)
                if jpeg is None:
                    DIAGNOSTIC_LOGGER.warning(
                        "RX invalid bytes=%d head=%s tail=%s gap_ms=%.1f risk=%s smoking=%d",
                        raw_length,
                        message[:8].hex(),
                        message[-8:].hex(),
                        gap_ms,
                        self.state.risk,
                        self.state.smoking,
                    )
                    self.state.last_error = "ESP32 发送了无效 JPEG 帧，已丢弃"
                    await self.broadcast()
                    continue
                digest = hashlib.sha256(jpeg).hexdigest()[:16]
                if len(jpeg) != len(message):
                    DIAGNOSTIC_LOGGER.warning(
                        "RX repaired raw_bytes=%d jpeg_bytes=%d sha256=%s head_skip=%d tail_skip=%d gap_ms=%.1f",
                        raw_length,
                        len(jpeg),
                        digest,
                        message.find(b"\xff\xd8\xff"),
                        raw_length - (message.rfind(b"\xff\xd9") + 2),
                        gap_ms,
                    )
                    self.state.last_error = "ESP32 JPEG frame contained dirty bytes and was repaired"
                    await self.broadcast()
                rate.record_frame(time.monotonic())
                self._rx_sequence += 1
                sequence = self._rx_sequence
                self._frame_metadata[digest] = (sequence, received_at)
                if len(self._frame_metadata) > 64:
                    self._frame_metadata.pop(next(iter(self._frame_metadata)))
                replaced_pending = frames.full()
                offer_latest(frames, jpeg)
                DIAGNOSTIC_LOGGER.info(
                    "RX frame=%d bytes=%d sha256=%s gap_ms=%.1f fps=%.2f replaced_pending=%s risk=%s smoking=%d",
                    sequence,
                    len(jpeg),
                    digest,
                    gap_ms,
                    getattr(rate, "receive_fps", 0.0),
                    replaced_pending,
                    self.state.risk,
                    self.state.smoking,
                )
                continue
            try:
                telemetry = json.loads(message)
            except (json.JSONDecodeError, TypeError):
                telemetry = None
            if isinstance(telemetry, dict) and telemetry.get("type") == "environment":
                dht_valid = telemetry.get("dht_valid") is True
                temperature = telemetry.get("temperature_c")
                humidity = telemetry.get("humidity_pct")
                mq2_adc = telemetry.get("mq2_adc")
                sampled_at_ms = telemetry.get("sampled_at_ms")
                if (
                    isinstance(temperature, (int, float)) and not isinstance(temperature, bool)
                    and isinstance(humidity, (int, float)) and not isinstance(humidity, bool)
                    and type(mq2_adc) is int and 0 <= mq2_adc <= 4095
                    and type(sampled_at_ms) is int and sampled_at_ms >= 0
                    and -40 <= temperature <= 125 and 0 <= humidity <= 100
                ):
                    self.state.update_environment(
                        dht_valid, float(temperature), float(humidity), mq2_adc,
                        sampled_at_ms,
                    )
                    DIAGNOSTIC_LOGGER.info(
                        "ENV temperature_c=%.1f humidity_pct=%.1f mq2_adc=%d dht_valid=%s",
                        temperature, humidity, mq2_adc, dht_valid,
                    )
                    await self.broadcast()
                else:
                    DIAGNOSTIC_LOGGER.warning("ENV invalid payload=%r", str(message)[:256])
                continue
            ack = parse_ack(message)
            if ack:
                sent_at = self._pending_commands.pop(ack["command_id"], None)
                latency_ms = (
                    -1.0 if sent_at is None else (time.monotonic() - sent_at) * 1000.0
                )
                DIAGNOSTIC_LOGGER.info(
                    "ACK command=%d applied=%s latency_ms=%.1f error=%s",
                    ack["command_id"],
                    ack["applied"],
                    latency_ms,
                    ack["error"] or "none",
                )
                if not ack["applied"]:
                    self.state.last_error = (
                        f"ESP32 指令 {ack['command_id']} 被拒绝："
                        f"{ack['error'] or '未知错误'}"
                    )
                    await self.broadcast()
            else:
                DIAGNOSTIC_LOGGER.warning("ACK invalid payload=%r", str(message)[:256])

    async def _process_frames(
        self,
        websocket: Any,
        frames: asyncio.Queue[bytes],
        rate: StreamRate,
        detector: PersonDetector,
    ) -> None:
        while not self._stop_event.is_set():
            jpeg = await frames.get()
            await self._process_frame(websocket, jpeg, rate.receive_fps, detector)

    async def _process_frame(
        self,
        websocket: Any,
        jpeg: bytes,
        receive_fps: float,
        detector: PersonDetector,
    ) -> None:
        jpeg = sanitize_jpeg_frame(jpeg)
        if jpeg is None:
            DIAGNOSTIC_LOGGER.warning("AI rejected invalid queued JPEG")
            self.state.last_error = "ESP32 发送了无效 JPEG 帧，已丢弃"
            await self.broadcast()
            return
        digest = hashlib.sha256(jpeg).hexdigest()[:16]
        sequence, received_at = self._frame_metadata.get(
            digest,
            (self.state.frame_sequence + 1, time.monotonic()),
        )
        started = time.monotonic()
        try:
            detection: Detection = await self._run_worker(detector.detect, jpeg)
            elapsed = max(time.monotonic() - started, 1e-6)
            self.state.update_preview_frame(jpeg, receive_fps, sequence)
            self._preview_frames[sequence] = jpeg
            if len(self._preview_frames) > 8:
                self._preview_frames.pop(next(iter(self._preview_frames)))
            people_level = self.state.thresholds.classify(detection.people)
            environment_level = self.state.thresholds.classify_environment(
                self.state.temperature_c,
                self.state.humidity_pct,
                self.state.mq2_adc,
                self.state.dht_valid,
            )
            observed_level = (
                "high" if environment_level == "high" else people_level
            )
            previous_level = self._policy.current_level
            changed = self._policy.observe(observed_level)
            level = self._policy.current_level
            DIAGNOSTIC_LOGGER.info(
                "AI frame=%d sha256=%s age_ms=%.1f inference_ms=%.1f people=%d smoking=%d confidence=%.4f observed=%s previous=%s current=%s changed=%s",
                sequence,
                digest,
                (time.monotonic() - received_at) * 1000.0,
                elapsed * 1000.0,
                detection.people,
                detection.smoking,
                detection.smoking_confidence,
                observed_level,
                previous_level,
                level,
                changed,
            )
            self.state.update_detection(
                people=detection.people,
                smoking=detection.smoking,
                smoking_confidence=detection.smoking_confidence,
                risk=level,
                inference_fps=1.0 / elapsed,
                warning=self._warning_text(
                    level,
                    detection.people,
                    detection.smoking,
                    detection.smoking_confidence,
                    max_people=self.state.thresholds.max_people,
                    environment_level=environment_level,
                ),
            )
            message = self._reporter.next_message(
                level,
                detection.people,
                time.monotonic(),
                changed=changed,
                smoking=detection.smoking,
                smoking_confidence=detection.smoking_confidence,
            )
            if message is not None:
                command = json.loads(message)
                async with self._command_lock:
                    await websocket.send(message)
                self._pending_commands[command["command_id"]] = time.monotonic()
                if len(self._pending_commands) > 64:
                    self._pending_commands.pop(next(iter(self._pending_commands)))
                DIAGNOSTIC_LOGGER.info(
                    "TX command=%d level=%s people=%d smoking=%d confidence=%.4f changed=%s",
                    command["command_id"],
                    command["level"],
                    command["people"],
                    command["smoking"],
                    command["smoking_confidence"],
                    changed,
                )
            await self.broadcast()
        except ValueError as error:
            DIAGNOSTIC_LOGGER.exception(
                "AI failed frame=%d sha256=%s error=%s", sequence, digest, error
            )
            self.state.last_error = str(error)
            await self.broadcast()

    def log_browser_frame_request(self, frame: bytes, requested_sequence: int = 0) -> None:
        self._browser_frame_requests += 1
        digest = hashlib.sha256(frame).hexdigest()[:16]
        sequence, received_at = self._frame_metadata.get(digest, (0, time.monotonic()))
        DIAGNOSTIC_LOGGER.info(
            "HTTP request=%d requested_frame=%d served_frame=%d bytes=%d sha256=%s age_ms=%.1f risk=%s smoking=%d",
            self._browser_frame_requests,
            requested_sequence,
            sequence,
            len(frame),
            digest,
            (time.monotonic() - received_at) * 1000.0,
            self.state.risk,
            self.state.smoking,
        )

    def get_preview_frame(self, requested_sequence: int = 0) -> bytes | None:
        """Return the requested validated frame when it is still in the short cache."""
        if requested_sequence > 0:
            frame = self._preview_frames.get(requested_sequence)
            if frame is not None:
                return frame
        return self.state.latest_frame

    def log_browser_event(self, event: str, frame_sequence: int, width: int, height: int,
                          risk: str, frame_url: str) -> None:
        DIAGNOSTIC_LOGGER.info(
            "BROWSER event=%s frame=%d dimensions=%dx%d risk=%s url=%s",
            event,
            frame_sequence,
            width,
            height,
            risk,
            frame_url[:256],
        )

    def _encode_jpeg(self, detector: PersonDetector, image: Any) -> bytes:
        ok, encoded = detector.cv2.imencode(
            ".jpg", image, [int(detector.cv2.IMWRITE_JPEG_QUALITY), 85]
        )
        if not ok:
            raise ValueError("failed to encode annotated frame")
        return encoded.tobytes()

    @staticmethod
    def _warning_text(
        level: str,
        people: int,
        smoking: int,
        smoking_confidence: float,
        *,
        max_people: int | None = None,
        environment_level: str = "low",
    ) -> str:
        if max_people is not None:
            if environment_level == "high":
                return (
                    f"高风险：环境传感器超过阈值（温度/湿度/MQ2），"
                    "已触发声光报警"
                )
            if level == "low":
                return f"低风险：当前人数 {people}，最大允许人数 {max_people}"
            return (
                f"{level == 'medium' and '中风险' or '高风险'}：当前人数 {people}，"
                f"超过最大允许人数 {max_people}，已触发声光报警"
            )
        labels = {"low": "低风险", "medium": "中风险", "high": "高风险"}
        if smoking == 0 and level == "low":
            return f"低风险：未检测到抽烟行为，画面中 {people} 人"
        return (
            f"{labels[level]}：检测到 {smoking} 人抽烟，"
            f"最高置信度 {smoking_confidence:.0%}，画面中 {people} 人"
        )
