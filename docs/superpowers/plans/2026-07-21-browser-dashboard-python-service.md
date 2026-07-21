# 浏览器仪表盘 Python 服务实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增一个浏览器单页仪表盘，由本地 Python 服务连接 ESP32 视频流、运行 YOLO、下发 WS2812 风险指令，并向浏览器显示实时视频和预警状态。

**Architecture:** 保留现有 `python_client.video_ai` 命令行客户端，新建 `python_client.dashboard` 作为浏览器仪表盘入口。Python 后端使用 FastAPI 提供本地页面、REST 控制接口和浏览器 WebSocket 状态推送；ESP32 仍只通过现有 `ws://<device-ip>:81/video` 被 Python 服务连接。

**Tech Stack:** Python 3.11, FastAPI, Uvicorn, websockets, OpenCV, Ultralytics YOLO, vanilla HTML/CSS/JavaScript, pytest.

---

## 文件结构

- Create: `python_client/dashboard_state.py`
  - 负责阈值、运行状态、最新帧、预警快照、保存结果等纯 Python 状态模型。
- Create: `python_client/dashboard_service.py`
  - 负责 ESP32 WebSocket 接入、YOLO 推理、风险指令生成、状态广播和截图保存。
- Create: `python_client/dashboard_app.py`
  - 负责 FastAPI 应用、REST 控制接口、浏览器 WebSocket 和静态页面挂载。
- Create: `python_client/dashboard.py`
  - 负责命令行入口，启动 Uvicorn 服务。
- Create: `python_client/static/dashboard.html`
  - 浏览器单页仪表盘结构。
- Create: `python_client/static/dashboard.css`
  - 深色监控仪表盘样式。
- Create: `python_client/static/dashboard.js`
  - 浏览器控制逻辑、状态 WebSocket、视频刷新和按钮事件。
- Modify: `python_client/requirements.txt`
  - 增加 FastAPI、Uvicorn、HTTPX 依赖。
- Test: `python_client/tests/test_dashboard_state.py`
  - 测试阈值、状态切换、帧保存数据模型。
- Test: `python_client/tests/test_dashboard_app.py`
  - 测试 FastAPI 控制接口，不启动真实 ESP32。

## 范围说明

本计划不改 ESP32 端协议和固件。ESP32 当前已经支持接收如下风险指令：

```json
{"type":"risk","command_id":1,"level":"medium","people":4}
```

Python 仪表盘会继续沿用这个协议。后续如果要增加自定义灯效，例如呼吸、闪烁、颜色选择，再单独扩展 ESP32 协议。

---

### Task 1: 增加 Web 服务依赖

**Files:**
- Modify: `python_client/requirements.txt`

- [ ] **Step 1: 修改依赖文件**

把 `python_client/requirements.txt` 改成：

```text
fastapi>=0.115,<1
httpx>=0.27,<1
numpy>=1.26,<3
opencv-python>=4.10,<5
pytest>=8,<9
ultralytics>=8.3,<9
uvicorn[standard]>=0.30,<1
websockets>=14,<16
```

- [ ] **Step 2: 安装依赖**

Run:

```powershell
.\.venv\Scripts\python.exe -m pip install -r python_client\requirements.txt
```

Expected: 安装成功，退出码为 `0`。

- [ ] **Step 3: 提交**

```powershell
git add python_client/requirements.txt
git commit -m "Add dashboard web dependencies"
```

---

### Task 2: 新增仪表盘状态模型

**Files:**
- Create: `python_client/dashboard_state.py`
- Test: `python_client/tests/test_dashboard_state.py`

- [ ] **Step 1: 写失败测试**

创建 `python_client/tests/test_dashboard_state.py`：

```python
import json
from pathlib import Path

import pytest

from python_client.dashboard_state import (
    DashboardState,
    Thresholds,
    build_risk_command,
)


def test_thresholds_classify_configurable_people_counts():
    thresholds = Thresholds(medium_people=2, high_people=5)

    assert thresholds.classify(0) == "low"
    assert thresholds.classify(2) == "medium"
    assert thresholds.classify(5) == "high"


@pytest.mark.parametrize(
    ("medium", "high"),
    [(0, 3), (3, 3), (5, 2)],
)
def test_thresholds_reject_invalid_boundaries(medium, high):
    with pytest.raises(ValueError):
        Thresholds(medium_people=medium, high_people=high)


def test_dashboard_state_keeps_latest_frame_and_snapshot():
    state = DashboardState()

    state.update_frame(
        jpeg=b"frame",
        people=4,
        risk="medium",
        receive_fps=9.5,
        inference_fps=3.2,
        warning="中风险：检测到 4 人",
    )

    assert state.latest_frame == b"frame"
    snapshot = state.snapshot()
    assert snapshot["people"] == 4
    assert snapshot["risk"] == "medium"
    assert snapshot["receive_fps"] == 9.5
    assert snapshot["inference_fps"] == 3.2
    assert snapshot["warning"] == "中风险：检测到 4 人"


def test_build_risk_command_matches_esp32_protocol():
    message = build_risk_command(command_id=7, level="high", people=6)

    assert json.loads(message) == {
        "type": "risk",
        "command_id": 7,
        "level": "high",
        "people": 6,
    }


def test_save_latest_frame_writes_image_and_metadata(tmp_path: Path):
    state = DashboardState()
    state.update_frame(
        jpeg=b"jpeg-bytes",
        people=1,
        risk="low",
        receive_fps=10.0,
        inference_fps=4.0,
        warning="低风险：检测到 1 人",
    )

    result = state.save_latest_frame(tmp_path, "capture")

    assert result.image_path.exists()
    assert result.metadata_path.exists()
    assert result.image_path.read_bytes() == b"jpeg-bytes"
    metadata = json.loads(result.metadata_path.read_text(encoding="utf-8"))
    assert metadata["people"] == 1
    assert metadata["risk"] == "low"
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
.\.venv\Scripts\python.exe -m pytest python_client\tests\test_dashboard_state.py -q
```

