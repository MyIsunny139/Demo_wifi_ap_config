import asyncio
import gc
import hashlib
import json
import threading
import time
import weakref
from functools import wraps
from pathlib import Path
from types import SimpleNamespace

from python_client.dashboard_service import DashboardService

OLD_JPEG = b"\xff\xd8\xffold-frame\xff\xd9"
NEW_JPEG = b"\xff\xd8\xffnew-frame\xff\xd9"
RAW_JPEG = b"\xff\xd8\xffraw-jpeg\xff\xd9"


def async_test(function):
    @wraps(function)
    def wrapper(*args, **kwargs):
        return asyncio.run(function(*args, **kwargs))

    return wrapper


class FakeBrowser:
    def __init__(self) -> None:
        self.sent: list[str] = []

    async def send(self, message: str) -> None:
        self.sent.append(message)


class FakeFastAPIBrowser:
    def __init__(self) -> None:
        self.sent: list[str] = []

    async def send_text(self, message: str) -> None:
        self.sent.append(message)


class FakeESP32WebSocket:
    def __init__(self, incoming=()) -> None:
        self.incoming = list(incoming)
        self.sent: list[str] = []

    def __aiter__(self):
        return self

    async def __anext__(self):
        if not self.incoming:
            raise StopAsyncIteration
        return self.incoming.pop(0)

    async def send(self, message: str) -> None:
        self.sent.append(message)


class FakeCV2:
    IMWRITE_JPEG_QUALITY = 1

    @staticmethod
    def imencode(extension, image, options):
        assert extension == ".jpg"
        assert options == [FakeCV2.IMWRITE_JPEG_QUALITY, 85]
        return True, SimpleNamespace(tobytes=lambda: b"annotated-jpeg")


class FakeDetector:
    cv2 = FakeCV2()

    def __init__(
        self,
        people: int = 2,
        smoking: int = 0,
        smoking_confidence: float = 0.0,
    ) -> None:
        self.people = people
        self.smoking = smoking
        self.smoking_confidence = smoking_confidence

    def detect(self, jpeg: bytes):
        assert jpeg == RAW_JPEG
        return SimpleNamespace(
            people=self.people,
            smoking=self.smoking,
            smoking_confidence=self.smoking_confidence,
            image=object(),
        )


@async_test
async def test_service_start_stop_updates_state_without_duplicate_runner(monkeypatch):
    service = DashboardService(host="192.168.1.3", model="yolo11n.pt", device="cpu")
    started = asyncio.Event()

    async def idle_runner():
        started.set()
        await service._stop_event.wait()

    monkeypatch.setattr(service, "_run_forever", idle_runner)

    await service.start()
    await started.wait()
    assert service.state.warning == "正在连接 ESP32"
    runner = service._runner
    await service.start()

    assert service.state.running is True
    assert service._runner is runner

    await service.stop()

    assert service.state.running is False
    assert service.state.connected is False
    assert service.state.processing == "idle"
    assert service.state.warning == "已停止"
    assert service._runner is None


@async_test
async def test_model_initialization_failure_resets_state_broadcasts_and_allows_restart(
    monkeypatch,
):
    def fail_to_load(*args):
        raise RuntimeError("模型文件损坏")

    service = DashboardService(host="192.168.1.3", detector_factory=fail_to_load)
    browser = FakeBrowser()
    service.add_browser(browser)

    await service.start()
    first_runner = service._runner
    result = await asyncio.gather(first_runner, return_exceptions=True)

    assert result == [None]
    assert service.state.running is False
    assert service.state.connected is False
    assert service.state.processing == "idle"
    assert service.state.last_error == "模型文件损坏"
    assert service.state.warning == "YOLO 模型初始化失败：模型文件损坏"
    final_snapshot = json.loads(browser.sent[-1])["state"]
    assert final_snapshot["running"] is False
    assert final_snapshot["last_error"] == "模型文件损坏"
    assert final_snapshot["warning"] == "YOLO 模型初始化失败：模型文件损坏"

    restarted = asyncio.Event()

    async def idle_runner():
        restarted.set()
        await service._stop_event.wait()

    monkeypatch.setattr(service, "_run_forever", idle_runner)
    await service.start()
    await restarted.wait()

    assert service.state.running is True
    assert service._runner is not first_runner
    await service.stop()