Expected: FAIL，错误包含 `ModuleNotFoundError: No module named 'python_client.dashboard_state'`。

- [ ] **Step 3: 实现状态模型**

创建 `python_client/dashboard_state.py`：

```python
"""State model for the browser dashboard."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal


RiskLevel = Literal["low", "medium", "high"]


@dataclass(frozen=True)
class Thresholds:
    medium_people: int = 3
    high_people: int = 6

    def __post_init__(self) -> None:
        if self.medium_people < 1:
            raise ValueError("medium_people must be at least 1")
        if self.high_people <= self.medium_people:
            raise ValueError("high_people must be greater than medium_people")

    def classify(self, people: int) -> RiskLevel:
        if people < 0:
            raise ValueError("people must not be negative")
        if people >= self.high_people:
            return "high"
        if people >= self.medium_people:
            return "medium"
        return "low"


@dataclass(frozen=True)
class SaveResult:
    image_path: Path
    metadata_path: Path


def build_risk_command(command_id: int, level: RiskLevel, people: int) -> str:
    if command_id < 0:
        raise ValueError("command_id must not be negative")
    if people < 0:
        raise ValueError("people must not be negative")
    if level not in {"low", "medium", "high"}:
        raise ValueError("invalid risk level")
    return json.dumps(
        {"type": "risk", "command_id": command_id, "level": level, "people": people},
        separators=(",", ":"),
    )


@dataclass
class DashboardState:
    thresholds: Thresholds = field(default_factory=Thresholds)
    running: bool = False
    connected: bool = False
    processing: str = "idle"
    people: int = 0
    risk: RiskLevel = "low"
    warning: str = "等待启动"
    receive_fps: float = 0.0
    inference_fps: float = 0.0
    last_error: str | None = None
    last_saved: str | None = None
    updated_at: float = field(default_factory=time.time)
    latest_frame: bytes | None = field(default=None, repr=False)

    def set_thresholds(self, thresholds: Thresholds) -> None:
        self.thresholds = thresholds
        self.updated_at = time.time()

    def update_frame(
        self,
        jpeg: bytes,
        people: int,
        risk: RiskLevel,
        receive_fps: float,
        inference_fps: float,
        warning: str,
    ) -> None:
        self.latest_frame = jpeg
        self.people = people
        self.risk = risk
        self.receive_fps = receive_fps
        self.inference_fps = inference_fps
        self.warning = warning
        self.last_error = None
        self.updated_at = time.time()

    def snapshot(self) -> dict[str, object]:
        return {
            "running": self.running,
            "connected": self.connected,
            "processing": self.processing,
            "people": self.people,
            "risk": self.risk,
            "warning": self.warning,
            "receive_fps": self.receive_fps,
            "inference_fps": self.inference_fps,
            "medium_people": self.thresholds.medium_people,
            "high_people": self.thresholds.high_people,
            "last_error": self.last_error,
            "last_saved": self.last_saved,
            "updated_at": self.updated_at,
            "has_frame": self.latest_frame is not None,
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
```

- [ ] **Step 4: 运行测试确认通过**

Run:

```powershell
.\.venv\Scripts\python.exe -m pytest python_client\tests\test_dashboard_state.py -q
```

Expected: PASS，所有测试通过。

- [ ] **Step 5: 提交**

```powershell
git add python_client/dashboard_state.py python_client/tests/test_dashboard_state.py
git commit -m "Add dashboard state model"
```

---

### Task 3: 新增后端服务编排层

**Files:**
- Create: `python_client/dashboard_service.py`
- Test: `python_client/tests/test_dashboard_service.py`

- [ ] **Step 1: 写失败测试**

创建 `python_client/tests/test_dashboard_service.py`：