@async_test
async def test_service_broadcasts_snapshot_and_removes_failed_clients():
    service = DashboardService(host="192.168.1.3")
    generic = FakeBrowser()
    fastapi = FakeFastAPIBrowser()

    class FailedBrowser:
        async def send(self, message: str) -> None:
            raise ConnectionError("closed")

    failed = FailedBrowser()
    for browser in (generic, fastapi, failed):
        service.add_browser(browser)

    await service.broadcast()

    for message in (generic.sent[-1], fastapi.sent[-1]):
        payload = json.loads(message)
        assert payload["type"] == "state"
        assert payload["state"]["running"] is False
    assert failed not in service._browser_clients


@async_test
async def test_service_updates_connection_and_broadcasts_address():
    service = DashboardService(host="192.168.1.3")
    browser = FakeBrowser()
    service.add_browser(browser)

    await service.update_connection(" 192.168.166.200 ", 81)

    assert service.uri == "ws://192.168.166.200:81/video"
    state = json.loads(browser.sent[-1])["state"]
    assert state["esp32_host"] == "192.168.166.200"
    assert state["esp32_port"] == 81


@async_test
async def test_service_rejects_connection_change_while_running():
    service = DashboardService(host="192.168.1.3")
    service.state.running = True

    try:
        await service.update_connection("192.168.166.200", 81)
    except RuntimeError as error:
        assert str(error) == "请先停止处理，再修改 ESP32 地址"
    else:
        raise AssertionError("running service accepted a connection change")


@async_test
async def test_broadcast_times_out_hanging_client_and_stop_does_not_wait_forever():
    class HangingBrowser:
        async def send(self, message: str) -> None:
            await asyncio.Event().wait()

    service = DashboardService(host="192.168.1.3")
    service.BROWSER_SEND_TIMEOUT = 0.01
    browser = HangingBrowser()
    service.add_browser(browser)

    await asyncio.wait_for(service.broadcast(), timeout=0.1)
    assert browser not in service._browser_clients

    service.add_browser(browser)
    await asyncio.wait_for(service.stop(), timeout=0.1)
    assert browser not in service._browser_clients


@async_test
async def test_broadcast_hard_timeout_does_not_wait_for_cancel_resistant_sender():
    cancelled = asyncio.Event()
    release = asyncio.Event()
    loop_errors = []
    loop = asyncio.get_running_loop()
    previous_handler = loop.get_exception_handler()
    loop.set_exception_handler(lambda current_loop, context: loop_errors.append(context))

    class CancelResistantBrowser:
        async def send(self, message: str) -> None:
            try:
                await asyncio.Event().wait()
            except asyncio.CancelledError:
                cancelled.set()
                await release.wait()
                raise RuntimeError("late send failure")

    service = DashboardService(host="192.168.1.3")
    service.BROWSER_SEND_TIMEOUT = 0.01
    browser = CancelResistantBrowser()
    service.add_browser(browser)

    started = asyncio.get_running_loop().time()
    await asyncio.wait_for(service.broadcast(), timeout=0.08)

    assert asyncio.get_running_loop().time() - started < 0.08
    assert cancelled.is_set()
    assert browser not in service._browser_clients
    assert len(service._detached_browser_tasks) == 1

    release.set()
    for _ in range(10):
        if not service._detached_browser_tasks:
            break
        await asyncio.sleep(0)

    loop.set_exception_handler(previous_handler)
    assert not service._detached_browser_tasks
    assert loop_errors == []


@async_test
async def test_readding_browser_is_blocked_until_old_detached_send_finishes():
    release = asyncio.Event()
    active = 0
    max_active = 0

    class CancelResistantBrowser:
        async def send(self, message: str) -> None:
            nonlocal active, max_active
            active += 1
            max_active = max(max_active, active)
            try:
                while not release.is_set():
                    try:
                        await release.wait()
                    except asyncio.CancelledError:
                        continue
                raise RuntimeError("late send failure")
            finally:
                active -= 1

    service = DashboardService(host="192.168.1.3")
    service.BROWSER_SEND_TIMEOUT = 0.01
    browser = CancelResistantBrowser()
    assert service.add_browser(browser) is True
    await service.broadcast()
    assert len(service._detached_browser_tasks) == 1
    assert active == 1

    assert service.add_browser(browser) is False
    assert browser not in service._browser_clients
    await service.broadcast()
    assert max_active == 1

    release.set()
    for _ in range(10):
        if not service._detached_browser_tasks:
            break
        await asyncio.sleep(0)

    assert not service._detached_browser_tasks
    assert id(browser) not in service._blocked_browser_ids
    assert service.add_browser(browser) is True