```python
import asyncio
import json
from pathlib import Path

import pytest

from python_client.dashboard_service import DashboardService


class FakeWebSocket:
    def __init__(self):
        self.sent: list[str] = []

    async def send(self, message: str) -> None:
        self.sent.append(message)


@pytest.mark.asyncio
async def test_service_start_stop_updates_state():
    service = DashboardService(host="192.168.1.3", model="yolo11n.pt", device="cpu")

    await service.start()
    assert service.state.running is True

    await service.stop()
    assert service.state.running is False
    assert service.state.connected is False


@pytest.mark.asyncio
async def test_service_broadcasts_snapshot_to_browser_clients():
    service = DashboardService(host="192.168.1.3", model="yolo11n.pt", device="cpu")
    browser = FakeWebSocket()

    service.add_browser(browser)
    await service.broadcast()

    payload = json.loads(browser.sent[-1])
    assert payload["type"] == "state"
    assert payload["state"]["running"] is False


@pytest.mark.asyncio
async def test_service_saves_latest_frame(tmp_path: Path):
    service = DashboardService(
        host="192.168.1.3",
        model="yolo11n.pt",
        device="cpu",
        capture_dir=tmp_path,
    )
    service.state.update_frame(
        jpeg=b"frame",
        people=3,
        risk="medium",
        receive_fps=8.0,
        inference_fps=2.0,
        warning="中风险：检测到 3 人",
    )

    result = await service.save_current_frame("manual")

    assert result.image_path.exists()
    assert result.metadata_path.exists()
    assert service.state.last_saved == str(result.image_path)
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
.\.venv\Scripts\python.exe -m pytest python_client\tests\test_dashboard_service.py -q
```

Expected: FAIL，错误包含 `ModuleNotFoundError: No module named 'python_client.dashboard_service'`。

- [ ] **Step 3: 实现服务骨架和可测方法**

创建 `python_client/dashboard_service.py`：

```python
"""Runtime service behind the browser dashboard."""

from __future__ import annotations

import asyncio
import json
import logging
import time
from pathlib import Path
from typing import Any

from .dashboard_state import DashboardState, SaveResult, Thresholds, build_risk_command
from .risk_policy import RiskPolicy
from .video_ai import Detection, PersonDetector, RiskReporter, StreamRate, offer_latest, parse_ack


LOGGER = logging.getLogger("dashboard_service")


class DashboardService:
    def __init__(
        self,
        host: str,
        port: int = 81,
        model: str = "yolo11n.pt",
        confidence: float = 0.4,
        device: str | None = None,
        reconnect_delay: float = 2.0,
        capture_dir: Path | str = "captures",
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
        self._next_command_id = 1

    @property
    def uri(self) -> str:
        return f"ws://{self.host}:{self.port}/video"

    def add_browser(self, websocket: Any) -> None:
        self._browser_clients.add(websocket)

    def remove_browser(self, websocket: Any) -> None:
        self._browser_clients.discard(websocket)

    async def start(self) -> None:
        if self.state.running:
            return
        self._stop_event = asyncio.Event()
        self.state.running = True
        self.state.processing = "starting"
        self.state.warning = "正在连接 ESP32"
        self._runner = asyncio.create_task(self._run_forever())
        await self.broadcast()

    async def stop(self) -> None:
        self._stop_event.set()
        if self._runner:
            self._runner.cancel()
            await asyncio.gather(self._runner, return_exceptions=True)
            self._runner = None
        self.state.running = False
        self.state.connected = False
        self.state.processing = "idle"
        self.state.warning = "已停止"
        await self.broadcast()

    async def update_thresholds(self, medium_people: int, high_people: int) -> None:
        self.state.set_thresholds(Thresholds(medium_people=medium_people, high_people=high_people))
        await self.broadcast()

    async def save_current_frame(self, prefix: str = "capture") -> SaveResult:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        result = self.state.save_latest_frame(self.capture_dir, f"{prefix}-{stamp}")
        await self.broadcast()
        return result

    async def broadcast(self) -> None:
        message = json.dumps(
            {"type": "state", "state": self.state.snapshot()},
            ensure_ascii=False,
            separators=(",", ":"),
        )
        stale: list[Any] = []
        for client in self._browser_clients:
            try:
                await client.send(message)
            except Exception:
                stale.append(client)
        for client in stale:
            self.remove_browser(client)

    async def _run_forever(self) -> None:
        try:
            import websockets

            detector = PersonDetector(self.model, self.confidence, self.device)
            policy = RiskPolicy(required_frames=3)
            reporter = RiskReporter(refresh_seconds=1.0)
            while not self._stop_event.is_set():
                try:
                    self.state.processing = "connecting"
                    await self.broadcast()
                    async with websockets.connect(
                        self.uri,
                        max_size=4 * 1024 * 1024,
                        open_timeout=10,
                        close_timeout=2,
                        ping_interval=20,
                        ping_timeout=20,
                    ) as websocket:
                        self.state.connected = True
                        self.state.processing = "running"
                        self.state.warning = "视频流已连接"
                        reporter.start_connection()
                        await self.broadcast()
                        await self._run_connection(websocket, detector, policy, reporter)
                except asyncio.CancelledError:
                    raise
                except Exception as error:
                    self.state.connected = False
                    self.state.processing = "reconnecting"
                    self.state.last_error = str(error)
                    self.state.warning = "ESP32 连接中断，等待重连"
                    await self.broadcast()
                    try:
                        await asyncio.wait_for(self._stop_event.wait(), timeout=self.reconnect_delay)
                    except asyncio.TimeoutError:
                        pass
        finally:
            self.state.connected = False
            self.state.processing = "idle"

    async def _run_connection(
        self,
        websocket: Any,
        detector: PersonDetector,
        policy: RiskPolicy,
        reporter: RiskReporter,
    ) -> None:
        frames: asyncio.Queue[bytes] = asyncio.Queue(maxsize=1)
        rate = StreamRate()
        receive_task = asyncio.create_task(self._receive_stream(websocket, frames, rate))
        process_task = asyncio.create_task(
            self._process_frames(websocket, frames, rate, detector, policy, reporter)
        )
        done, pending = await asyncio.wait(
            {receive_task, process_task},
            return_when=asyncio.FIRST_COMPLETED,
        )
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)
        for task in done:
            task.result()

    async def _receive_stream(self, websocket: Any, frames: asyncio.Queue[bytes], rate: StreamRate) -> None:
        async for message in websocket:
            if isinstance(message, bytes):
                rate.record_frame(time.monotonic())
                offer_latest(frames, message)
                continue
            ack = parse_ack(message)
            if ack and not ack["applied"]:
                self.state.last_error = f"ESP32 指令拒绝：{ack['error'] or 'unknown'}"
                await self.broadcast()

    async def _process_frames(
        self,
        websocket: Any,
        frames: asyncio.Queue[bytes],
        rate: StreamRate,
        detector: PersonDetector,
        policy: RiskPolicy,
        reporter: RiskReporter,
    ) -> None:
        while not self._stop_event.is_set():
            jpeg = await frames.get()
            started = time.monotonic()
            try:
                detection: Detection = await asyncio.to_thread(detector.detect, jpeg)
            except ValueError as error:
                self.state.last_error = str(error)
                await self.broadcast()
                continue
            elapsed = max(time.monotonic() - started, 1e-6)
            level = self.state.thresholds.classify(detection.people)
            changed = policy.observe(detection.people)
            warning = self._warning_text(level, detection.people)
            encoded = await asyncio.to_thread(self._encode_jpeg, detector, detection.image)
            self.state.update_frame(
                jpeg=encoded,
                people=detection.people,
                risk=level,
                receive_fps=rate.receive_fps,
                inference_fps=1.0 / elapsed,
                warning=warning,
            )
            message = reporter.next_message(level, detection.people, time.monotonic(), changed=changed is not None)
            if message is not None:
                async with self._command_lock:
                    await websocket.send(message)
            await self.broadcast()

    def _encode_jpeg(self, detector: PersonDetector, image: Any) -> bytes:
        ok, encoded = detector.cv2.imencode(".jpg", image, [int(detector.cv2.IMWRITE_JPEG_QUALITY), 85])
        if not ok:
            raise ValueError("failed to encode annotated frame")
        return encoded.tobytes()

    def _warning_text(self, level: str, people: int) -> str:
        if level == "high":
            return f"高风险：检测到 {people} 人"
        if level == "medium":
            return f"中风险：检测到 {people} 人"
        return f"低风险：检测到 {people} 人"
```

- [ ] **Step 4: 运行服务测试**

Run:

```powershell
.\.venv\Scripts\python.exe -m pytest python_client\tests\test_dashboard_service.py -q
```

Expected: PASS。

- [ ] **Step 5: 提交**

```powershell
git add python_client/dashboard_service.py python_client/tests/test_dashboard_service.py
git commit -m "Add dashboard service layer"
```

---

### Task 4: 新增 FastAPI 应用接口

**Files:**
- Create: `python_client/dashboard_app.py`
- Test: `python_client/tests/test_dashboard_app.py`

- [ ] **Step 1: 写失败测试**

创建 `python_client/tests/test_dashboard_app.py`：

```python
from pathlib import Path

from fastapi.testclient import TestClient

from python_client.dashboard_app import create_app


def test_index_page_loads():
    app = create_app(host="192.168.1.3", capture_dir=Path("captures"))
    client = TestClient(app)

    response = client.get("/")

    assert response.status_code == 200
    assert "video-ai" in response.text


def test_state_endpoint_returns_snapshot():
    app = create_app(host="192.168.1.3", capture_dir=Path("captures"))
    client = TestClient(app)

    response = client.get("/api/state")

    assert response.status_code == 200
    assert response.json()["running"] is False


def test_threshold_endpoint_validates_and_updates_state():
    app = create_app(host="192.168.1.3", capture_dir=Path("captures"))
    client = TestClient(app)

    response = client.post("/api/thresholds", json={"medium_people": 2, "high_people": 5})

    assert response.status_code == 200
    assert response.json()["medium_people"] == 2
    assert response.json()["high_people"] == 5


def test_threshold_endpoint_rejects_invalid_order():
    app = create_app(host="192.168.1.3", capture_dir=Path("captures"))
    client = TestClient(app)

    response = client.post("/api/thresholds", json={"medium_people": 5, "high_people": 5})

    assert response.status_code == 400
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
.\.venv\Scripts\python.exe -m pytest python_client\tests\test_dashboard_app.py -q
```