@async_test
async def test_second_broadcast_skips_client_blocked_by_cancelled_broadcast():
    started = asyncio.Event()
    release = asyncio.Event()
    active = 0
    max_active = 0

    class CancelResistantBrowser:
        async def send(self, message: str) -> None:
            nonlocal active, max_active
            active += 1
            max_active = max(max_active, active)
            started.set()
            try:
                while not release.is_set():
                    try:
                        await release.wait()
                    except asyncio.CancelledError:
                        continue
            finally:
                active -= 1

    service = DashboardService(host="192.168.1.3")
    service.BROWSER_SEND_TIMEOUT = 1.0
    browser = CancelResistantBrowser()
    assert service.add_browser(browser) is True

    first_broadcast = asyncio.create_task(service.broadcast())
    await started.wait()
    first_broadcast.cancel()
    await asyncio.gather(first_broadcast, return_exceptions=True)

    assert browser in service._browser_clients
    assert id(browser) in service._blocked_browser_ids
    assert len(service._detached_browser_tasks) == 1

    service.BROWSER_SEND_TIMEOUT = 0.01
    try:
        await asyncio.wait_for(service.broadcast(), timeout=0.08)
        assert max_active == 1
        assert browser not in service._browser_clients
    finally:
        release.set()
        for _ in range(10):
            if not service._detached_browser_tasks:
                break
            await asyncio.sleep(0)

    assert active == 0
    assert not service._detached_browser_tasks
    assert id(browser) not in service._blocked_browser_ids


@async_test
async def test_stop_drops_detached_refs_and_late_sender_exception_is_consumed():
    release = asyncio.Event()
    loop_errors = []
    loop = asyncio.get_running_loop()
    previous_handler = loop.get_exception_handler()
    loop.set_exception_handler(lambda current_loop, context: loop_errors.append(context))

    class CancelResistantBrowser:
        async def send(self, message: str) -> None:
            while not release.is_set():
                try:
                    await release.wait()
                except asyncio.CancelledError:
                    continue
            raise RuntimeError("late stop send failure")

    service = DashboardService(host="192.168.1.3")
    service.BROWSER_SEND_TIMEOUT = 0.01
    browser = CancelResistantBrowser()
    browser_id = id(browser)
    browser_ref = weakref.ref(browser)
    assert service.add_browser(browser) is True
    await service.broadcast()
    detached = next(iter(service._detached_browser_tasks))

    assert set(service._detached_browser_owners.values()) == {browser_id}
    await service.stop()

    assert not service._detached_browser_tasks
    assert not service._detached_browser_owners
    assert browser_id in service._blocked_browser_ids
    assert browser not in service._browser_clients

    del browser
    gc.collect()
    assert browser_ref() is not None

    release.set()
    for _ in range(10):
        if detached.done() and browser_id not in service._blocked_browser_ids:
            break
        await asyncio.sleep(0)
    del detached
    gc.collect()

    loop.set_exception_handler(previous_handler)
    assert browser_id not in service._blocked_browser_ids
    assert browser_ref() is None
    assert loop_errors == []


@async_test
async def test_service_saves_latest_frame_with_microsecond_unique_names(
    tmp_path: Path, monkeypatch
):
    service = DashboardService(host="192.168.1.3", capture_dir=tmp_path)
    service.state.update_frame(
        jpeg=b"frame",
        people=3,
        smoking=1,
        smoking_confidence=0.8,
        risk="medium",
        receive_fps=8.0,
        inference_fps=2.0,
        warning="中风险：检测到 3 人",
    )
    monkeypatch.setattr(
        "python_client.dashboard_service.time.time_ns",
        lambda: 1784605027123456000,
    )

    first = await service.save_current_frame("manual")
    second = await service.save_current_frame("manual")

    assert first.image_path != second.image_path
    assert first.image_path.stem.endswith("-123456-000001")
    assert second.image_path.stem.endswith("-123456-000002")
    assert first.image_path.read_bytes() == b"frame"
    assert second.image_path.exists()
    assert first.metadata_path.exists()
    assert second.metadata_path.exists()
    assert service.state.last_saved == str(second.image_path)


@async_test
async def test_save_skips_capture_stem_already_on_disk(tmp_path: Path, monkeypatch):
    service = DashboardService(host="192.168.1.3", capture_dir=tmp_path)
    service.state.update_frame(
        jpeg=b"frame",
        people=1,
        smoking=0,
        smoking_confidence=0.0,
        risk="low",
        receive_fps=1.0,
        inference_fps=1.0,
        warning="低风险：检测到 1 人",
    )
    frozen_ns = 1784605027123456000
    monkeypatch.setattr("python_client.dashboard_service.time.time_ns", lambda: frozen_ns)
    timestamp = (
        f"{time.strftime('%Y%m%d-%H%M%S', time.localtime(frozen_ns // 1_000_000_000))}"
        "-123456-000001"
    )
    (tmp_path / f"manual-{timestamp}.jpg").write_bytes(b"existing")

    result = await service.save_current_frame("manual")

    assert result.image_path.stem.endswith("-123456-000002")
    assert result.image_path.read_bytes() == b"frame"


@async_test
async def test_stop_waits_for_blocking_model_worker_to_release():
    started = threading.Event()
    release = threading.Event()

    def blocking_detector_factory(*args):
        started.set()
        release.wait()
        return FakeDetector()

    service = DashboardService(
        host="192.168.1.3", detector_factory=blocking_detector_factory
    )
    await service.start()
    for _ in range(100):
        if started.is_set():
            break
        await asyncio.sleep(0.001)
    assert started.is_set()

    stopping = asyncio.create_task(service.stop())
    await asyncio.sleep(0.01)
    assert not stopping.done()

    release.set()
    await asyncio.wait_for(stopping, timeout=0.5)
    assert service.state.running is False
    assert not service._worker_tasks


@async_test
async def test_receive_stream_queues_latest_frame_without_publishing_it():
    service = DashboardService(host="192.168.1.3")
    browser = FakeBrowser()
    service.add_browser(browser)
    websocket = FakeESP32WebSocket(
        [
            OLD_JPEG,
            b"dirty" + NEW_JPEG + b"tail",
            '{"type":"ack","command_id":7,"applied":false,"error":"bad level"}',
        ]
    )
    frames: asyncio.Queue[bytes] = asyncio.Queue(maxsize=1)
    rate = SimpleNamespace(record_frame=lambda now: None)

    await service._receive_stream(websocket, frames, rate)

    assert frames.get_nowait() == NEW_JPEG
    assert service.state.latest_frame is None
    assert service.state.frame_sequence == 0
    assert service._rx_sequence == 2
    assert service.state.last_error == "ESP32 指令 7 被拒绝：bad level"
    assert json.loads(browser.sent[-1])["state"]["last_error"] == service.state.last_error


@async_test
async def test_receive_stream_updates_environment_sensor_state():
    service = DashboardService(host="192.168.1.3")
    websocket = FakeESP32WebSocket([
        '{"type":"environment","dht_valid":true,"temperature_c":24.5,"humidity_pct":45,"mq2_adc":900,"sampled_at_ms":2000}'
    ])
    await service._receive_stream(
        websocket, asyncio.Queue(maxsize=1), SimpleNamespace(record_frame=lambda now: None)
    )
    assert service.state.temperature_c == 24.5
    assert service.state.humidity_pct == 45.0
    assert service.state.mq2_adc == 900
    assert service.state.environment_sampled_at_ms == 2000