Expected: FAIL，错误包含 `ModuleNotFoundError: No module named 'python_client.dashboard_app'`。

- [ ] **Step 3: 实现 FastAPI 应用**

创建 `python_client/dashboard_app.py`：

```python
"""FastAPI app for the browser dashboard."""

from __future__ import annotations

from pathlib import Path

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from .dashboard_service import DashboardService
from .dashboard_state import Thresholds


STATIC_DIR = Path(__file__).with_name("static")


class ThresholdPayload(BaseModel):
    medium_people: int
    high_people: int


def create_app(
    host: str,
    port: int = 81,
    model: str = "yolo11n.pt",
    confidence: float = 0.4,
    device: str | None = None,
    capture_dir: Path | str = "captures",
) -> FastAPI:
    app = FastAPI(title="video-ai dashboard")
    service = DashboardService(
        host=host,
        port=port,
        model=model,
        confidence=confidence,
        device=device,
        capture_dir=capture_dir,
    )
    app.state.dashboard_service = service
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

    @app.get("/", response_class=HTMLResponse)
    async def index() -> str:
        return (STATIC_DIR / "dashboard.html").read_text(encoding="utf-8")

    @app.get("/api/state")
    async def state() -> dict[str, object]:
        return service.state.snapshot()

    @app.post("/api/start")
    async def start() -> dict[str, object]:
        await service.start()
        return service.state.snapshot()

    @app.post("/api/stop")
    async def stop() -> dict[str, object]:
        await service.stop()
        return service.state.snapshot()

    @app.post("/api/thresholds")
    async def thresholds(payload: ThresholdPayload) -> dict[str, object]:
        try:
            validated = Thresholds(
                medium_people=payload.medium_people,
                high_people=payload.high_people,
            )
        except ValueError as error:
            raise HTTPException(status_code=400, detail=str(error)) from error
        await service.update_thresholds(validated.medium_people, validated.high_people)
        return service.state.snapshot()

    @app.post("/api/screenshot")
    async def screenshot() -> dict[str, str]:
        try:
            result = await service.save_current_frame("screenshot")
        except RuntimeError as error:
            raise HTTPException(status_code=409, detail=str(error)) from error
        return {"image_path": str(result.image_path), "metadata_path": str(result.metadata_path)}

    @app.post("/api/save")
    async def save() -> dict[str, str]:
        try:
            result = await service.save_current_frame("save")
        except RuntimeError as error:
            raise HTTPException(status_code=409, detail=str(error)) from error
        return {"image_path": str(result.image_path), "metadata_path": str(result.metadata_path)}

    @app.get("/frame/latest.jpg")
    async def latest_frame() -> Response:
        frame = service.state.latest_frame
        if frame is None:
            raise HTTPException(status_code=404, detail="no frame available")
        return Response(content=frame, media_type="image/jpeg")

    @app.websocket("/ws/state")
    async def state_socket(websocket: WebSocket) -> None:
        await websocket.accept()
        service.add_browser(websocket)
        try:
            await service.broadcast()
            while True:
                await websocket.receive_text()
        except WebSocketDisconnect:
            service.remove_browser(websocket)

    return app
```

- [ ] **Step 4: 运行接口测试**

Run:

```powershell
.\.venv\Scripts\python.exe -m pytest python_client\tests\test_dashboard_app.py -q
```

Expected: PASS。

- [ ] **Step 5: 提交**

```powershell
git add python_client/dashboard_app.py python_client/tests/test_dashboard_app.py
git commit -m "Add dashboard FastAPI app"
```

---

### Task 5: 新增浏览器单页界面

**Files:**
- Create: `python_client/static/dashboard.html`
- Create: `python_client/static/dashboard.css`
- Create: `python_client/static/dashboard.js`

- [ ] **Step 1: 创建 HTML**

创建 `python_client/static/dashboard.html`：