@async_test
async def test_process_frame_publishes_preview_after_successful_decode(monkeypatch):
    service = DashboardService(host="192.168.1.3")
    await service.update_thresholds(max_people=1)
    service.state.latest_frame = b"preview-jpeg"
    detector = FakeDetector(people=2, smoking=1, smoking_confidence=0.86)
    websocket = FakeESP32WebSocket()
    calls = []
    real_to_thread = asyncio.to_thread

    async def recording_to_thread(function, *args, **kwargs):
        calls.append(function)
        return await real_to_thread(function, *args, **kwargs)

    monkeypatch.setattr(asyncio, "to_thread", recording_to_thread)
    service._reporter.start_connection()

    for _ in range(3):
        await service._process_frame(websocket, RAW_JPEG, 8.0, detector)

    commands = [json.loads(message) for message in websocket.sent]
    assert service.state.risk == "medium"
    assert commands[-1]["level"] == "medium"
    assert commands[-1]["people"] == 2
    assert commands[-1]["smoking"] == 1
    assert commands[-1]["smoking_confidence"] == 0.86
    assert service.state.warning == "中风险：当前人数 2，超过最大允许人数 1，已触发声光报警"
    assert service.state.latest_frame == RAW_JPEG
    assert service.state.frame_sequence == 3
    assert detector.detect in calls
    assert service._encode_jpeg not in calls


@async_test
async def test_mq2_threshold_triggers_high_risk_command():
    service = DashboardService(host="192.168.1.3")
    service.state.update_environment(True, 24.0, 45.0, 1900, 2000)
    websocket = FakeESP32WebSocket()
    service._reporter.start_connection()

    for _ in range(3):
        await service._process_frame(websocket, RAW_JPEG, 8.0, FakeDetector(people=0))

    commands = [json.loads(message) for message in websocket.sent]
    assert service.state.risk == "high"
    assert commands[-1]["level"] == "high"
    assert "环境传感器超过阈值" in service.state.warning


@async_test
async def test_process_frame_keeps_previous_preview_when_decode_fails():
    service = DashboardService(host="192.168.1.3")
    service.state.update_preview_frame(b"previous-valid-jpeg", 7.0, frame_sequence=9)
    service.state.update_detection(1, 1, 0.8, "medium", 3.0, "existing warning")
    websocket = FakeESP32WebSocket()

    class InvalidJpegDetector:
        def detect(self, jpeg):
            raise ValueError("received an invalid JPEG frame")

    await service._process_frame(websocket, RAW_JPEG, 8.0, InvalidJpegDetector())

    assert service.state.latest_frame == b"previous-valid-jpeg"
    assert service.state.frame_sequence == 9
    assert service.state.risk == "medium"
    assert service.state.last_error == "received an invalid JPEG frame"
    assert websocket.sent == []


@async_test
async def test_process_frame_preserves_receive_sequence_after_queued_frames_are_dropped():
    service = DashboardService(host="192.168.1.3")
    detector = FakeDetector()
    websocket = FakeESP32WebSocket()
    digest = hashlib.sha256(RAW_JPEG).hexdigest()[:16]
    service._frame_metadata[digest] = (6, time.monotonic())

    await service._process_frame(websocket, RAW_JPEG, 8.0, detector)

    assert service.state.latest_frame == RAW_JPEG
    assert service.state.frame_sequence == 6


@async_test
async def test_process_frame_clears_medium_risk_on_first_non_smoking_frame():
    service = DashboardService(host="192.168.1.3")
    await service.update_thresholds(max_people=1)
    detector = FakeDetector(people=2, smoking=1, smoking_confidence=0.86)
    websocket = FakeESP32WebSocket()
    service._reporter.start_connection()

    for _ in range(3):
        await service._process_frame(websocket, RAW_JPEG, 8.0, detector)
    assert service.state.risk == "medium"

    detector.people = 1
    detector.smoking = 0
    detector.smoking_confidence = 0.0
    await service._process_frame(websocket, RAW_JPEG, 8.0, detector)

    commands = [json.loads(message) for message in websocket.sent]
    assert service.state.risk == "low"
    assert commands[-1]["level"] == "low"
    assert commands[-1]["smoking"] == 0
    assert "保持中" not in service.state.warning


@async_test
async def test_run_forever_reconnects_after_connection_failure():
    attempts = 0
    websocket = FakeESP32WebSocket()

    class ConnectionContext:
        async def __aenter__(self):
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                raise ConnectionError("offline")
            return websocket

        async def __aexit__(self, exc_type, exc, traceback):
            return False

    service = DashboardService(
        host="192.168.1.3",
        reconnect_delay=0.001,
        connector=lambda *args, **kwargs: ConnectionContext(),
        detector_factory=lambda *args: FakeDetector(),
    )
    browser = FakeBrowser()
    service.add_browser(browser)

    async def finish_connection(*args):
        service._stop_event.set()

    service._run_connection = finish_connection
    service.state.running = True

    await service._run_forever()

    assert attempts == 2
    assert service.state.connected is False
    assert service.state.processing == "idle"
    assert service.state.last_error is None
    assert service.state.running is False
    assert service.state.warning == "已停止"
    warnings = [json.loads(message)["state"]["warning"] for message in browser.sent]
    assert "ESP32 连接中断，等待重连" in warnings


@async_test
async def test_stop_active_connection_cancels_and_awaits_both_children(monkeypatch):
    service = DashboardService(host="192.168.1.3")
    receive_started = asyncio.Event()
    process_started = asyncio.Event()
    receive_finished = asyncio.Event()
    process_finished = asyncio.Event()

    async def blocked_receive(*args):
        receive_started.set()
        try:
            await asyncio.Event().wait()
        finally:
            receive_finished.set()

    async def blocked_process(*args):
        process_started.set()
        try:
            await asyncio.Event().wait()
        finally:
            process_finished.set()

    monkeypatch.setattr(service, "_receive_stream", blocked_receive)
    monkeypatch.setattr(service, "_process_frames", blocked_process)
    service.state.running = True
    service.state.connected = True
    service.state.processing = "running"
    service._runner = asyncio.create_task(
        service._run_connection(FakeESP32WebSocket(), FakeDetector())
    )
    await receive_started.wait()
    await process_started.wait()

    await service.stop()

    assert receive_finished.is_set()
    assert process_finished.is_set()
    assert service._runner is None
    assert service.state.running is False
    assert service.state.connected is False
    assert service.state.processing == "idle"
    assert service.state.warning == "已停止"


@async_test
async def test_clean_websocket_close_broadcasts_waits_and_reconnects():
    enter_times = []
    first_closed = asyncio.Event()
    browser = FakeBrowser()

    class ConnectionContext:
        async def __aenter__(self):
            enter_times.append(asyncio.get_running_loop().time())
            return FakeESP32WebSocket()

        async def __aexit__(self, exc_type, exc, traceback):
            return False

    service = DashboardService(
        host="192.168.1.3",
        reconnect_delay=0.05,
        connector=lambda *args, **kwargs: ConnectionContext(),
        detector_factory=lambda *args: FakeDetector(),
    )
    service.add_browser(browser)
    service.state.running = True

    async def close_connection(*args):
        if len(enter_times) == 1:
            first_closed.set()
            return
        service._stop_event.set()

    service._run_connection = close_connection
    runner = asyncio.create_task(service._run_forever())
    await first_closed.wait()
    for _ in range(10):
        if service.state.processing == "reconnecting":
            break
        await asyncio.sleep(0)

    assert len(enter_times) == 1
    assert service.state.connected is False
    assert service.state.processing == "reconnecting"
    assert service.state.warning == "ESP32 视频流已关闭，等待重连"
    for _ in range(10):
        warnings = [json.loads(message)["state"]["warning"] for message in browser.sent]
        if "ESP32 视频流已关闭，等待重连" in warnings:
            break
        await asyncio.sleep(0)
    assert "ESP32 视频流已关闭，等待重连" in warnings

    await asyncio.wait_for(runner, timeout=0.2)

    assert len(enter_times) == 2
    assert enter_times[1] - enter_times[0] >= 0.04
    assert service.state.running is False
    assert service.state.connected is False
    assert service.state.processing == "idle"
    assert service.state.warning == "已停止"
    assert json.loads(browser.sent[-1])["state"]["warning"] == "已停止"


def test_warning_text_is_chinese_for_each_risk_level():
    assert (
        DashboardService._warning_text("low", 1, 0, 0.0)
        == "低风险：未检测到抽烟行为，画面中 1 人"
    )
    assert (
        DashboardService._warning_text("medium", 3, 1, 0.76)
        == "中风险：检测到 1 人抽烟，最高置信度 76%，画面中 3 人"
    )
    assert (
        DashboardService._warning_text("high", 6, 2, 0.91)
        == "高风险：检测到 2 人抽烟，最高置信度 91%，画面中 6 人"
    )