```html
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>video-ai</title>
  <link rel="stylesheet" href="/static/dashboard.css">
</head>
<body>
  <main class="dashboard">
    <header class="topbar">
      <div>
        <h1>video-ai</h1>
        <p id="subtitle">ESP32-S3 CAM 人员检测仪表盘</p>
      </div>
      <div class="status-row">
        <span id="connectionBadge" class="badge off">未连接</span>
        <span id="processingBadge" class="badge neutral">idle</span>
      </div>
    </header>

    <section class="content">
      <section class="video-panel">
        <div id="warningBanner" class="warning low">等待启动</div>
        <img id="videoFrame" alt="实时视频画面">
        <div id="emptyState" class="empty-state">暂无视频帧</div>
      </section>

      <aside class="control-panel">
        <section>
          <h2>连接控制</h2>
          <button id="startButton" class="primary">启动</button>
          <button id="stopButton">停止</button>
        </section>

        <section>
          <h2>风险阈值</h2>
          <label>
            中风险人数
            <input id="mediumInput" type="number" min="1" value="3">
          </label>
          <label>
            高风险人数
            <input id="highInput" type="number" min="2" value="6">
          </label>
          <button id="thresholdButton">应用阈值</button>
        </section>

        <section>
          <h2>图像保存</h2>
          <button id="screenshotButton">截图</button>
          <button id="saveButton">保存</button>
          <p id="savePath" class="muted">尚未保存</p>
        </section>
      </aside>
    </section>

    <footer class="status-strip">
      <div><span>人数</span><strong id="peopleValue">0</strong></div>
      <div><span>风险</span><strong id="riskValue">low</strong></div>
      <div><span>接收 FPS</span><strong id="rxFpsValue">0.0</strong></div>
      <div><span>推理 FPS</span><strong id="aiFpsValue">0.0</strong></div>
      <div><span>最后更新</span><strong id="updatedValue">--</strong></div>
    </footer>
  </main>
  <script src="/static/dashboard.js"></script>
</body>
</html>
```

- [ ] **Step 2: 创建 CSS**

创建 `python_client/static/dashboard.css`：

```css
:root {
  color-scheme: dark;
  --bg: #111827;
  --panel: #172033;
  --panel-2: #1f2937;
  --text: #f9fafb;
  --muted: #9ca3af;
  --border: #374151;
  --blue: #38bdf8;
  --green: #22c55e;
  --yellow: #f59e0b;
  --red: #ef4444;
}

* { box-sizing: border-box; }

body {
  margin: 0;
  min-height: 100vh;
  background: var(--bg);
  color: var(--text);
  font-family: "Noto Sans SC", "Microsoft YaHei", Arial, sans-serif;
}

.dashboard {
  min-height: 100vh;
  display: grid;
  grid-template-rows: auto 1fr auto;
  gap: 12px;
  padding: 16px;
}

.topbar,
.status-strip,
.video-panel,
.control-panel {
  border: 1px solid var(--border);
  background: var(--panel);
  border-radius: 8px;
}

.topbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 14px 16px;
}

h1, h2, p { margin: 0; }
h1 { font-size: 22px; }
h2 { font-size: 15px; margin-bottom: 10px; }

.status-row { display: flex; gap: 8px; }
.badge {
  display: inline-flex;
  min-width: 72px;
  justify-content: center;
  padding: 6px 10px;
  border-radius: 999px;
  font-size: 13px;
  border: 1px solid var(--border);
}
.badge.on { color: #dcfce7; border-color: var(--green); }
.badge.off { color: #fee2e2; border-color: var(--red); }
.badge.neutral { color: #e0f2fe; border-color: var(--blue); }

.content {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 320px;
  gap: 12px;
  min-height: 0;
}

.video-panel {
  position: relative;
  min-height: 420px;
  display: grid;
  place-items: center;
  overflow: hidden;
  background: #050b16;
}

#videoFrame {
  width: 100%;
  height: 100%;
  object-fit: contain;
  display: none;
}

.empty-state {
  color: var(--muted);
}

.warning {
  position: absolute;
  top: 12px;
  left: 12px;
  right: 12px;
  z-index: 1;
  padding: 10px 12px;
  border-radius: 6px;
  background: rgba(17, 24, 39, 0.86);
  border: 1px solid var(--border);
  font-weight: 600;
}
.warning.low { border-color: var(--green); }
.warning.medium { border-color: var(--yellow); }
.warning.high { border-color: var(--red); }

.control-panel {
  padding: 14px;
  display: grid;
  align-content: start;
  gap: 18px;
}

.control-panel section {
  display: grid;
  gap: 10px;
}

label {
  display: grid;
  gap: 6px;
  color: var(--muted);
  font-size: 13px;
}

input, button {
  width: 100%;
  min-height: 38px;
  border-radius: 6px;
  border: 1px solid var(--border);
  background: var(--panel-2);
  color: var(--text);
  font: inherit;
}

button {
  cursor: pointer;
  transition: border-color 160ms ease, background 160ms ease;
}
button:hover { border-color: var(--blue); }
button.primary { background: #075985; border-color: var(--blue); }

.muted {
  color: var(--muted);
  font-size: 13px;
  word-break: break-all;
}

.status-strip {
  display: grid;
  grid-template-columns: repeat(5, 1fr);
  gap: 1px;
  overflow: hidden;
}
.status-strip div {
  padding: 12px;
  background: var(--panel-2);
}
.status-strip span {
  display: block;
  color: var(--muted);
  font-size: 12px;
}
.status-strip strong {
  display: block;
  margin-top: 4px;
  font-size: 18px;
}

@media (max-width: 900px) {
  .content { grid-template-columns: 1fr; }
  .status-strip { grid-template-columns: repeat(2, 1fr); }
}
```

- [ ] **Step 3: 创建 JavaScript**

创建 `python_client/static/dashboard.js`：

```javascript
const elements = {
  connectionBadge: document.getElementById("connectionBadge"),
  processingBadge: document.getElementById("processingBadge"),
  warningBanner: document.getElementById("warningBanner"),
  videoFrame: document.getElementById("videoFrame"),
  emptyState: document.getElementById("emptyState"),
  startButton: document.getElementById("startButton"),
  stopButton: document.getElementById("stopButton"),
  mediumInput: document.getElementById("mediumInput"),
  highInput: document.getElementById("highInput"),
  thresholdButton: document.getElementById("thresholdButton"),
  screenshotButton: document.getElementById("screenshotButton"),
  saveButton: document.getElementById("saveButton"),
  savePath: document.getElementById("savePath"),
  peopleValue: document.getElementById("peopleValue"),
  riskValue: document.getElementById("riskValue"),
  rxFpsValue: document.getElementById("rxFpsValue"),
  aiFpsValue: document.getElementById("aiFpsValue"),
  updatedValue: document.getElementById("updatedValue"),
};

function formatTime(seconds) {
  if (!seconds) return "--";
  return new Date(seconds * 1000).toLocaleTimeString();
}

function renderState(state) {
  elements.connectionBadge.textContent = state.connected ? "已连接" : "未连接";
  elements.connectionBadge.className = `badge ${state.connected ? "on" : "off"}`;
  elements.processingBadge.textContent = state.processing;

  elements.warningBanner.textContent = state.warning || "等待状态";
  elements.warningBanner.className = `warning ${state.risk || "low"}`;

  elements.peopleValue.textContent = state.people;
  elements.riskValue.textContent = state.risk;
  elements.rxFpsValue.textContent = Number(state.receive_fps || 0).toFixed(1);
  elements.aiFpsValue.textContent = Number(state.inference_fps || 0).toFixed(1);
  elements.updatedValue.textContent = formatTime(state.updated_at);
  elements.mediumInput.value = state.medium_people;
  elements.highInput.value = state.high_people;

  if (state.last_saved) {
    elements.savePath.textContent = state.last_saved;
  }

  if (state.has_frame) {
    elements.videoFrame.src = `/frame/latest.jpg?t=${Date.now()}`;
    elements.videoFrame.style.display = "block";
    elements.emptyState.style.display = "none";
  } else {
    elements.videoFrame.style.display = "none";
    elements.emptyState.style.display = "block";
  }
}

async function postJson(url, body) {
  const response = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined,
  });
  if (!response.ok) {
    const error = await response.json().catch(() => ({ detail: response.statusText }));
    throw new Error(error.detail || response.statusText);
  }
  return response.json();
}

async function postAction(url) {
  try {
    const state = await postJson(url);
    renderState(state);
  } catch (error) {
    elements.savePath.textContent = error.message;
  }
}

function connectStateSocket() {
  const socket = new WebSocket(`ws://${location.host}/ws/state`);
  socket.onmessage = (event) => {
    const payload = JSON.parse(event.data);
    if (payload.type === "state") {
      renderState(payload.state);
    }
  };
  socket.onclose = () => setTimeout(connectStateSocket, 1000);
}

elements.startButton.addEventListener("click", () => postAction("/api/start"));
elements.stopButton.addEventListener("click", () => postAction("/api/stop"));
elements.thresholdButton.addEventListener("click", async () => {
  try {
    const state = await postJson("/api/thresholds", {
      medium_people: Number(elements.mediumInput.value),
      high_people: Number(elements.highInput.value),
    });
    renderState(state);
  } catch (error) {
    elements.savePath.textContent = error.message;
  }
});
elements.screenshotButton.addEventListener("click", () => postAction("/api/screenshot"));
elements.saveButton.addEventListener("click", () => postAction("/api/save"));

fetch("/api/state")
  .then((response) => response.json())
  .then(renderState);
connectStateSocket();
```

- [ ] **Step 4: 静态资源接口冒烟测试**

Run:

```powershell
.\.venv\Scripts\python.exe -m pytest python_client\tests\test_dashboard_app.py::test_index_page_loads -q
```

Expected: PASS。

- [ ] **Step 5: 提交**

```powershell
git add python_client/static/dashboard.html python_client/static/dashboard.css python_client/static/dashboard.js
git commit -m "Add browser dashboard UI"
```

---

### Task 6: 新增仪表盘启动入口

**Files:**
- Create: `python_client/dashboard.py`
- Modify: `python_client/README.md`

- [ ] **Step 1: 创建启动入口**

创建 `python_client/dashboard.py`：

```python
"""Run the browser dashboard for video-ai."""

from __future__ import annotations

import argparse
import logging
from pathlib import Path

import uvicorn

from .dashboard_app import create_app


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp32-host", required=True, help="ESP32 IPv4 address")
    parser.add_argument("--esp32-port", type=int, default=81, help="ESP32 WebSocket port")
    parser.add_argument("--listen-host", default="127.0.0.1", help="Dashboard listen host")
    parser.add_argument("--listen-port", type=int, default=8000, help="Dashboard listen port")
    parser.add_argument("--model", default="yolo11n.pt", help="Ultralytics model path")
    parser.add_argument("--confidence", type=float, default=0.4, help="Detection confidence")
    parser.add_argument("--device", default=None, help="Ultralytics device, for example 0 or cpu")
    parser.add_argument("--capture-dir", default="captures", help="Directory for screenshots and saves")
    args = parser.parse_args()
    if not 1 <= args.esp32_port <= 65535:
        parser.error("--esp32-port must be in [1, 65535]")
    if not 1 <= args.listen_port <= 65535:
        parser.error("--listen-port must be in [1, 65535]")
    if not 0.0 < args.confidence <= 1.0:
        parser.error("--confidence must be in (0, 1]")
    return args


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    args = parse_args()
    app = create_app(
        host=args.esp32_host,
        port=args.esp32_port,
        model=args.model,
        confidence=args.confidence,
        device=args.device,
        capture_dir=Path(args.capture_dir),
    )
    uvicorn.run(app, host=args.listen_host, port=args.listen_port)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: 更新 README**

在 `python_client/README.md` 的运行部分增加：

```markdown
## 浏览器仪表盘

启动本地 Web 仪表盘：

```powershell
.\.venv\Scripts\python.exe -m python_client.dashboard --esp32-host 192.168.1.3 --device cpu
```

浏览器打开：

```text
http://127.0.0.1:8000
```

页面中的“启动”按钮会让 Python 服务连接 ESP32 的 `ws://192.168.1.3:81/video`。浏览器不直接连接 ESP32。截图和保存文件默认写入 `captures` 目录。
```

- [ ] **Step 3: 编译检查**

Run:

```powershell
.\.venv\Scripts\python.exe -m py_compile python_client\dashboard.py python_client\dashboard_app.py python_client\dashboard_service.py python_client\dashboard_state.py
```

Expected: 退出码 `0`。

- [ ] **Step 4: 提交**

```powershell
git add python_client/dashboard.py python_client/README.md
git commit -m "Add dashboard launch command"
```

---

### Task 7: 全量验证和手动联调

**Files:**
- No code changes expected unless verification finds a defect.

- [ ] **Step 1: 运行 Python 单元测试**

Run:

```powershell
.\.venv\Scripts\python.exe -m pytest python_client\tests -q
```

Expected: 全部 PASS。

- [ ] **Step 2: 运行 Python 编译检查**

Run:

```powershell
.\.venv\Scripts\python.exe -m py_compile python_client\risk_policy.py python_client\video_ai.py python_client\dashboard.py python_client\dashboard_app.py python_client\dashboard_service.py python_client\dashboard_state.py
```

Expected: 退出码 `0`。

- [ ] **Step 3: 启动仪表盘**

Run:

```powershell
.\.venv\Scripts\python.exe -m python_client.dashboard --esp32-host 192.168.1.3 --device cpu
```

Expected: 终端显示 Uvicorn 正在监听 `http://127.0.0.1:8000`。

- [ ] **Step 4: 浏览器手动验证**

打开：

```text
http://127.0.0.1:8000
```

Expected:

- 页面显示 `video-ai` 仪表盘。
- 点击 `启动` 后连接状态从 `未连接` 变为 `已连接`。
- 主视频区域出现实时标注画面。
- 人数、风险、接收 FPS、推理 FPS 会更新。
- 调整阈值后风险等级按新阈值变化。
- ESP32 WS2812 会根据低、中、高风险显示对应灯效。
- 点击 `截图` 和 `保存` 后，`captures` 目录出现 `.jpg` 和 `.json` 文件。

- [ ] **Step 5: 提交联调修正**

如果手动联调发现必须修正的问题，按实际文件提交：

```powershell
git add python_client
git commit -m "Fix dashboard integration issues"
```

如果没有修正，不需要提交。

---

## 自查结果

- 规格覆盖：浏览器页面、连接状态、实时预警、阈值输入、启动、停止、截图、保存、Python 接 ESP32、YOLO 分析、指令下发、WS2812 灯效和错误处理都有对应任务。
- 协议边界：沿用现有 ESP32 风险 JSON 指令，不扩展 ESP32 固件协议。
- 测试覆盖：纯状态模型、FastAPI 接口和服务编排都有测试入口；真实摄像头和 WS2812 通过最后的手动联调验证。
- 兼容性：保留现有 `python_client.video_ai`，新增 `python_client.dashboard`，不破坏已有 OpenCV 预览流程。
